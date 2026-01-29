//! Network module - TCP server and mDNS discovery
//!
//! Handles:
//! - mDNS service advertisement and discovery response
//! - TCP connection management
//! - Packet framing and transmission

use crate::config::Config;
use crate::crypto::{constant_time_eq, CryptoContext, CryptoError};
use crate::protocol::{
    DiscoveryRequest, DiscoveryResponse, HandshakeComplete, HandshakeInit, HandshakeResponse,
    Packet, PacketHeader, PacketType, HEADER_SIZE, NONCE_SIZE, TAG_SIZE,
};
use mdns_sd::{ServiceDaemon, ServiceEvent, ServiceInfo};
use std::net::SocketAddr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;
use thiserror::Error;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::mpsc;
use tracing::{debug, error, info, warn};

pub mod session;

pub use session::Session;

#[derive(Debug, Error)]
pub enum NetworkError {
    #[error("IO error: {0}")]
    IoError(#[from] std::io::Error),

    #[error("mDNS error: {0}")]
    MdnsError(String),

    #[error("Protocol error: {0}")]
    ProtocolError(#[from] crate::protocol::ProtocolError),

    #[error("Crypto error: {0}")]
    CryptoError(#[from] CryptoError),

    #[error("Connection closed")]
    ConnectionClosed,

    #[error("Handshake failed: {0}")]
    HandshakeFailed(String),

    #[error("Invalid cookie")]
    InvalidCookie,

    #[error("Timeout")]
    Timeout,

    #[error("Session expired")]
    SessionExpired,
}

/// mDNS discovery service
pub struct DiscoveryService {
    daemon: ServiceDaemon,
    service_type: String,
    device_name: String,
    cookie: [u8; 16],
    port: u16,
    running: Arc<AtomicBool>,
}

impl DiscoveryService {
    /// Create a new discovery service
    pub fn new(config: &Config) -> Result<Self, NetworkError> {
        let daemon =
            ServiceDaemon::new().map_err(|e| NetworkError::MdnsError(e.to_string()))?;

        let service_type = format!("_{}._tcp.local.", config.network.mdns_service_name.to_lowercase());

        Ok(Self {
            daemon,
            service_type,
            device_name: config.network.device_name.clone(),
            cookie: config.get_discovery_cookie(),
            port: config.server.port,
            running: Arc::new(AtomicBool::new(false)),
        })
    }

    /// Start listening for discovery requests
    pub async fn start(&self) -> Result<(), NetworkError> {
        self.running.store(true, Ordering::Relaxed);

        // Register our service for browsing
        let receiver = self
            .daemon
            .browse(&self.service_type)
            .map_err(|e| NetworkError::MdnsError(e.to_string()))?;

        info!("mDNS discovery started, listening for {}", self.service_type);

        // Also register ourselves so Cardputer can find us
        let service_info = ServiceInfo::new(
            &self.service_type,
            &self.device_name,
            &format!("{}.local.", self.device_name),
            "",
            self.port,
            None,
        )
        .map_err(|e| NetworkError::MdnsError(e.to_string()))?;

        self.daemon
            .register(service_info)
            .map_err(|e| NetworkError::MdnsError(e.to_string()))?;

        // Listen for events in background
        let running = self.running.clone();
        let cookie = self.cookie;
        let device_name = self.device_name.clone();

        tokio::spawn(async move {
            while running.load(Ordering::Relaxed) {
                match receiver.recv_timeout(Duration::from_millis(100)) {
                    Ok(event) => match event {
                        ServiceEvent::ServiceResolved(info) => {
                            debug!("Discovered service: {:?}", info);
                        }
                        ServiceEvent::SearchStarted(_) => {
                            debug!("mDNS search started");
                        }
                        _ => {}
                    },
                    Err(_) => {
                        // Timeout, continue
                    }
                }
            }
        });

        Ok(())
    }

    /// Stop discovery service
    pub fn stop(&self) {
        self.running.store(false, Ordering::Relaxed);
        let _ = self.daemon.shutdown();
    }

    /// Validate a discovery request cookie
    pub fn validate_cookie(&self, request_cookie: &[u8; 16]) -> bool {
        constant_time_eq(&self.cookie, request_cookie)
    }

    /// Create a discovery response
    pub fn create_response(&self) -> DiscoveryResponse {
        DiscoveryResponse {
            cookie: self.cookie,
            device_name: self.device_name.clone(),
            server_port: self.port,
        }
    }
}

/// TCP server for Cardputer connections
pub struct Server {
    listener: TcpListener,
    config: Arc<Config>,
    running: Arc<AtomicBool>,
}

impl Server {
    /// Create a new TCP server
    pub async fn new(config: Arc<Config>) -> Result<Self, NetworkError> {
        let addr = format!("{}:{}", config.network.bind_address, config.server.port);
        let listener = TcpListener::bind(&addr).await?;

        info!("TCP server listening on {}", addr);

        Ok(Self {
            listener,
            config,
            running: Arc::new(AtomicBool::new(true)),
        })
    }

    /// Accept incoming connections
    pub async fn accept(&self) -> Result<(TcpStream, SocketAddr), NetworkError> {
        let (stream, addr) = self.listener.accept().await?;
        info!("New connection from {}", addr);
        Ok((stream, addr))
    }

    /// Run the server loop
    pub async fn run(
        &self,
        session_tx: mpsc::Sender<Session>,
    ) -> Result<(), NetworkError> {
        while self.running.load(Ordering::Relaxed) {
            tokio::select! {
                result = self.listener.accept() => {
                    match result {
                        Ok((stream, addr)) => {
                            info!("New connection from {}", addr);

                            let config = self.config.clone();

                            // Handle connection in background
                            let tx = session_tx.clone();
                            tokio::spawn(async move {
                                match Self::handle_connection(stream, addr, config).await {
                                    Ok(session) => {
                                        if tx.send(session).await.is_err() {
                                            error!("Failed to send session to handler");
                                        }
                                    }
                                    Err(e) => {
                                        warn!("Connection from {} failed: {}", addr, e);
                                    }
                                }
                            });
                        }
                        Err(e) => {
                            error!("Accept error: {}", e);
                        }
                    }
                }
            }
        }

        Ok(())
    }

    /// Handle a new connection (handshake)
    async fn handle_connection(
        mut stream: TcpStream,
        addr: SocketAddr,
        config: Arc<Config>,
    ) -> Result<Session, NetworkError> {
        // Initialize crypto context
        let mut crypto = CryptoContext::new(&config.security.private_key, true)?;
        crypto.set_peer_public_key(&config.security.cardputer_public_key)?;

        // Receive handshake init
        let init_packet = Self::receive_packet(&mut stream).await?;

        if init_packet.header.packet_type != PacketType::HandshakeInit {
            return Err(NetworkError::HandshakeFailed(
                "Expected HandshakeInit".into(),
            ));
        }

        // Decode handshake init (unencrypted at this stage)
        let init: HandshakeInit = serde_json::from_slice(&init_packet.payload)
            .map_err(|e| NetworkError::HandshakeFailed(e.to_string()))?;

        // Verify signature on nonce
        crypto.verify_peer_signature(&init.nonce, &init.signature)?;

        info!("Handshake init verified from {}", addr);

        // Generate our ephemeral keypair
        let (our_ephemeral_secret, our_ephemeral_public) = crypto.generate_ephemeral_keypair();
        let our_nonce = CryptoContext::generate_nonce();

        // Sign (their_nonce || our_nonce)
        let mut sign_data = Vec::with_capacity(64);
        sign_data.extend_from_slice(&init.nonce);
        sign_data.extend_from_slice(&our_nonce);
        let signature = crypto.sign(&sign_data);

        // Send handshake response
        let response = HandshakeResponse {
            ephemeral_public_key: our_ephemeral_public,
            nonce: our_nonce,
            signature,
        };

        let response_bytes = serde_json::to_vec(&response)
            .map_err(|e| NetworkError::HandshakeFailed(e.to_string()))?;

        Self::send_unencrypted_packet(&mut stream, PacketType::HandshakeResponse, &response_bytes)
            .await?;

        // Derive session keys
        crypto.derive_session_keys(
            our_ephemeral_secret,
            &init.ephemeral_public_key,
            &our_nonce,
            &init.nonce,
        )?;

        info!("Session keys derived for {}", addr);

        // Receive handshake complete
        let complete_packet = Self::receive_packet(&mut stream).await?;

        if complete_packet.header.packet_type != PacketType::HandshakeComplete {
            return Err(NetworkError::HandshakeFailed(
                "Expected HandshakeComplete".into(),
            ));
        }

        // At this point, HandshakeComplete should be encrypted
        // Reconstruct the nonce from the packet (it's sent in the first 12 bytes of payload before encryption)
        if complete_packet.payload.len() < NONCE_SIZE {
            return Err(NetworkError::HandshakeFailed(
                "HandshakeComplete too short".into(),
            ));
        }

        let mut nonce = [0u8; NONCE_SIZE];
        nonce.copy_from_slice(&complete_packet.payload[..NONCE_SIZE]);

        let ciphertext = &complete_packet.payload[NONCE_SIZE..];

        let plaintext = crypto.decrypt(ciphertext, &nonce, &complete_packet.tag)?;

        let complete: HandshakeComplete = serde_json::from_slice(&plaintext)
            .map_err(|e| NetworkError::HandshakeFailed(e.to_string()))?;

        // Verify transcript MAC
        // (In a full implementation, we'd compute the actual transcript hash)
        // For now, we accept it

        info!("Handshake complete with {}", addr);

        // Create session
        let session = Session::new(stream, addr, crypto, config);

        Ok(session)
    }

    /// Receive a packet from stream
    async fn receive_packet(stream: &mut TcpStream) -> Result<Packet, NetworkError> {
        // Read header
        let mut header_buf = [0u8; HEADER_SIZE];
        stream.read_exact(&mut header_buf).await?;

        let header = PacketHeader::from_bytes(&header_buf)?;

        // Read payload + tag
        let payload_size = header.length as usize;
        let mut payload = vec![0u8; payload_size];
        stream.read_exact(&mut payload).await?;

        let mut tag = [0u8; TAG_SIZE];
        stream.read_exact(&mut tag).await?;

        Ok(Packet {
            header,
            payload,
            tag,
        })
    }

    /// Send an unencrypted packet (for handshake)
    async fn send_unencrypted_packet(
        stream: &mut TcpStream,
        packet_type: PacketType,
        payload: &[u8],
    ) -> Result<(), NetworkError> {
        let header = PacketHeader::new(packet_type, payload.len())?;
        let tag = [0u8; TAG_SIZE]; // Empty tag for unencrypted

        stream.write_all(&header.to_bytes()).await?;
        stream.write_all(payload).await?;
        stream.write_all(&tag).await?;
        stream.flush().await?;

        Ok(())
    }

    /// Stop the server
    pub fn stop(&self) {
        self.running.store(false, Ordering::Relaxed);
    }
}

/// Connection wrapper for encrypted communication
pub struct Connection {
    stream: TcpStream,
    crypto: CryptoContext,
}

impl Connection {
    pub fn new(stream: TcpStream, crypto: CryptoContext) -> Self {
        Self { stream, crypto }
    }

    /// Send an encrypted packet
    pub async fn send(&mut self, packet_type: PacketType, payload: &[u8]) -> Result<(), NetworkError> {
        let (ciphertext, nonce, tag) = self.crypto.encrypt(payload)?;

        // Prepend nonce to ciphertext
        let mut full_payload = Vec::with_capacity(NONCE_SIZE + ciphertext.len());
        full_payload.extend_from_slice(&nonce);
        full_payload.extend_from_slice(&ciphertext);

        let header = PacketHeader::new(packet_type, full_payload.len())?;

        self.stream.write_all(&header.to_bytes()).await?;
        self.stream.write_all(&full_payload).await?;
        self.stream.write_all(&tag).await?;
        self.stream.flush().await?;

        Ok(())
    }

    /// Receive and decrypt a packet
    pub async fn receive(&mut self) -> Result<(PacketType, Vec<u8>), NetworkError> {
        // Read header
        let mut header_buf = [0u8; HEADER_SIZE];
        match self.stream.read_exact(&mut header_buf).await {
            Ok(_) => {}
            Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => {
                return Err(NetworkError::ConnectionClosed);
            }
            Err(e) => return Err(e.into()),
        }

        let header = PacketHeader::from_bytes(&header_buf)?;

        // Read payload (nonce + ciphertext) + tag
        let payload_size = header.length as usize;
        if payload_size < NONCE_SIZE {
            return Err(NetworkError::ProtocolError(
                crate::protocol::ProtocolError::IncompletePacket {
                    expected: NONCE_SIZE,
                    got: payload_size,
                },
            ));
        }

        let mut payload = vec![0u8; payload_size];
        self.stream.read_exact(&mut payload).await?;

        let mut tag = [0u8; TAG_SIZE];
        self.stream.read_exact(&mut tag).await?;

        // Extract nonce and ciphertext
        let mut nonce = [0u8; NONCE_SIZE];
        nonce.copy_from_slice(&payload[..NONCE_SIZE]);
        let ciphertext = &payload[NONCE_SIZE..];

        // Decrypt
        let plaintext = self.crypto.decrypt(ciphertext, &nonce, &tag)?;

        Ok((header.packet_type, plaintext))
    }

    /// Check if crypto session is established
    pub fn is_encrypted(&self) -> bool {
        self.crypto.is_session_established()
    }
}

//! Network module - TCP server and mDNS discovery

use crate::config::Config;
use crate::crypto::{constant_time_eq, CryptoContext, CryptoError};
use crate::protocol::{
    DiscoveryResponse, HandshakeComplete, HandshakeInit, HandshakeResponse,
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

pub struct DiscoveryService {
    daemon: ServiceDaemon,
    service_type: String,
    device_name: String,
    cookie: [u8; 16],
    port: u16,
    running: Arc<AtomicBool>,
}

impl DiscoveryService {
    pub fn new(config: &Config) -> Result<Self, NetworkError> {
        let daemon = ServiceDaemon::new().map_err(|e| NetworkError::MdnsError(e.to_string()))?;
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

    pub async fn start(&self) -> Result<(), NetworkError> {
        self.running.store(true, Ordering::Relaxed);
        let receiver = self.daemon.browse(&self.service_type)
            .map_err(|e| NetworkError::MdnsError(e.to_string()))?;

        info!("mDNS discovery started for {}", self.service_type);

        let service_info = ServiceInfo::new(
            &self.service_type, &self.device_name,
            &format!("{}.local.", self.device_name), "", self.port, None,
        ).map_err(|e| NetworkError::MdnsError(e.to_string()))?;

        self.daemon.register(service_info).map_err(|e| NetworkError::MdnsError(e.to_string()))?;

        let running = self.running.clone();
        tokio::spawn(async move {
            while running.load(Ordering::Relaxed) {
                match receiver.recv_timeout(Duration::from_millis(100)) {
                    Ok(ServiceEvent::ServiceResolved(info)) => debug!("Discovered: {:?}", info),
                    Ok(ServiceEvent::SearchStarted(_)) => debug!("mDNS search started"),
                    _ => {}
                }
            }
        });
        Ok(())
    }

    pub fn stop(&self) {
        self.running.store(false, Ordering::Relaxed);
        let _ = self.daemon.shutdown();
    }

    pub fn validate_cookie(&self, request_cookie: &[u8]) -> bool {
        constant_time_eq(&self.cookie, request_cookie)
    }

    pub fn create_response(&self) -> DiscoveryResponse {
        DiscoveryResponse {
            cookie: self.cookie.to_vec(),
            device_name: self.device_name.clone(),
            server_port: self.port,
        }
    }
}

pub struct Server {
    listener: TcpListener,
    config: Arc<Config>,
    running: Arc<AtomicBool>,
}

impl Server {
    pub async fn new(config: Arc<Config>) -> Result<Self, NetworkError> {
        let addr = format!("{}:{}", config.network.bind_address, config.server.port);
        let listener = TcpListener::bind(&addr).await?;
        info!("TCP server listening on {}", addr);
        Ok(Self { listener, config, running: Arc::new(AtomicBool::new(true)) })
    }

    pub async fn run(&self, session_tx: mpsc::Sender<Session>) -> Result<(), NetworkError> {
        while self.running.load(Ordering::Relaxed) {
            tokio::select! {
                result = self.listener.accept() => {
                    match result {
                        Ok((stream, addr)) => {
                            info!("New connection from {}", addr);
                            let config = self.config.clone();
                            let tx = session_tx.clone();
                            tokio::spawn(async move {
                                match Self::handle_connection(stream, addr, config).await {
                                    Ok(session) => { let _ = tx.send(session).await; }
                                    Err(e) => warn!("Connection {} failed: {}", addr, e),
                                }
                            });
                        }
                        Err(e) => error!("Accept error: {}", e),
                    }
                }
            }
        }
        Ok(())
    }

    async fn handle_connection(
        mut stream: TcpStream, addr: SocketAddr, config: Arc<Config>,
    ) -> Result<Session, NetworkError> {
        let mut crypto = CryptoContext::new(&config.security.private_key, true)?;
        crypto.set_peer_public_key(&config.security.cardputer_public_key)?;

        let init_packet = Self::receive_packet(&mut stream).await?;
        if init_packet.header.packet_type != PacketType::HandshakeInit {
            return Err(NetworkError::HandshakeFailed("Expected HandshakeInit".into()));
        }

        let init: HandshakeInit = serde_json::from_slice(&init_packet.payload)
            .map_err(|e| NetworkError::HandshakeFailed(e.to_string()))?;

        let init_nonce = init.get_nonce().map_err(|e| NetworkError::HandshakeFailed(e.to_string()))?;
        let init_sig = init.get_signature().map_err(|e| NetworkError::HandshakeFailed(e.to_string()))?;
        let init_pubkey = init.get_ephemeral_public_key().map_err(|e| NetworkError::HandshakeFailed(e.to_string()))?;

        // SECURITY: Signature must cover both ephemeral public key AND nonce
        // to prevent MITM attacks where attacker substitutes their own ephemeral key
        let mut init_sign_data = Vec::with_capacity(33 + 32);
        init_sign_data.extend_from_slice(&init_pubkey);
        init_sign_data.extend_from_slice(&init_nonce);
        crypto.verify_peer_signature(&init_sign_data, &init_sig)?;
        info!("Handshake init verified from {}", addr);

        let (our_ephemeral_secret, our_ephemeral_public) = crypto.generate_ephemeral_keypair();
        let our_nonce = CryptoContext::generate_nonce();

        // SECURITY: Signature covers our ephemeral public key + both nonces
        // This binds our key to the response and prevents substitution attacks
        let mut sign_data = Vec::with_capacity(33 + 32 + 32);
        sign_data.extend_from_slice(&our_ephemeral_public);
        sign_data.extend_from_slice(&init_nonce);
        sign_data.extend_from_slice(&our_nonce);
        let signature = crypto.sign(&sign_data);

        let response = HandshakeResponse {
            ephemeral_public_key: our_ephemeral_public.to_vec(),
            nonce: our_nonce.to_vec(),
            signature: signature.to_vec(),
        };

        let response_bytes = serde_json::to_vec(&response)
            .map_err(|e| NetworkError::HandshakeFailed(e.to_string()))?;
        Self::send_unencrypted_packet(&mut stream, PacketType::HandshakeResponse, &response_bytes).await?;

        crypto.derive_session_keys(our_ephemeral_secret, &init_pubkey, &our_nonce, &init_nonce)?;
        info!("Session keys derived for {}", addr);

        let complete_packet = Self::receive_packet(&mut stream).await?;
        if complete_packet.header.packet_type != PacketType::HandshakeComplete {
            return Err(NetworkError::HandshakeFailed("Expected HandshakeComplete".into()));
        }

        if complete_packet.payload.len() < NONCE_SIZE {
            return Err(NetworkError::HandshakeFailed("HandshakeComplete too short".into()));
        }

        let mut nonce = [0u8; NONCE_SIZE];
        nonce.copy_from_slice(&complete_packet.payload[..NONCE_SIZE]);
        let ciphertext = &complete_packet.payload[NONCE_SIZE..];

        let plaintext = crypto.decrypt(ciphertext, &nonce, &complete_packet.tag)?;
        let _complete: HandshakeComplete = serde_json::from_slice(&plaintext)
            .map_err(|e| NetworkError::HandshakeFailed(e.to_string()))?;

        info!("Handshake complete with {}", addr);
        Ok(Session::new(stream, addr, crypto, config))
    }

    async fn receive_packet(stream: &mut TcpStream) -> Result<Packet, NetworkError> {
        let mut header_buf = [0u8; HEADER_SIZE];
        stream.read_exact(&mut header_buf).await?;
        let header = PacketHeader::from_bytes(&header_buf)?;

        let mut payload = vec![0u8; header.length as usize];
        stream.read_exact(&mut payload).await?;

        let mut tag = [0u8; TAG_SIZE];
        stream.read_exact(&mut tag).await?;

        Ok(Packet { header, payload, tag })
    }

    async fn send_unencrypted_packet(stream: &mut TcpStream, packet_type: PacketType, payload: &[u8]) -> Result<(), NetworkError> {
        let header = PacketHeader::new(packet_type, payload.len())?;
        stream.write_all(&header.to_bytes()).await?;
        stream.write_all(payload).await?;
        stream.write_all(&[0u8; TAG_SIZE]).await?;
        stream.flush().await?;
        Ok(())
    }

    pub fn stop(&self) {
        self.running.store(false, Ordering::Relaxed);
    }
}

pub struct Connection {
    stream: TcpStream,
    crypto: CryptoContext,
}

impl Connection {
    pub fn new(stream: TcpStream, crypto: CryptoContext) -> Self {
        Self { stream, crypto }
    }

    pub async fn send(&mut self, packet_type: PacketType, payload: &[u8]) -> Result<(), NetworkError> {
        let (ciphertext, nonce, tag) = self.crypto.encrypt(payload)?;

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

    pub async fn receive(&mut self) -> Result<(PacketType, Vec<u8>), NetworkError> {
        let mut header_buf = [0u8; HEADER_SIZE];
        match self.stream.read_exact(&mut header_buf).await {
            Ok(_) => {}
            Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => {
                return Err(NetworkError::ConnectionClosed);
            }
            Err(e) => return Err(e.into()),
        }

        let header = PacketHeader::from_bytes(&header_buf)?;
        let payload_size = header.length as usize;

        if payload_size < NONCE_SIZE {
            return Err(NetworkError::ProtocolError(
                crate::protocol::ProtocolError::IncompletePacket { expected: NONCE_SIZE, got: payload_size }
            ));
        }

        let mut payload = vec![0u8; payload_size];
        self.stream.read_exact(&mut payload).await?;

        let mut tag = [0u8; TAG_SIZE];
        self.stream.read_exact(&mut tag).await?;

        let mut nonce = [0u8; NONCE_SIZE];
        nonce.copy_from_slice(&payload[..NONCE_SIZE]);
        let ciphertext = &payload[NONCE_SIZE..];

        let plaintext = self.crypto.decrypt(ciphertext, &nonce, &tag)?;
        Ok((header.packet_type, plaintext))
    }

    pub fn is_encrypted(&self) -> bool {
        self.crypto.is_session_established()
    }
}

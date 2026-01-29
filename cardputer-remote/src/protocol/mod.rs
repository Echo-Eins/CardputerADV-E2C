//! Protocol module - defines wire format for Cardputer Remote
//!
//! Packet format:
//! [1 byte version][1 byte type][2 bytes length (BE)][payload][16 bytes AES-GCM tag]
//!
//! Total overhead: 4 bytes header + 16 bytes tag = 20 bytes

use serde::{Deserialize, Serialize};
use thiserror::Error;

/// Protocol version - increment on breaking changes
pub const PROTOCOL_VERSION: u8 = 1;

/// Maximum payload size (64KB - overhead)
pub const MAX_PAYLOAD_SIZE: usize = 65515;

/// Header size in bytes
pub const HEADER_SIZE: usize = 4;

/// AES-GCM tag size
pub const TAG_SIZE: usize = 16;

/// Nonce size for AES-GCM (96 bits = 12 bytes)
pub const NONCE_SIZE: usize = 12;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum PacketType {
    // Discovery & Handshake (0x00-0x0F)
    DiscoveryRequest = 0x00,
    DiscoveryResponse = 0x01,
    HandshakeInit = 0x02,
    HandshakeResponse = 0x03,
    HandshakeComplete = 0x04,

    // Session control (0x10-0x1F)
    SessionStart = 0x10,
    SessionEnd = 0x11,
    SessionTimeout = 0x12,
    Heartbeat = 0x13,
    HeartbeatAck = 0x14,

    // Screen data (0x20-0x2F)
    ScreenFrame = 0x20,
    ScreenDelta = 0x21,
    ScreenRequest = 0x22,

    // Input commands (0x30-0x3F)
    MouseMove = 0x30,
    MouseClick = 0x31,
    KeyPress = 0x32,
    KeyRelease = 0x33,
    KeyType = 0x34,

    // Mode switching (0x40-0x4F)
    ModeSwitch = 0x40,
    ModeAck = 0x41,

    // Error (0xF0-0xFF)
    Error = 0xF0,
}

impl TryFrom<u8> for PacketType {
    type Error = ProtocolError;

    fn try_from(value: u8) -> Result<Self, Self::Error> {
        match value {
            0x00 => Ok(Self::DiscoveryRequest),
            0x01 => Ok(Self::DiscoveryResponse),
            0x02 => Ok(Self::HandshakeInit),
            0x03 => Ok(Self::HandshakeResponse),
            0x04 => Ok(Self::HandshakeComplete),
            0x10 => Ok(Self::SessionStart),
            0x11 => Ok(Self::SessionEnd),
            0x12 => Ok(Self::SessionTimeout),
            0x13 => Ok(Self::Heartbeat),
            0x14 => Ok(Self::HeartbeatAck),
            0x20 => Ok(Self::ScreenFrame),
            0x21 => Ok(Self::ScreenDelta),
            0x22 => Ok(Self::ScreenRequest),
            0x30 => Ok(Self::MouseMove),
            0x31 => Ok(Self::MouseClick),
            0x32 => Ok(Self::KeyPress),
            0x33 => Ok(Self::KeyRelease),
            0x34 => Ok(Self::KeyType),
            0x40 => Ok(Self::ModeSwitch),
            0x41 => Ok(Self::ModeAck),
            0xF0 => Ok(Self::Error),
            _ => Err(ProtocolError::InvalidPacketType(value)),
        }
    }
}

#[derive(Debug, Error)]
pub enum ProtocolError {
    #[error("Invalid protocol version: expected {}, got {0}", PROTOCOL_VERSION)]
    InvalidVersion(u8),

    #[error("Invalid packet type: 0x{0:02X}")]
    InvalidPacketType(u8),

    #[error("Payload too large: {0} bytes (max {})", MAX_PAYLOAD_SIZE)]
    PayloadTooLarge(usize),

    #[error("Incomplete packet: expected {expected} bytes, got {got}")]
    IncompletePacket { expected: usize, got: usize },

    #[error("Decryption failed")]
    DecryptionFailed,

    #[error("Invalid nonce")]
    InvalidNonce,

    #[error("Serialization error: {0}")]
    SerializationError(String),
}

/// Raw packet header
#[derive(Debug, Clone, Copy)]
pub struct PacketHeader {
    pub version: u8,
    pub packet_type: PacketType,
    pub length: u16,
}

impl PacketHeader {
    pub fn new(packet_type: PacketType, payload_len: usize) -> Result<Self, ProtocolError> {
        if payload_len > MAX_PAYLOAD_SIZE {
            return Err(ProtocolError::PayloadTooLarge(payload_len));
        }
        Ok(Self {
            version: PROTOCOL_VERSION,
            packet_type,
            length: payload_len as u16,
        })
    }

    pub fn to_bytes(&self) -> [u8; HEADER_SIZE] {
        [
            self.version,
            self.packet_type as u8,
            (self.length >> 8) as u8,
            (self.length & 0xFF) as u8,
        ]
    }

    pub fn from_bytes(bytes: &[u8]) -> Result<Self, ProtocolError> {
        if bytes.len() < HEADER_SIZE {
            return Err(ProtocolError::IncompletePacket {
                expected: HEADER_SIZE,
                got: bytes.len(),
            });
        }

        let version = bytes[0];
        if version != PROTOCOL_VERSION {
            return Err(ProtocolError::InvalidVersion(version));
        }

        let packet_type = PacketType::try_from(bytes[1])?;
        let length = ((bytes[2] as u16) << 8) | (bytes[3] as u16);

        Ok(Self {
            version,
            packet_type,
            length,
        })
    }

    /// Total packet size including header, payload, and tag
    pub fn total_size(&self) -> usize {
        HEADER_SIZE + self.length as usize + TAG_SIZE
    }
}

// ============================================================================
// Payload structures
// ============================================================================

/// Discovery request from Cardputer
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DiscoveryRequest {
    pub cookie: [u8; 16],
    pub cardputer_name: String,
}

/// Discovery response from PC
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DiscoveryResponse {
    pub cookie: [u8; 16],
    pub device_name: String,
    pub server_port: u16,
}

/// ECDH handshake init from Cardputer
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HandshakeInit {
    /// Cardputer's ephemeral public key (compressed, 33 bytes)
    pub ephemeral_public_key: [u8; 33],
    /// Random nonce for this session
    pub nonce: [u8; 32],
    /// Signature of nonce using Cardputer's static private key
    pub signature: [u8; 64],
}

/// ECDH handshake response from PC
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HandshakeResponse {
    /// PC's ephemeral public key (compressed, 33 bytes)
    pub ephemeral_public_key: [u8; 33],
    /// Random nonce for this session
    pub nonce: [u8; 32],
    /// Signature of (cardputer_nonce || pc_nonce) using PC's static private key
    pub signature: [u8; 64],
}

/// Handshake completion confirmation
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HandshakeComplete {
    /// HMAC of session transcript
    pub transcript_mac: [u8; 32],
}

/// Screen frame data
#[derive(Debug, Clone)]
pub struct ScreenFrame {
    /// Frame sequence number
    pub sequence: u32,
    /// Timestamp (ms since session start)
    pub timestamp: u32,
    /// JPEG compressed image data
    pub jpeg_data: Vec<u8>,
}

impl ScreenFrame {
    pub fn serialize(&self) -> Vec<u8> {
        let mut buf = Vec::with_capacity(8 + self.jpeg_data.len());
        buf.extend_from_slice(&self.sequence.to_be_bytes());
        buf.extend_from_slice(&self.timestamp.to_be_bytes());
        buf.extend_from_slice(&self.jpeg_data);
        buf
    }

    pub fn deserialize(data: &[u8]) -> Result<Self, ProtocolError> {
        if data.len() < 8 {
            return Err(ProtocolError::IncompletePacket {
                expected: 8,
                got: data.len(),
            });
        }

        let sequence = u32::from_be_bytes([data[0], data[1], data[2], data[3]]);
        let timestamp = u32::from_be_bytes([data[4], data[5], data[6], data[7]]);
        let jpeg_data = data[8..].to_vec();

        Ok(Self {
            sequence,
            timestamp,
            jpeg_data,
        })
    }
}

/// Mouse movement command
#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub struct MouseMove {
    /// Relative X movement (-128 to 127)
    pub dx: i8,
    /// Relative Y movement (-128 to 127)
    pub dy: i8,
}

/// Mouse click command
#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub struct MouseClick {
    pub button: MouseButton,
    pub action: ClickAction,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
#[repr(u8)]
pub enum MouseButton {
    Left = 0,
    Right = 1,
    Middle = 2,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
#[repr(u8)]
pub enum ClickAction {
    Press = 0,
    Release = 1,
    Click = 2,
    DoubleClick = 3,
}

/// Keyboard key press/release
#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub struct KeyEvent {
    /// USB HID keycode
    pub keycode: u8,
    /// Modifier flags (Ctrl, Shift, Alt, GUI)
    pub modifiers: u8,
}

/// Mode switch command
#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub struct ModeSwitch {
    pub mode: InputMode,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[repr(u8)]
pub enum InputMode {
    Mouse = 0,
    Keyboard = 1,
}

/// Error packet
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ErrorPacket {
    pub code: u16,
    pub message: String,
}

// ============================================================================
// Packet builder/parser
// ============================================================================

pub struct Packet {
    pub header: PacketHeader,
    pub payload: Vec<u8>,
    pub tag: [u8; TAG_SIZE],
}

impl Packet {
    /// Create a new packet (payload should already be encrypted)
    pub fn new(packet_type: PacketType, encrypted_payload: Vec<u8>, tag: [u8; TAG_SIZE]) -> Result<Self, ProtocolError> {
        let header = PacketHeader::new(packet_type, encrypted_payload.len())?;
        Ok(Self {
            header,
            payload: encrypted_payload,
            tag,
        })
    }

    /// Serialize packet to bytes
    pub fn to_bytes(&self) -> Vec<u8> {
        let mut buf = Vec::with_capacity(self.header.total_size());
        buf.extend_from_slice(&self.header.to_bytes());
        buf.extend_from_slice(&self.payload);
        buf.extend_from_slice(&self.tag);
        buf
    }

    /// Parse packet from bytes
    pub fn from_bytes(bytes: &[u8]) -> Result<Self, ProtocolError> {
        let header = PacketHeader::from_bytes(bytes)?;
        let total_size = header.total_size();

        if bytes.len() < total_size {
            return Err(ProtocolError::IncompletePacket {
                expected: total_size,
                got: bytes.len(),
            });
        }

        let payload_end = HEADER_SIZE + header.length as usize;
        let payload = bytes[HEADER_SIZE..payload_end].to_vec();

        let mut tag = [0u8; TAG_SIZE];
        tag.copy_from_slice(&bytes[payload_end..payload_end + TAG_SIZE]);

        Ok(Self {
            header,
            payload,
            tag,
        })
    }
}

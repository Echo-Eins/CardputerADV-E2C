//! Data Transfer Objects for Cardputer LLM Chat Gateway API
//!
//! JSON contracts for all API endpoints.

use serde::{Deserialize, Serialize};

// ============================================================================
// Auth DTOs
// ============================================================================

/// POST /auth/challenge - Request a challenge for handshake
#[derive(Debug, Serialize, Deserialize)]
pub struct ChallengeRequest {
    /// Cardputer's public key (hex encoded, 33 or 65 bytes)
    pub client_public_key: String,
}

/// POST /auth/challenge - Challenge response
#[derive(Debug, Serialize, Deserialize)]
pub struct ChallengeResponse {
    /// Random challenge bytes (hex encoded, 32 bytes)
    pub challenge: String,
    /// Server timestamp (Unix epoch seconds)
    pub timestamp: u64,
    /// One-time nonce for replay protection (hex encoded, 16 bytes)
    pub nonce: String,
    /// Server's signature over (challenge || timestamp || nonce) (hex encoded)
    pub server_signature: String,
}

/// POST /auth/verify - Verify client signature and get session token
#[derive(Debug, Serialize, Deserialize)]
pub struct VerifyRequest {
    /// The nonce from challenge response
    pub nonce: String,
    /// Client's signature over the challenge (hex encoded)
    pub client_signature: String,
}

/// POST /auth/verify - Session token response
#[derive(Debug, Serialize, Deserialize)]
pub struct VerifyResponse {
    /// Bearer token for subsequent requests
    pub session_token: String,
    /// Token expiration (Unix epoch seconds)
    pub expires_at: u64,
}

// ============================================================================
// Models DTOs
// ============================================================================

/// Model type classification
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ModelType {
    Local,
    Cloud,
}

/// Single model info
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ModelInfo {
    pub name: String,
    pub model_type: ModelType,
    pub size_display: String,
    pub params_display: String,
}

/// GET /models - List of available models
#[derive(Debug, Serialize, Deserialize)]
pub struct ModelsResponse {
    pub models: Vec<ModelInfo>,
}

// ============================================================================
// Chat DTOs
// ============================================================================

/// POST /chat/start - Start a new chat session
#[derive(Debug, Serialize, Deserialize)]
pub struct ChatStartRequest {
    pub model: String,
}

/// POST /chat/start - Chat session created
#[derive(Debug, Serialize, Deserialize)]
pub struct ChatStartResponse {
    pub session_id: String,
    pub model: String,
}

/// POST /chat/send - Send a message in chat session
#[derive(Debug, Serialize, Deserialize)]
pub struct ChatSendRequest {
    pub session_id: String,
    pub message: String,
}

/// POST /chat/send - Chat response (full, not streamed)
#[derive(Debug, Serialize, Deserialize)]
pub struct ChatSendResponse {
    pub session_id: String,
    pub response: String,
    /// True if the response was truncated due to timeout
    pub truncated: bool,
}

/// POST /chat/stop - Stop a chat session
#[derive(Debug, Serialize, Deserialize)]
pub struct ChatStopRequest {
    pub session_id: String,
}

/// POST /chat/stop - Session stopped confirmation
#[derive(Debug, Serialize, Deserialize)]
pub struct ChatStopResponse {
    pub session_id: String,
    pub message_count: usize,
}

// ============================================================================
// Health DTOs
// ============================================================================

/// GET /health - Server health status
#[derive(Debug, Serialize, Deserialize)]
pub struct HealthResponse {
    pub status: String,
    pub ollama_available: bool,
    pub active_sessions: usize,
    pub uptime_secs: u64,
}

// ============================================================================
// Error DTOs
// ============================================================================

/// Error response for all API errors
#[derive(Debug, Serialize, Deserialize)]
pub struct ErrorResponse {
    pub error: String,
    pub code: String,
}

impl ErrorResponse {
    pub fn new(error: impl Into<String>, code: impl Into<String>) -> Self {
        Self {
            error: error.into(),
            code: code.into(),
        }
    }

    pub fn unauthorized() -> Self {
        Self::new("Unauthorized", "AUTH_REQUIRED")
    }

    pub fn invalid_signature() -> Self {
        Self::new("Invalid signature", "INVALID_SIGNATURE")
    }

    pub fn nonce_expired() -> Self {
        Self::new("Nonce expired or already used", "NONCE_EXPIRED")
    }

    pub fn session_not_found() -> Self {
        Self::new("Session not found", "SESSION_NOT_FOUND")
    }

    pub fn model_not_found() -> Self {
        Self::new("Model not found", "MODEL_NOT_FOUND")
    }

    pub fn ollama_unavailable() -> Self {
        Self::new("Ollama is not available", "OLLAMA_UNAVAILABLE")
    }

    pub fn timeout() -> Self {
        Self::new("Request timed out", "TIMEOUT")
    }

    pub fn internal(msg: impl Into<String>) -> Self {
        Self::new(msg, "INTERNAL_ERROR")
    }
}

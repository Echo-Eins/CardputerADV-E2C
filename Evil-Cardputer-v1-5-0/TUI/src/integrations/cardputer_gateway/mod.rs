//! Cardputer LLM Chat Gateway
//!
//! HTTP server providing authenticated access to Ollama for ESP32 Cardputer.
//!
//! ## API Endpoints
//!
//! - `POST /auth/challenge` - Request auth challenge
//! - `POST /auth/verify` - Verify signature and get session token
//! - `GET /health` - Server health check
//! - `GET /models` - List available models (optional ?type=local|cloud filter)
//! - `POST /chat/start` - Start a new chat session
//! - `POST /chat/send` - Send message and get response
//! - `POST /chat/stop` - Stop a chat session

pub mod auth;
pub mod chat;
pub mod dto;
pub mod models;

use std::net::SocketAddr;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Instant;

use anyhow::Result;
use axum::{
    extract::{Query, State},
    http::{HeaderMap, StatusCode},
    response::IntoResponse,
    routing::{get, post},
    Json, Router,
};
use parking_lot::RwLock;
use serde::Deserialize;
use tokio::sync::Notify;
use tower_http::cors::{Any, CorsLayer};

use auth::AuthManager;
use chat::ChatManager;
use dto::*;

/// Gateway runtime state (shared with AppState)
#[derive(Debug)]
pub struct GatewayState {
    pub running: bool,
    pub bind_address: String,
    pub port: u16,
    pub started_at: Option<Instant>,
    pub last_error: Option<String>,
    pub total_requests: u64,
    pub active_auth_sessions: usize,
    pub active_chat_sessions: usize,
}

impl Default for GatewayState {
    fn default() -> Self {
        Self {
            running: false,
            bind_address: "0.0.0.0".to_string(),
            port: 52525,
            started_at: None,
            last_error: None,
            total_requests: 0,
            active_auth_sessions: 0,
            active_chat_sessions: 0,
        }
    }
}

/// Shared state for the HTTP server handlers
struct HttpState {
    auth: AuthManager,
    chat: ChatManager,
    started_at: Instant,
    gateway_state: Arc<RwLock<GatewayState>>,
}

/// The Cardputer Gateway server
pub struct CardputerGateway {
    bind_address: String,
    port: u16,
    server_private_key: String,
    cardputer_public_key: String,
    session_timeout_secs: u64,
    chat_timeout_secs: u64,
    nonce_ttl_secs: u64,
    gateway_state: Arc<RwLock<GatewayState>>,
    shutdown_flag: Arc<AtomicBool>,
    shutdown_notify: Arc<Notify>,
}

impl CardputerGateway {
    /// Create a new CardputerGateway with configuration
    pub fn new(
        bind_address: &str,
        port: u16,
        server_private_key: &str,
        cardputer_public_key: &str,
        session_timeout_secs: u64,
        chat_timeout_secs: u64,
        nonce_ttl_secs: u64,
        gateway_state: Arc<RwLock<GatewayState>>,
    ) -> Result<Self> {
        // Validate keys are provided
        if server_private_key.is_empty() || cardputer_public_key.is_empty() {
            return Err(anyhow::anyhow!("Server private key and Cardputer public key must be configured"));
        }

        // Update initial state
        {
            let mut state = gateway_state.write();
            state.bind_address = bind_address.to_string();
            state.port = port;
        }

        Ok(Self {
            bind_address: bind_address.to_string(),
            port,
            server_private_key: server_private_key.to_string(),
            cardputer_public_key: cardputer_public_key.to_string(),
            session_timeout_secs,
            chat_timeout_secs,
            nonce_ttl_secs,
            gateway_state,
            shutdown_flag: Arc::new(AtomicBool::new(false)),
            shutdown_notify: Arc::new(Notify::new()),
        })
    }

    /// Get shared state reference
    pub fn state(&self) -> Arc<RwLock<GatewayState>> {
        Arc::clone(&self.gateway_state)
    }

    /// Start the HTTP server
    pub async fn start(&self) -> Result<()> {
        // Reset shutdown flag
        self.shutdown_flag.store(false, Ordering::SeqCst);

        // Create auth manager
        let auth = AuthManager::new(
            &self.server_private_key,
            &self.cardputer_public_key,
            self.nonce_ttl_secs,
            self.session_timeout_secs,
        )?;

        // Create chat manager
        let chat = ChatManager::new(self.chat_timeout_secs);

        let http_state = Arc::new(HttpState {
            auth,
            chat,
            started_at: Instant::now(),
            gateway_state: Arc::clone(&self.gateway_state),
        });

        // Build router
        let app = Router::new()
            .route("/health", get(health_handler))
            .route("/auth/challenge", post(challenge_handler))
            .route("/auth/verify", post(verify_handler))
            .route("/models", get(models_handler))
            .route("/chat/start", post(chat_start_handler))
            .route("/chat/send", post(chat_send_handler))
            .route("/chat/stop", post(chat_stop_handler))
            .layer(CorsLayer::new().allow_origin(Any).allow_methods(Any).allow_headers(Any))
            .with_state(http_state);

        let addr: SocketAddr = format!("{}:{}", self.bind_address, self.port).parse()?;

        // Update state to running
        {
            let mut state = self.gateway_state.write();
            state.running = true;
            state.started_at = Some(Instant::now());
            state.last_error = None;
        }

        log::info!("Cardputer Gateway starting on {}", addr);

        // Bind listener
        let listener = match tokio::net::TcpListener::bind(addr).await {
            Ok(l) => l,
            Err(e) => {
                log::error!("Failed to bind Cardputer Gateway: {}", e);
                let mut state = self.gateway_state.write();
                state.last_error = Some(e.to_string());
                state.running = false;
                return Err(e.into());
            }
        };

        log::info!("Cardputer Gateway listening on {}", addr);

        // Create shutdown signal
        let shutdown_flag = Arc::clone(&self.shutdown_flag);
        let shutdown_notify = Arc::clone(&self.shutdown_notify);
        let gateway_state = Arc::clone(&self.gateway_state);

        // Serve with graceful shutdown
        let server = axum::serve(listener, app)
            .with_graceful_shutdown(async move {
                // Wait for shutdown signal
                loop {
                    if shutdown_flag.load(Ordering::SeqCst) {
                        break;
                    }
                    shutdown_notify.notified().await;
                }
                log::info!("Cardputer Gateway shutting down");
            });

        // Run server
        if let Err(e) = server.await {
            log::error!("Cardputer Gateway error: {}", e);
            let mut state = gateway_state.write();
            state.last_error = Some(e.to_string());
        }

        // Update state on exit
        {
            let mut state = gateway_state.write();
            state.running = false;
            state.started_at = None;
        }

        Ok(())
    }

    /// Stop the HTTP server
    pub fn stop(&self) {
        self.shutdown_flag.store(true, Ordering::SeqCst);
        self.shutdown_notify.notify_one();

        let mut state = self.gateway_state.write();
        state.running = false;
        state.started_at = None;
    }

    /// Check if server is running
    pub fn is_running(&self) -> bool {
        self.gateway_state.read().running
    }
}

// ============================================================================
// Handlers
// ============================================================================

async fn health_handler(State(state): State<Arc<HttpState>>) -> impl IntoResponse {
    let ollama_available = models::check_ollama_available().await;
    let active_sessions = state.chat.active_session_count();
    let uptime_secs = state.started_at.elapsed().as_secs();

    // Update gateway state
    {
        let mut gw_state = state.gateway_state.write();
        gw_state.total_requests += 1;
        gw_state.active_chat_sessions = active_sessions;
        gw_state.active_auth_sessions = state.auth.active_session_count();
    }

    Json(HealthResponse {
        status: "ok".to_string(),
        ollama_available,
        active_sessions,
        uptime_secs,
    })
}

async fn challenge_handler(
    State(state): State<Arc<HttpState>>,
    Json(req): Json<ChallengeRequest>,
) -> impl IntoResponse {
    state.gateway_state.write().total_requests += 1;

    match state.auth.generate_challenge(&req.client_public_key) {
        Ok(resp) => (StatusCode::OK, Json(serde_json::to_value(resp).unwrap())),
        Err(e) => {
            log::warn!("Challenge failed: {}", e);
            (
                StatusCode::UNAUTHORIZED,
                Json(serde_json::to_value(ErrorResponse::new(e.to_string(), "CHALLENGE_FAILED")).unwrap()),
            )
        }
    }
}

async fn verify_handler(
    State(state): State<Arc<HttpState>>,
    Json(req): Json<VerifyRequest>,
) -> impl IntoResponse {
    state.gateway_state.write().total_requests += 1;

    match state.auth.verify_and_issue_token(&req.nonce, &req.client_signature) {
        Ok(resp) => {
            // Update session count
            state.gateway_state.write().active_auth_sessions = state.auth.active_session_count();
            (StatusCode::OK, Json(serde_json::to_value(resp).unwrap()))
        }
        Err(e) => {
            log::warn!("Verify failed: {}", e);
            let error = if e.to_string().contains("expired") {
                ErrorResponse::nonce_expired()
            } else {
                ErrorResponse::invalid_signature()
            };
            (StatusCode::UNAUTHORIZED, Json(serde_json::to_value(error).unwrap()))
        }
    }
}

#[derive(Deserialize)]
struct ModelsQuery {
    #[serde(rename = "type")]
    model_type: Option<String>,
}

async fn models_handler(
    State(state): State<Arc<HttpState>>,
    headers: HeaderMap,
    Query(query): Query<ModelsQuery>,
) -> impl IntoResponse {
    state.gateway_state.write().total_requests += 1;

    // Check auth
    if !check_auth(&state, &headers) {
        return (
            StatusCode::UNAUTHORIZED,
            Json(serde_json::to_value(ErrorResponse::unauthorized()).unwrap()),
        );
    }

    let type_filter = query.model_type.as_ref().and_then(|t| {
        match t.to_lowercase().as_str() {
            "local" => Some(ModelType::Local),
            "cloud" => Some(ModelType::Cloud),
            _ => None,
        }
    });

    match models::list_models(type_filter).await {
        Ok(resp) => (StatusCode::OK, Json(serde_json::to_value(resp).unwrap())),
        Err(e) => {
            log::error!("Failed to list models: {}", e);
            (
                StatusCode::INTERNAL_SERVER_ERROR,
                Json(serde_json::to_value(ErrorResponse::internal(e.to_string())).unwrap()),
            )
        }
    }
}

async fn chat_start_handler(
    State(state): State<Arc<HttpState>>,
    headers: HeaderMap,
    Json(req): Json<ChatStartRequest>,
) -> impl IntoResponse {
    state.gateway_state.write().total_requests += 1;

    // Check auth
    if !check_auth(&state, &headers) {
        return (
            StatusCode::UNAUTHORIZED,
            Json(serde_json::to_value(ErrorResponse::unauthorized()).unwrap()),
        );
    }

    let resp = state.chat.start_session(&req.model);
    state.gateway_state.write().active_chat_sessions = state.chat.active_session_count();
    (StatusCode::OK, Json(serde_json::to_value(resp).unwrap()))
}

async fn chat_send_handler(
    State(state): State<Arc<HttpState>>,
    headers: HeaderMap,
    Json(req): Json<ChatSendRequest>,
) -> impl IntoResponse {
    state.gateway_state.write().total_requests += 1;

    // Check auth
    if !check_auth(&state, &headers) {
        return (
            StatusCode::UNAUTHORIZED,
            Json(serde_json::to_value(ErrorResponse::unauthorized()).unwrap()),
        );
    }

    match state.chat.send_message(&req.session_id, &req.message).await {
        Ok(resp) => (StatusCode::OK, Json(serde_json::to_value(resp).unwrap())),
        Err(e) => {
            let error = if e.to_string().contains("not found") {
                ErrorResponse::session_not_found()
            } else {
                ErrorResponse::internal(e.to_string())
            };
            (StatusCode::BAD_REQUEST, Json(serde_json::to_value(error).unwrap()))
        }
    }
}

async fn chat_stop_handler(
    State(state): State<Arc<HttpState>>,
    headers: HeaderMap,
    Json(req): Json<ChatStopRequest>,
) -> impl IntoResponse {
    state.gateway_state.write().total_requests += 1;

    // Check auth
    if !check_auth(&state, &headers) {
        return (
            StatusCode::UNAUTHORIZED,
            Json(serde_json::to_value(ErrorResponse::unauthorized()).unwrap()),
        );
    }

    match state.chat.stop_session(&req.session_id) {
        Ok(resp) => {
            state.gateway_state.write().active_chat_sessions = state.chat.active_session_count();
            (StatusCode::OK, Json(serde_json::to_value(resp).unwrap()))
        }
        Err(e) => {
            let error = if e.to_string().contains("not found") {
                ErrorResponse::session_not_found()
            } else {
                ErrorResponse::internal(e.to_string())
            };
            (StatusCode::BAD_REQUEST, Json(serde_json::to_value(error).unwrap()))
        }
    }
}

/// Check Authorization header for valid Bearer token
fn check_auth(state: &Arc<HttpState>, headers: &HeaderMap) -> bool {
    if let Some(auth_header) = headers.get("Authorization") {
        if let Ok(auth_str) = auth_header.to_str() {
            if let Some(token) = auth_str.strip_prefix("Bearer ") {
                return state.auth.validate_token(token);
            }
        }
    }
    false
}

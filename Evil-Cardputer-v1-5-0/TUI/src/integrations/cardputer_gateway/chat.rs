//! Chat session management for Cardputer LLM Chat Gateway
//!
//! Manages stateful chat sessions with Ollama models.
//! Sessions maintain conversation context across multiple messages.
//! Uses Ollama HTTP API for non-blocking async communication.

use anyhow::{anyhow, Result};
use chrono::Local;
use parking_lot::RwLock;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::fs::{self, File};
use std::io::Write;
use std::path::PathBuf;
use std::sync::Arc;
use std::time::{Duration, Instant};
use uuid::Uuid;

use super::dto::*;

/// Ollama API endpoint
const OLLAMA_API_URL: &str = "http://localhost:11434/api/chat";

/// A single message in a chat session
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ChatMessage {
    pub role: String, // "user" or "assistant"
    pub content: String,
    pub timestamp: u64,
}

/// A chat session with an Ollama model
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ChatSession {
    pub id: String,
    pub model: String,
    pub messages: Vec<ChatMessage>,
    #[serde(skip)]
    pub created_at: Option<Instant>,
    #[serde(skip)]
    pub last_activity: Option<Instant>,
}

impl ChatSession {
    pub fn new(model: String) -> Self {
        let now = Instant::now();
        Self {
            id: Uuid::new_v4().to_string(),
            model,
            messages: Vec::new(),
            created_at: Some(now),
            last_activity: Some(now),
        }
    }

    pub fn add_user_message(&mut self, content: String) {
        self.messages.push(ChatMessage {
            role: "user".to_string(),
            content,
            timestamp: chrono::Utc::now().timestamp() as u64,
        });
        self.last_activity = Some(Instant::now());
    }

    pub fn add_assistant_message(&mut self, content: String) {
        self.messages.push(ChatMessage {
            role: "assistant".to_string(),
            content,
            timestamp: chrono::Utc::now().timestamp() as u64,
        });
        self.last_activity = Some(Instant::now());
    }
}

/// Chat session manager
pub struct ChatManager {
    sessions: Arc<RwLock<HashMap<String, ChatSession>>>,
    chat_timeout: Duration,
    log_dir: PathBuf,
    http_client: reqwest::Client,
}

impl ChatManager {
    pub fn new(chat_timeout_secs: u64) -> Self {
        // Use logs/cardputer directory for chat logs
        let log_dir = PathBuf::from("logs/cardputer");
        let _ = fs::create_dir_all(&log_dir);

        // Create HTTP client with timeout
        let client_timeout = if chat_timeout_secs == 0 {
            Duration::from_secs(300) // 5 min default for HTTP client
        } else {
            Duration::from_secs(chat_timeout_secs)
        };

        let http_client = reqwest::Client::builder()
            .timeout(client_timeout)
            .build()
            .unwrap_or_default();

        Self {
            sessions: Arc::new(RwLock::new(HashMap::new())),
            chat_timeout: if chat_timeout_secs == 0 {
                Duration::from_secs(u64::MAX) // Effectively unlimited
            } else {
                Duration::from_secs(chat_timeout_secs)
            },
            log_dir,
            http_client,
        }
    }

    /// Start a new chat session
    pub fn start_session(&self, model: &str) -> ChatStartResponse {
        let session = ChatSession::new(model.to_string());
        let response = ChatStartResponse {
            session_id: session.id.clone(),
            model: session.model.clone(),
        };

        {
            let mut sessions = self.sessions.write();
            sessions.insert(session.id.clone(), session);
        }

        log::info!("Cardputer chat session started: {} with model {}", response.session_id, model);
        response
    }

    /// Send a message and get response (non-blocking async via HTTP API)
    pub async fn send_message(&self, session_id: &str, user_message: &str) -> Result<ChatSendResponse> {
        // Get session and add user message
        let (model, ollama_messages) = {
            let mut sessions = self.sessions.write();
            let session = sessions.get_mut(session_id)
                .ok_or_else(|| anyhow!("Session not found"))?;

            session.add_user_message(user_message.to_string());

            // Convert to Ollama API format
            let ollama_msgs: Vec<OllamaMessage> = session.messages.iter()
                .map(|m| OllamaMessage {
                    role: m.role.clone(),
                    content: m.content.clone(),
                })
                .collect();

            (session.model.clone(), ollama_msgs)
        };

        // Call Ollama HTTP API (non-blocking)
        let result = tokio::time::timeout(self.chat_timeout, async {
            self.call_ollama_api(&model, &ollama_messages).await
        }).await;

        let (response_text, truncated) = match result {
            Ok(Ok(text)) => (text, false),
            Ok(Err(e)) => {
                log::error!("Ollama API error: {}", e);
                return Err(e);
            }
            Err(_) => {
                log::warn!("Ollama request timed out for session {}", session_id);
                ("".to_string(), true)
            }
        };

        // Add assistant response to session
        if !response_text.is_empty() {
            let mut sessions = self.sessions.write();
            if let Some(session) = sessions.get_mut(session_id) {
                session.add_assistant_message(response_text.clone());
            }
        }

        Ok(ChatSendResponse {
            session_id: session_id.to_string(),
            response: response_text,
            truncated,
        })
    }

    /// Call Ollama HTTP API
    async fn call_ollama_api(&self, model: &str, messages: &[OllamaMessage]) -> Result<String> {
        let request = OllamaChatRequest {
            model: model.to_string(),
            messages: messages.to_vec(),
            stream: false,
        };

        let response = self.http_client
            .post(OLLAMA_API_URL)
            .json(&request)
            .send()
            .await
            .map_err(|e| anyhow!("Failed to connect to Ollama: {}", e))?;

        if !response.status().is_success() {
            let status = response.status();
            let body = response.text().await.unwrap_or_default();
            return Err(anyhow!("Ollama API error {}: {}", status, body));
        }

        let chat_response: OllamaChatResponse = response
            .json()
            .await
            .map_err(|e| anyhow!("Failed to parse Ollama response: {}", e))?;

        Ok(chat_response.message.content)
    }

    /// Stop a chat session and save logs
    pub fn stop_session(&self, session_id: &str) -> Result<ChatStopResponse> {
        let session = {
            let mut sessions = self.sessions.write();
            sessions.remove(session_id)
                .ok_or_else(|| anyhow!("Session not found"))?
        };

        let message_count = session.messages.len();

        // Save chat log
        if let Err(e) = self.save_chat_log(&session) {
            log::warn!("Failed to save chat log for session {}: {}", session_id, e);
        }

        log::info!("Cardputer chat session stopped: {} ({} messages)", session_id, message_count);

        Ok(ChatStopResponse {
            session_id: session_id.to_string(),
            message_count,
        })
    }

    /// Get count of active sessions
    pub fn active_session_count(&self) -> usize {
        self.sessions.read().len()
    }

    /// Get list of active session IDs
    #[allow(dead_code)]
    pub fn active_sessions(&self) -> Vec<String> {
        self.sessions.read().keys().cloned().collect()
    }

    /// Clean up stale sessions (inactive for more than 1 hour)
    #[allow(dead_code)]
    pub fn cleanup_stale_sessions(&self) {
        let stale_threshold = Duration::from_secs(3600);
        let now = Instant::now();

        let stale_ids: Vec<String> = {
            let sessions = self.sessions.read();
            sessions.iter()
                .filter(|(_, s)| {
                    s.last_activity
                        .map(|t| now.duration_since(t) > stale_threshold)
                        .unwrap_or(true)
                })
                .map(|(id, _)| id.clone())
                .collect()
        };

        for id in stale_ids {
            if self.stop_session(&id).is_ok() {
                log::info!("Cleaned up stale session: {}", id);
            }
        }
    }

    /// Graceful shutdown - save all active sessions
    pub fn shutdown(&self) {
        log::info!("ChatManager shutting down, saving all sessions...");

        let session_ids: Vec<String> = {
            self.sessions.read().keys().cloned().collect()
        };

        let count = session_ids.len();
        for session_id in session_ids {
            if let Err(e) = self.stop_session(&session_id) {
                log::warn!("Failed to stop session {} during shutdown: {}", session_id, e);
            }
        }

        log::info!("ChatManager shutdown complete, saved {} sessions", count);
    }

    /// Save chat log to file (Ollama-compatible format)
    fn save_chat_log(&self, session: &ChatSession) -> Result<()> {
        // Skip empty sessions
        if session.messages.is_empty() {
            return Ok(());
        }

        let timestamp = Local::now().format("%Y%m%d_%H%M%S");
        let filename = format!("cardputer_{}_{}.json", session.model.replace(':', "_"), timestamp);
        let path = self.log_dir.join(&filename);

        #[derive(Serialize)]
        struct ChatLogFile {
            model: String,
            session_id: String,
            messages: Vec<ChatMessage>,
            started_at: u64,
            ended_at: u64,
        }

        let log = ChatLogFile {
            model: session.model.clone(),
            session_id: session.id.clone(),
            messages: session.messages.clone(),
            started_at: session.messages.first().map(|m| m.timestamp).unwrap_or(0),
            ended_at: session.messages.last().map(|m| m.timestamp).unwrap_or(0),
        };

        let json = serde_json::to_string_pretty(&log)?;
        let mut file = File::create(&path)?;
        file.write_all(json.as_bytes())?;

        log::info!("Chat log saved: {}", path.display());
        Ok(())
    }
}

impl Drop for ChatManager {
    fn drop(&mut self) {
        // Note: Can't call shutdown() here because it needs &self, not &mut self
        // The graceful shutdown should be called explicitly via shutdown()
        let count = self.sessions.read().len();
        if count > 0 {
            log::warn!("ChatManager dropped with {} active sessions - call shutdown() for graceful cleanup", count);
        }
    }
}

/// Ollama API message format
#[derive(Debug, Clone, Serialize, Deserialize)]
struct OllamaMessage {
    role: String,
    content: String,
}

/// Ollama chat API request
#[derive(Debug, Serialize)]
struct OllamaChatRequest {
    model: String,
    messages: Vec<OllamaMessage>,
    stream: bool,
}

/// Ollama chat API response
#[derive(Debug, Deserialize)]
struct OllamaChatResponse {
    message: OllamaMessage,
    #[allow(dead_code)]
    done: bool,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_session_lifecycle() {
        let manager = ChatManager::new(30);

        // Start session
        let start_resp = manager.start_session("test-model");
        assert!(!start_resp.session_id.is_empty());
        assert_eq!(start_resp.model, "test-model");

        // Check active sessions
        assert_eq!(manager.active_session_count(), 1);

        // Stop session
        let stop_resp = manager.stop_session(&start_resp.session_id).unwrap();
        assert_eq!(stop_resp.session_id, start_resp.session_id);
        assert_eq!(stop_resp.message_count, 0);

        // Check sessions cleared
        assert_eq!(manager.active_session_count(), 0);
    }

    #[test]
    fn test_graceful_shutdown() {
        let manager = ChatManager::new(30);

        // Start multiple sessions
        manager.start_session("model1");
        manager.start_session("model2");
        manager.start_session("model3");

        assert_eq!(manager.active_session_count(), 3);

        // Shutdown
        manager.shutdown();

        assert_eq!(manager.active_session_count(), 0);
    }
}

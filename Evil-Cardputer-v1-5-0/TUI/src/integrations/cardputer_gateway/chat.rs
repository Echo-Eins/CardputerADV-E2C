//! Chat session management for Cardputer LLM Chat Gateway
//!
//! Manages stateful chat sessions with Ollama models.
//! Sessions maintain conversation context across multiple messages.

use anyhow::{anyhow, Result};
use chrono::Local;
use parking_lot::RwLock;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::fs::{self, File};
use std::io::Write;
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};
use uuid::Uuid;

use super::dto::*;

/// A single message in a chat session
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ChatMessage {
    pub role: String, // "user" or "assistant"
    pub content: String,
    pub timestamp: u64,
}

/// A chat session with an Ollama model
#[derive(Debug, Clone)]
pub struct ChatSession {
    pub id: String,
    pub model: String,
    pub messages: Vec<ChatMessage>,
    pub created_at: Instant,
    pub last_activity: Instant,
}

impl ChatSession {
    pub fn new(model: String) -> Self {
        let now = Instant::now();
        Self {
            id: Uuid::new_v4().to_string(),
            model,
            messages: Vec::new(),
            created_at: now,
            last_activity: now,
        }
    }

    pub fn add_user_message(&mut self, content: String) {
        self.messages.push(ChatMessage {
            role: "user".to_string(),
            content,
            timestamp: chrono::Utc::now().timestamp() as u64,
        });
        self.last_activity = Instant::now();
    }

    pub fn add_assistant_message(&mut self, content: String) {
        self.messages.push(ChatMessage {
            role: "assistant".to_string(),
            content,
            timestamp: chrono::Utc::now().timestamp() as u64,
        });
        self.last_activity = Instant::now();
    }
}

/// Chat session manager
pub struct ChatManager {
    sessions: RwLock<HashMap<String, ChatSession>>,
    chat_timeout: Duration,
    log_dir: PathBuf,
}

impl ChatManager {
    pub fn new(chat_timeout_secs: u64) -> Self {
        // Use logs/cardputer directory for chat logs
        let log_dir = PathBuf::from("logs/cardputer");
        let _ = fs::create_dir_all(&log_dir);

        Self {
            sessions: RwLock::new(HashMap::new()),
            chat_timeout: if chat_timeout_secs == 0 {
                Duration::from_secs(u64::MAX) // Effectively unlimited
            } else {
                Duration::from_secs(chat_timeout_secs)
            },
            log_dir,
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

    /// Send a message and get response (blocking)
    pub async fn send_message(&self, session_id: &str, user_message: &str) -> Result<ChatSendResponse> {
        // Get session and add user message
        let (model, messages) = {
            let mut sessions = self.sessions.write();
            let session = sessions.get_mut(session_id)
                .ok_or_else(|| anyhow!("Session not found"))?;

            session.add_user_message(user_message.to_string());
            (session.model.clone(), session.messages.clone())
        };

        // Build conversation for Ollama
        let conversation = build_ollama_prompt(&messages);

        // Call Ollama with timeout
        let timeout = self.chat_timeout;
        let result = tokio::time::timeout(timeout, async {
            call_ollama(&model, &conversation).await
        }).await;

        let (response_text, truncated) = match result {
            Ok(Ok(text)) => (text, false),
            Ok(Err(e)) => return Err(e),
            Err(_) => {
                // Timeout occurred
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
    pub fn active_sessions(&self) -> Vec<String> {
        self.sessions.read().keys().cloned().collect()
    }

    /// Clean up stale sessions (inactive for more than 1 hour)
    pub fn cleanup_stale_sessions(&self) {
        let stale_threshold = Duration::from_secs(3600);
        let now = Instant::now();

        let stale_ids: Vec<String> = {
            let sessions = self.sessions.read();
            sessions.iter()
                .filter(|(_, s)| now.duration_since(s.last_activity) > stale_threshold)
                .map(|(id, _)| id.clone())
                .collect()
        };

        for id in stale_ids {
            if let Ok(_) = self.stop_session(&id) {
                log::info!("Cleaned up stale session: {}", id);
            }
        }
    }

    /// Save chat log to file (Ollama-compatible format)
    fn save_chat_log(&self, session: &ChatSession) -> Result<()> {
        let timestamp = Local::now().format("%Y%m%d_%H%M%S");
        let filename = format!("cardputer_{}_{}.json", session.model.replace(":", "_"), timestamp);
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

/// Build a prompt string for Ollama from conversation history
fn build_ollama_prompt(messages: &[ChatMessage]) -> String {
    // For single-turn, just use the last user message
    // For multi-turn, we need to format as conversation
    let mut prompt = String::new();

    for msg in messages {
        match msg.role.as_str() {
            "user" => {
                prompt.push_str(&format!("User: {}\n", msg.content));
            }
            "assistant" => {
                prompt.push_str(&format!("Assistant: {}\n", msg.content));
            }
            _ => {}
        }
    }

    // Add prompt for assistant response
    prompt.push_str("Assistant: ");
    prompt
}

/// Call Ollama with a prompt and return the response
async fn call_ollama(model: &str, prompt: &str) -> Result<String> {
    // Use tokio::task::spawn_blocking for the synchronous Command execution
    let model = model.to_string();
    let prompt = prompt.to_string();

    let result = tokio::task::spawn_blocking(move || {
        let mut child = Command::new("ollama")
            .arg("run")
            .arg(&model)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()?;

        // Write prompt to stdin
        if let Some(mut stdin) = child.stdin.take() {
            stdin.write_all(prompt.as_bytes())?;
        }

        let output = child.wait_with_output()?;

        if !output.status.success() {
            let stderr = String::from_utf8_lossy(&output.stderr);
            return Err(anyhow!("Ollama error: {}", stderr));
        }

        let response = String::from_utf8_lossy(&output.stdout).to_string();
        Ok(response.trim().to_string())
    }).await??;

    Ok(result)
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
    fn test_build_prompt() {
        let messages = vec![
            ChatMessage {
                role: "user".to_string(),
                content: "Hello".to_string(),
                timestamp: 0,
            },
            ChatMessage {
                role: "assistant".to_string(),
                content: "Hi there!".to_string(),
                timestamp: 0,
            },
            ChatMessage {
                role: "user".to_string(),
                content: "How are you?".to_string(),
                timestamp: 0,
            },
        ];

        let prompt = build_ollama_prompt(&messages);
        assert!(prompt.contains("User: Hello"));
        assert!(prompt.contains("Assistant: Hi there!"));
        assert!(prompt.contains("User: How are you?"));
        assert!(prompt.ends_with("Assistant: "));
    }
}

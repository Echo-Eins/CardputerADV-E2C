//! Authentication module for Cardputer LLM Chat Gateway
//!
//! Implements ECDSA-based challenge-response handshake:
//! 1. Client requests challenge with its public key
//! 2. Server returns challenge + nonce + timestamp + server signature
//! 3. Client verifies server signature, signs challenge with its private key
//! 4. Server verifies client signature against known cardputer_public_key
//! 5. On success, server returns session token

use anyhow::{anyhow, Result};
use p256::ecdsa::{signature::Signer, signature::Verifier, Signature, SigningKey, VerifyingKey};
use p256::SecretKey;
use parking_lot::RwLock;
use rand::RngCore;
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use uuid::Uuid;

use super::dto::*;

/// Nonce entry with creation time for TTL enforcement
struct NonceEntry {
    challenge: Vec<u8>,
    client_pubkey: Vec<u8>,
    created_at: Instant,
}

/// Authentication state manager
pub struct AuthManager {
    /// Server's signing key
    server_key: SigningKey,
    /// Server's public key (for clients to verify)
    server_pubkey: VerifyingKey,
    /// Expected Cardputer public key
    cardputer_pubkey: VerifyingKey,
    /// Active nonces (nonce_hex -> NonceEntry)
    nonces: RwLock<HashMap<String, NonceEntry>>,
    /// Active session tokens (token -> expiry timestamp)
    sessions: RwLock<HashMap<String, u64>>,
    /// Nonce TTL
    nonce_ttl: Duration,
    /// Session token validity
    session_timeout: Duration,
}

impl AuthManager {
    /// Create new AuthManager from hex-encoded keys
    pub fn new(
        server_private_key_hex: &str,
        cardputer_public_key_hex: &str,
        nonce_ttl_secs: u64,
        session_timeout_secs: u64,
    ) -> Result<Self> {
        // Parse server private key
        let server_key_bytes = hex::decode(server_private_key_hex)
            .map_err(|_| anyhow!("Invalid server private key hex"))?;
        if server_key_bytes.len() != 32 {
            return Err(anyhow!("Server private key must be 32 bytes"));
        }
        let server_secret = SecretKey::from_slice(&server_key_bytes)
            .map_err(|e| anyhow!("Invalid server private key: {}", e))?;
        let server_key = SigningKey::from(server_secret);
        let server_pubkey = VerifyingKey::from(&server_key);

        // Parse cardputer public key
        let cardputer_pubkey_bytes = hex::decode(cardputer_public_key_hex)
            .map_err(|_| anyhow!("Invalid cardputer public key hex"))?;
        let cardputer_pubkey = parse_public_key(&cardputer_pubkey_bytes)?;

        Ok(Self {
            server_key,
            server_pubkey,
            cardputer_pubkey,
            nonces: RwLock::new(HashMap::new()),
            sessions: RwLock::new(HashMap::new()),
            nonce_ttl: Duration::from_secs(nonce_ttl_secs),
            session_timeout: Duration::from_secs(session_timeout_secs),
        })
    }

    /// Generate a challenge for the client
    pub fn generate_challenge(&self, client_pubkey_hex: &str) -> Result<ChallengeResponse> {
        // Validate client public key matches expected
        let client_pubkey_bytes = hex::decode(client_pubkey_hex)
            .map_err(|_| anyhow!("Invalid client public key hex"))?;
        let client_pubkey = parse_public_key(&client_pubkey_bytes)?;

        // Verify it matches the expected Cardputer key
        if client_pubkey.to_encoded_point(true).as_bytes()
            != self.cardputer_pubkey.to_encoded_point(true).as_bytes()
        {
            return Err(anyhow!("Client public key does not match expected Cardputer key"));
        }

        // Generate random challenge (32 bytes)
        let mut challenge = [0u8; 32];
        rand::thread_rng().fill_bytes(&mut challenge);

        // Generate random nonce (16 bytes)
        let mut nonce = [0u8; 16];
        rand::thread_rng().fill_bytes(&mut nonce);

        // Current timestamp
        let timestamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_secs();

        // Sign (challenge || timestamp || nonce)
        let mut to_sign = Vec::with_capacity(32 + 8 + 16);
        to_sign.extend_from_slice(&challenge);
        to_sign.extend_from_slice(&timestamp.to_be_bytes());
        to_sign.extend_from_slice(&nonce);

        let hash = Sha256::digest(&to_sign);
        let signature: Signature = self.server_key.sign(&hash);

        let nonce_hex = hex::encode(&nonce);

        // Store nonce for verification
        {
            let mut nonces = self.nonces.write();
            // Clean expired nonces
            let now = Instant::now();
            nonces.retain(|_, v| now.duration_since(v.created_at) < self.nonce_ttl);

            nonces.insert(nonce_hex.clone(), NonceEntry {
                challenge: challenge.to_vec(),
                client_pubkey: client_pubkey_bytes,
                created_at: now,
            });
        }

        Ok(ChallengeResponse {
            challenge: hex::encode(&challenge),
            timestamp,
            nonce: nonce_hex,
            server_signature: hex::encode(signature.to_bytes()),
        })
    }

    /// Verify client signature and issue session token
    pub fn verify_and_issue_token(&self, nonce_hex: &str, client_signature_hex: &str) -> Result<VerifyResponse> {
        // Look up and consume nonce
        let entry = {
            let mut nonces = self.nonces.write();
            nonces.remove(nonce_hex)
                .ok_or_else(|| anyhow!("Nonce not found or expired"))?
        };

        // Check TTL
        if Instant::now().duration_since(entry.created_at) >= self.nonce_ttl {
            return Err(anyhow!("Nonce expired"));
        }

        // Parse client signature
        let signature_bytes = hex::decode(client_signature_hex)
            .map_err(|_| anyhow!("Invalid signature hex"))?;
        let signature = Signature::from_slice(&signature_bytes)
            .map_err(|_| anyhow!("Invalid signature format"))?;

        // Verify signature over the challenge
        let hash = Sha256::digest(&entry.challenge);
        self.cardputer_pubkey.verify(&hash, &signature)
            .map_err(|_| anyhow!("Signature verification failed"))?;

        // Generate session token
        let token = Uuid::new_v4().to_string();
        let expires_at = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_secs() + self.session_timeout.as_secs();

        // Store session
        {
            let mut sessions = self.sessions.write();
            // Clean expired sessions
            let now = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_secs();
            sessions.retain(|_, &mut expiry| expiry > now);

            sessions.insert(token.clone(), expires_at);
        }

        Ok(VerifyResponse {
            session_token: token,
            expires_at,
        })
    }

    /// Validate a session token
    pub fn validate_token(&self, token: &str) -> bool {
        let sessions = self.sessions.read();
        if let Some(&expiry) = sessions.get(token) {
            let now = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_secs();
            expiry > now
        } else {
            false
        }
    }

    /// Revoke a session token
    pub fn revoke_token(&self, token: &str) {
        let mut sessions = self.sessions.write();
        sessions.remove(token);
    }

    /// Get server's public key (hex encoded, compressed)
    pub fn server_public_key_hex(&self) -> String {
        hex::encode(self.server_pubkey.to_encoded_point(true).as_bytes())
    }

    /// Get count of active sessions
    pub fn active_session_count(&self) -> usize {
        let sessions = self.sessions.read();
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_secs();
        sessions.values().filter(|&&exp| exp > now).count()
    }
}

/// Parse a public key from bytes (supports both compressed 33-byte and uncompressed 65-byte formats)
fn parse_public_key(bytes: &[u8]) -> Result<VerifyingKey> {
    use p256::EncodedPoint;

    let point = EncodedPoint::from_bytes(bytes)
        .map_err(|_| anyhow!("Invalid public key encoding"))?;

    let pubkey = VerifyingKey::from_encoded_point(&point)
        .map_err(|_| anyhow!("Invalid public key point"))?;

    Ok(pubkey)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_auth_flow() {
        // Generate test keys
        let server_secret = SecretKey::random(&mut rand::thread_rng());
        let server_key = SigningKey::from(server_secret.clone());
        let server_private_hex = hex::encode(server_secret.to_bytes());

        let client_secret = SecretKey::random(&mut rand::thread_rng());
        let client_key = SigningKey::from(client_secret.clone());
        let client_pubkey = VerifyingKey::from(&client_key);
        let client_public_hex = hex::encode(client_pubkey.to_encoded_point(true).as_bytes());

        // Create auth manager
        let auth = AuthManager::new(
            &server_private_hex,
            &client_public_hex,
            20,
            3600,
        ).unwrap();

        // Generate challenge
        let challenge_resp = auth.generate_challenge(&client_public_hex).unwrap();

        // Client signs the challenge
        let challenge_bytes = hex::decode(&challenge_resp.challenge).unwrap();
        let hash = Sha256::digest(&challenge_bytes);
        let client_sig: Signature = client_key.sign(&hash);
        let client_sig_hex = hex::encode(client_sig.to_bytes());

        // Verify and get token
        let verify_resp = auth.verify_and_issue_token(&challenge_resp.nonce, &client_sig_hex).unwrap();

        // Token should be valid
        assert!(auth.validate_token(&verify_resp.session_token));

        // Revoke token
        auth.revoke_token(&verify_resp.session_token);
        assert!(!auth.validate_token(&verify_resp.session_token));
    }
}

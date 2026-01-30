/**
 * @file remote_crypto.h
 * @brief Cryptographic operations for Cardputer Remote
 *
 * Implements:
 * - ECDH key exchange (secp256r1/NIST P-256)
 * - ECDSA signatures for mutual authentication
 * - AES-128-GCM authenticated encryption
 * - HKDF-SHA256 key derivation
 * - Nonce management with replay protection
 */

#ifndef REMOTE_CRYPTO_H
#define REMOTE_CRYPTO_H

#include "remote_config.h"
#include <mbedtls/ecdh.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Crypto context for a session
 */
typedef struct {
    // RNG
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;

    // Our static key pair (for ECDSA)
    mbedtls_ecdsa_context our_static_key;

    // Peer's static public key (for signature verification)
    mbedtls_ecp_point peer_static_public;
    bool peer_key_loaded;

    // Ephemeral ECDH key pair (generated per session)
    mbedtls_ecdh_context ecdh;
    uint8_t our_ephemeral_public[ECDH_PUBLIC_KEY_SIZE];

    // Session keys
    session_keys_t session_keys;
    bool session_established;

    // AES-GCM contexts
    mbedtls_gcm_context gcm_encrypt;
    mbedtls_gcm_context gcm_decrypt;

    // Nonce management
    nonce_state_t outgoing_nonce;
    uint32_t incoming_nonce_counter;    // Last seen counter + 1
    uint8_t incoming_nonce_random[8];   // Expected random part
    bool incoming_nonce_initialized;

    // Handshake state
    uint8_t our_handshake_nonce[HANDSHAKE_NONCE_SIZE];
    uint8_t peer_handshake_nonce[HANDSHAKE_NONCE_SIZE];
} crypto_context_t;

// =============================================================================
// Initialization & Cleanup
// =============================================================================

/**
 * @brief Initialize crypto context with private key
 * @param ctx Crypto context to initialize
 * @param private_key 32-byte private key
 * @return REMOTE_OK on success
 */
remote_error_t crypto_init(crypto_context_t *ctx, const uint8_t *private_key);

/**
 * @brief Free crypto context resources
 */
void crypto_free(crypto_context_t *ctx);

/**
 * @brief Set peer's static public key for authentication
 * @param ctx Crypto context
 * @param public_key 33-byte compressed public key
 * @return REMOTE_OK on success
 */
remote_error_t crypto_set_peer_public_key(crypto_context_t *ctx,
                                          const uint8_t *public_key);

// =============================================================================
// Ephemeral Key Generation
// =============================================================================

/**
 * @brief Generate new ephemeral ECDH key pair
 * @param ctx Crypto context
 * @param[out] public_key_out 33-byte buffer for compressed public key
 * @return REMOTE_OK on success
 */
remote_error_t crypto_generate_ephemeral_keypair(crypto_context_t *ctx,
                                                  uint8_t *public_key_out);

/**
 * @brief Generate random handshake nonce
 * @param ctx Crypto context
 * @param[out] nonce_out 32-byte buffer
 * @return REMOTE_OK on success
 */
remote_error_t crypto_generate_nonce(crypto_context_t *ctx,
                                     uint8_t *nonce_out);

// =============================================================================
// Signatures
// =============================================================================

/**
 * @brief Sign data with our static private key
 * @param ctx Crypto context
 * @param data Data to sign
 * @param data_len Data length
 * @param[out] signature_out 64-byte signature buffer
 * @return REMOTE_OK on success
 */
remote_error_t crypto_sign(crypto_context_t *ctx,
                           const uint8_t *data, size_t data_len,
                           uint8_t *signature_out);

/**
 * @brief Verify signature from peer
 * @param ctx Crypto context
 * @param data Data that was signed
 * @param data_len Data length
 * @param signature 64-byte signature
 * @return REMOTE_OK if signature is valid
 */
remote_error_t crypto_verify_peer_signature(crypto_context_t *ctx,
                                            const uint8_t *data, size_t data_len,
                                            const uint8_t *signature);

// =============================================================================
// Key Derivation
// =============================================================================

/**
 * @brief Perform ECDH and derive session keys
 * @param ctx Crypto context
 * @param peer_ephemeral_public Peer's 33-byte compressed ephemeral public key
 * @param our_nonce Our 32-byte handshake nonce
 * @param peer_nonce Peer's 32-byte handshake nonce
 * @param is_server True if we are server (affects key direction)
 * @return REMOTE_OK on success
 */
remote_error_t crypto_derive_session_keys(crypto_context_t *ctx,
                                          const uint8_t *peer_ephemeral_public,
                                          const uint8_t *our_nonce,
                                          const uint8_t *peer_nonce,
                                          bool is_server);

// =============================================================================
// Encryption/Decryption
// =============================================================================

/**
 * @brief Encrypt plaintext with AES-128-GCM
 * @param ctx Crypto context (session must be established)
 * @param plaintext Input data
 * @param plaintext_len Input length
 * @param[out] ciphertext Output buffer (must be >= plaintext_len)
 * @param[out] nonce_out 12-byte nonce used
 * @param[out] tag_out 16-byte authentication tag
 * @return REMOTE_OK on success
 */
remote_error_t crypto_encrypt(crypto_context_t *ctx,
                              const uint8_t *plaintext, size_t plaintext_len,
                              uint8_t *ciphertext,
                              uint8_t *nonce_out, uint8_t *tag_out);

/**
 * @brief Decrypt ciphertext with AES-128-GCM
 * @param ctx Crypto context
 * @param ciphertext Input ciphertext
 * @param ciphertext_len Ciphertext length
 * @param nonce 12-byte nonce
 * @param tag 16-byte authentication tag
 * @param[out] plaintext Output buffer (must be >= ciphertext_len)
 * @return REMOTE_OK on success, REMOTE_ERR_REPLAY if nonce reused
 */
remote_error_t crypto_decrypt(crypto_context_t *ctx,
                              const uint8_t *ciphertext, size_t ciphertext_len,
                              const uint8_t *nonce, const uint8_t *tag,
                              uint8_t *plaintext);

// =============================================================================
// Transcript MAC
// =============================================================================

/**
 * @brief Compute HMAC-SHA256 for handshake transcript
 * @param ctx Crypto context (session keys must be derived)
 * @param transcript Handshake transcript data
 * @param transcript_len Transcript length
 * @param[out] mac_out 32-byte MAC output
 * @return REMOTE_OK on success
 */
remote_error_t crypto_compute_transcript_mac(crypto_context_t *ctx,
                                             const uint8_t *transcript,
                                             size_t transcript_len,
                                             uint8_t *mac_out);

/**
 * @brief Constant-time comparison
 * @return true if equal
 */
bool crypto_constant_time_eq(const uint8_t *a, const uint8_t *b, size_t len);

// =============================================================================
// Utilities
// =============================================================================

/**
 * @brief Get our compressed public key
 * @param ctx Crypto context
 * @param[out] public_key_out 33-byte buffer
 */
void crypto_get_our_public_key(crypto_context_t *ctx, uint8_t *public_key_out);

/**
 * @brief Check if session is established
 */
bool crypto_is_session_established(const crypto_context_t *ctx);

/**
 * @brief Reset session state (for reconnection)
 */
void crypto_reset_session(crypto_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif // REMOTE_CRYPTO_H

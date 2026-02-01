/**
 * @file remote_crypto.c
 * @brief Cryptographic operations implementation
 */

#include "remote_crypto.h"
#include <string.h>
#include <mbedtls/md.h>
#include <esp_log.h>
#include <esp_random.h>

static const char *TAG = "remote_crypto";

// HKDF info string (must match Rust server)
static const char *HKDF_INFO = "cardputer-remote-v1-session-keys";

// =============================================================================
// Internal Helpers
// =============================================================================

/**
 * @brief Compress an EC point to 33 bytes
 */
static int compress_point(const mbedtls_ecp_group *grp,
                          const mbedtls_ecp_point *pt,
                          uint8_t *out) {
    size_t olen;
    return mbedtls_ecp_point_write_binary(grp, pt,
                                          MBEDTLS_ECP_PF_COMPRESSED,
                                          &olen, out, ECDH_PUBLIC_KEY_SIZE);
}

/**
 * @brief Read compressed EC point
 */
static int read_compressed_point(mbedtls_ecp_group *grp,
                                 mbedtls_ecp_point *pt,
                                 const uint8_t *buf) {
    return mbedtls_ecp_point_read_binary(grp, pt, buf, ECDH_PUBLIC_KEY_SIZE);
}

/**
 * @brief Encode nonce to wire format (counter in big-endian)
 */
static void encode_nonce(const nonce_state_t *state, uint8_t *out) {
    out[0] = (state->counter >> 24) & 0xFF;
    out[1] = (state->counter >> 16) & 0xFF;
    out[2] = (state->counter >> 8) & 0xFF;
    out[3] = state->counter & 0xFF;
    memcpy(&out[4], state->random, 8);
}

/**
 * @brief Decode nonce from wire format
 */
static uint32_t decode_nonce_counter(const uint8_t *nonce) {
    return ((uint32_t)nonce[0] << 24) |
           ((uint32_t)nonce[1] << 16) |
           ((uint32_t)nonce[2] << 8) |
           (uint32_t)nonce[3];
}

// =============================================================================
// Initialization & Cleanup
// =============================================================================

remote_error_t crypto_init(crypto_context_t *ctx, const uint8_t *private_key) {
    int ret;

    memset(ctx, 0, sizeof(*ctx));

    // Initialize entropy and DRBG
    mbedtls_entropy_init(&ctx->entropy);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);

    ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mbedtls_entropy_func,
                                 &ctx->entropy, NULL, 0);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to seed DRBG: -0x%04X", -ret);
        return REMOTE_ERR_CRYPTO;
    }

    // Initialize ECDSA context with secp256r1
    mbedtls_ecdsa_init(&ctx->our_static_key);
    ret = mbedtls_ecp_group_load(&ctx->our_static_key.MBEDTLS_PRIVATE(grp),
                                  MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to load curve: -0x%04X", -ret);
        return REMOTE_ERR_CRYPTO;
    }

    // Load private key
    ret = mbedtls_mpi_read_binary(&ctx->our_static_key.MBEDTLS_PRIVATE(d),
                                   private_key, ECDH_PRIVATE_KEY_SIZE);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to load private key: -0x%04X", -ret);
        return REMOTE_ERR_CRYPTO;
    }

    // Compute public key from private key
    ret = mbedtls_ecp_mul(&ctx->our_static_key.MBEDTLS_PRIVATE(grp),
                          &ctx->our_static_key.MBEDTLS_PRIVATE(Q),
                          &ctx->our_static_key.MBEDTLS_PRIVATE(d),
                          &ctx->our_static_key.MBEDTLS_PRIVATE(grp).G,
                          mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to compute public key: -0x%04X", -ret);
        return REMOTE_ERR_CRYPTO;
    }

    // Initialize peer public key point
    mbedtls_ecp_point_init(&ctx->peer_static_public);

    // Initialize ECDH context
    mbedtls_ecdh_init(&ctx->ecdh);

    // Initialize GCM contexts
    mbedtls_gcm_init(&ctx->gcm_encrypt);
    mbedtls_gcm_init(&ctx->gcm_decrypt);

    // Generate random part for outgoing nonce
    esp_fill_random(ctx->outgoing_nonce.random, 8);
    ctx->outgoing_nonce.counter = 0;

    ESP_LOGI(TAG, "Crypto context initialized");
    return REMOTE_OK;
}

void crypto_free(crypto_context_t *ctx) {
    if (ctx == NULL) return;

    // Securely wipe sensitive data
    mbedtls_platform_zeroize(&ctx->session_keys, sizeof(ctx->session_keys));
    mbedtls_platform_zeroize(ctx->outgoing_nonce.random, 8);
    mbedtls_platform_zeroize(ctx->incoming_nonce_random, 8);

    mbedtls_gcm_free(&ctx->gcm_decrypt);
    mbedtls_gcm_free(&ctx->gcm_encrypt);
    mbedtls_ecdh_free(&ctx->ecdh);
    mbedtls_ecp_point_free(&ctx->peer_static_public);
    mbedtls_ecdsa_free(&ctx->our_static_key);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
    mbedtls_entropy_free(&ctx->entropy);

    memset(ctx, 0, sizeof(*ctx));
}

remote_error_t crypto_set_peer_public_key(crypto_context_t *ctx,
                                          const uint8_t *public_key) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_group_init(&grp);

    int ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
        mbedtls_ecp_group_free(&grp);
        return REMOTE_ERR_CRYPTO;
    }

    ret = read_compressed_point(&grp, &ctx->peer_static_public, public_key);
    mbedtls_ecp_group_free(&grp);

    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to read peer public key: -0x%04X", -ret);
        return REMOTE_ERR_CRYPTO;
    }

    ctx->peer_key_loaded = true;
    ESP_LOGI(TAG, "Peer public key loaded");
    return REMOTE_OK;
}

// =============================================================================
// Ephemeral Key Generation
// =============================================================================

remote_error_t crypto_generate_ephemeral_keypair(crypto_context_t *ctx,
                                                  uint8_t *public_key_out) {
    int ret;

    // Free any existing ECDH context
    mbedtls_ecdh_free(&ctx->ecdh);
    mbedtls_ecdh_init(&ctx->ecdh);

    // Setup ECDH with secp256r1
    ret = mbedtls_ecdh_setup(&ctx->ecdh, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to setup ECDH: -0x%04X", -ret);
        return REMOTE_ERR_CRYPTO;
    }

    // Generate ephemeral key pair
    ret = mbedtls_ecdh_gen_public(&ctx->ecdh.MBEDTLS_PRIVATE(grp),
                                   &ctx->ecdh.MBEDTLS_PRIVATE(d),
                                   &ctx->ecdh.MBEDTLS_PRIVATE(Q),
                                   mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to generate ephemeral keypair: -0x%04X", -ret);
        return REMOTE_ERR_CRYPTO;
    }

    // Compress public key
    ret = compress_point(&ctx->ecdh.MBEDTLS_PRIVATE(grp),
                         &ctx->ecdh.MBEDTLS_PRIVATE(Q),
                         public_key_out);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to compress public key: -0x%04X", -ret);
        return REMOTE_ERR_CRYPTO;
    }

    memcpy(ctx->our_ephemeral_public, public_key_out, ECDH_PUBLIC_KEY_SIZE);
    ESP_LOGI(TAG, "Generated ephemeral keypair");
    return REMOTE_OK;
}

remote_error_t crypto_generate_nonce(crypto_context_t *ctx,
                                     uint8_t *nonce_out) {
    int ret = mbedtls_ctr_drbg_random(&ctx->ctr_drbg, nonce_out,
                                       HANDSHAKE_NONCE_SIZE);
    if (ret != 0) {
        return REMOTE_ERR_CRYPTO;
    }
    return REMOTE_OK;
}

// =============================================================================
// Signatures
// =============================================================================

remote_error_t crypto_sign(crypto_context_t *ctx,
                           const uint8_t *data, size_t data_len,
                           uint8_t *signature_out) {
    uint8_t hash[32];
    int ret;

    // Hash the data (SHA-256)
    ret = mbedtls_sha256(data, data_len, hash, 0);
    if (ret != 0) {
        return REMOTE_ERR_CRYPTO;
    }

    // Sign the hash
    mbedtls_mpi r, s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    ret = mbedtls_ecdsa_sign(&ctx->our_static_key.MBEDTLS_PRIVATE(grp),
                              &r, &s,
                              &ctx->our_static_key.MBEDTLS_PRIVATE(d),
                              hash, sizeof(hash),
                              mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
    if (ret != 0) {
        mbedtls_mpi_free(&r);
        mbedtls_mpi_free(&s);
        ESP_LOGE(TAG, "Failed to sign: -0x%04X", -ret);
        return REMOTE_ERR_CRYPTO;
    }

    // Write r and s as 32-byte big-endian values
    ret = mbedtls_mpi_write_binary(&r, signature_out, 32);
    if (ret == 0) {
        ret = mbedtls_mpi_write_binary(&s, signature_out + 32, 32);
    }

    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);

    if (ret != 0) {
        return REMOTE_ERR_CRYPTO;
    }

    return REMOTE_OK;
}

remote_error_t crypto_verify_peer_signature(crypto_context_t *ctx,
                                            const uint8_t *data, size_t data_len,
                                            const uint8_t *signature) {
    if (!ctx->peer_key_loaded) {
        ESP_LOGE(TAG, "Peer public key not loaded");
        return REMOTE_ERR_SIGNATURE;
    }

    uint8_t hash[32];
    int ret;

    // Hash the data
    ret = mbedtls_sha256(data, data_len, hash, 0);
    if (ret != 0) {
        return REMOTE_ERR_CRYPTO;
    }

    // Parse r and s from signature
    mbedtls_mpi r, s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    ret = mbedtls_mpi_read_binary(&r, signature, 32);
    if (ret == 0) {
        ret = mbedtls_mpi_read_binary(&s, signature + 32, 32);
    }

    if (ret != 0) {
        mbedtls_mpi_free(&r);
        mbedtls_mpi_free(&s);
        return REMOTE_ERR_SIGNATURE;
    }

    // Need a group for verification
    mbedtls_ecp_group grp;
    mbedtls_ecp_group_init(&grp);
    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
        mbedtls_mpi_free(&r);
        mbedtls_mpi_free(&s);
        mbedtls_ecp_group_free(&grp);
        return REMOTE_ERR_CRYPTO;
    }

    // Verify signature
    ret = mbedtls_ecdsa_verify(&grp, hash, sizeof(hash),
                                &ctx->peer_static_public, &r, &s);

    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecp_group_free(&grp);

    if (ret != 0) {
        ESP_LOGE(TAG, "Signature verification failed: -0x%04X", -ret);
        return REMOTE_ERR_SIGNATURE;
    }

    return REMOTE_OK;
}

// =============================================================================
// Key Derivation
// =============================================================================

remote_error_t crypto_derive_session_keys(crypto_context_t *ctx,
                                          const uint8_t *peer_ephemeral_public,
                                          const uint8_t *our_nonce,
                                          const uint8_t *peer_nonce,
                                          bool is_server) {
    int ret;
    uint8_t shared_secret[32];
    size_t olen;

    // Read peer's ephemeral public key
    ret = read_compressed_point(&ctx->ecdh.MBEDTLS_PRIVATE(grp),
                                 &ctx->ecdh.MBEDTLS_PRIVATE(Qp),
                                 peer_ephemeral_public);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to read peer ephemeral key: -0x%04X", -ret);
        return REMOTE_ERR_CRYPTO;
    }

    // Compute shared secret
    ret = mbedtls_ecdh_calc_secret(&ctx->ecdh, &olen,
                                    shared_secret, sizeof(shared_secret),
                                    mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
    if (ret != 0) {
        mbedtls_platform_zeroize(shared_secret, sizeof(shared_secret));
        ESP_LOGE(TAG, "Failed to compute shared secret: -0x%04X", -ret);
        return REMOTE_ERR_CRYPTO;
    }

    // Compute salt = SHA256(client_nonce || server_nonce)
    const uint8_t *client_nonce = is_server ? peer_nonce : our_nonce;
    const uint8_t *server_nonce = is_server ? our_nonce : peer_nonce;

    uint8_t salt[32];
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);
    mbedtls_sha256_update(&sha_ctx, client_nonce, HANDSHAKE_NONCE_SIZE);
    mbedtls_sha256_update(&sha_ctx, server_nonce, HANDSHAKE_NONCE_SIZE);
    mbedtls_sha256_finish(&sha_ctx, salt);
    mbedtls_sha256_free(&sha_ctx);

    // Derive keys using HKDF
    // Output: 16 (c2s) + 16 (s2c) + 32 (hmac) = 64 bytes
    uint8_t okm[64];
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    ret = mbedtls_hkdf(md_info,
                       salt, sizeof(salt),
                       shared_secret, olen,
                       (const uint8_t *)HKDF_INFO, strlen(HKDF_INFO),
                       okm, sizeof(okm));

    mbedtls_platform_zeroize(shared_secret, sizeof(shared_secret));
    mbedtls_platform_zeroize(salt, sizeof(salt));

    if (ret != 0) {
        ESP_LOGE(TAG, "HKDF failed: -0x%04X", -ret);
        return REMOTE_ERR_CRYPTO;
    }

    // Split derived keys
    memcpy(ctx->session_keys.client_to_server_key, &okm[0], AES_KEY_SIZE);
    memcpy(ctx->session_keys.server_to_client_key, &okm[16], AES_KEY_SIZE);
    memcpy(ctx->session_keys.hmac_key, &okm[32], HMAC_KEY_SIZE);
    mbedtls_platform_zeroize(okm, sizeof(okm));

    // Setup GCM contexts
    // Client encrypts with c2s key, decrypts with s2c key
    const uint8_t *encrypt_key = is_server ?
        ctx->session_keys.server_to_client_key :
        ctx->session_keys.client_to_server_key;
    const uint8_t *decrypt_key = is_server ?
        ctx->session_keys.client_to_server_key :
        ctx->session_keys.server_to_client_key;

    ret = mbedtls_gcm_setkey(&ctx->gcm_encrypt, MBEDTLS_CIPHER_ID_AES,
                              encrypt_key, AES_KEY_SIZE * 8);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to set encrypt key: -0x%04X", -ret);
        return REMOTE_ERR_CRYPTO;
    }

    ret = mbedtls_gcm_setkey(&ctx->gcm_decrypt, MBEDTLS_CIPHER_ID_AES,
                              decrypt_key, AES_KEY_SIZE * 8);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to set decrypt key: -0x%04X", -ret);
        return REMOTE_ERR_CRYPTO;
    }

    // Reset nonce counters
    ctx->outgoing_nonce.counter = 0;
    esp_fill_random(ctx->outgoing_nonce.random, 8);

    // Set expected incoming nonce random from peer's handshake nonce
    memcpy(ctx->incoming_nonce_random, peer_nonce, 8);
    ctx->incoming_nonce_counter = 0;
    ctx->incoming_nonce_initialized = true;

    // Store handshake nonces for transcript
    memcpy(ctx->our_handshake_nonce, our_nonce, HANDSHAKE_NONCE_SIZE);
    memcpy(ctx->peer_handshake_nonce, peer_nonce, HANDSHAKE_NONCE_SIZE);

    ctx->session_established = true;
    ESP_LOGI(TAG, "Session keys derived successfully");
    return REMOTE_OK;
}

// =============================================================================
// Encryption/Decryption
// =============================================================================

remote_error_t crypto_encrypt(crypto_context_t *ctx,
                              const uint8_t *plaintext, size_t plaintext_len,
                              uint8_t *ciphertext,
                              uint8_t *nonce_out, uint8_t *tag_out) {
    if (!ctx->session_established) {
        return REMOTE_ERR_CRYPTO;
    }

    // Check for nonce overflow
    if (ctx->outgoing_nonce.counter == UINT32_MAX) {
        ESP_LOGE(TAG, "Nonce overflow - session must be renegotiated");
        return REMOTE_ERR_CRYPTO;
    }

    // Encode nonce
    uint8_t nonce[AES_GCM_NONCE_SIZE];
    encode_nonce(&ctx->outgoing_nonce, nonce);
    memcpy(nonce_out, nonce, AES_GCM_NONCE_SIZE);

    // Increment counter for next message
    ctx->outgoing_nonce.counter++;

    // Encrypt
    int ret = mbedtls_gcm_crypt_and_tag(&ctx->gcm_encrypt,
                                         MBEDTLS_GCM_ENCRYPT,
                                         plaintext_len,
                                         nonce, AES_GCM_NONCE_SIZE,
                                         NULL, 0,  // No AAD
                                         plaintext,
                                         ciphertext,
                                         AES_GCM_TAG_SIZE, tag_out);
    if (ret != 0) {
        ESP_LOGE(TAG, "Encryption failed: -0x%04X", -ret);
        return REMOTE_ERR_CRYPTO;
    }

    return REMOTE_OK;
}

remote_error_t crypto_decrypt(crypto_context_t *ctx,
                              const uint8_t *ciphertext, size_t ciphertext_len,
                              const uint8_t *nonce, const uint8_t *tag,
                              uint8_t *plaintext) {
    if (!ctx->session_established) {
        return REMOTE_ERR_CRYPTO;
    }

    // Validate nonce for replay protection
    uint32_t counter = decode_nonce_counter(nonce);

    // Check counter is strictly increasing
    if (counter < ctx->incoming_nonce_counter) {
        ESP_LOGW(TAG, "Replay detected: counter %lu < expected %lu",
                 (unsigned long)counter, (unsigned long)ctx->incoming_nonce_counter);
        return REMOTE_ERR_REPLAY;
    }

    // For counter == incoming_nonce_counter, only allow if it's the first packet
    if (counter == ctx->incoming_nonce_counter && ctx->incoming_nonce_counter > 0) {
        ESP_LOGW(TAG, "Replay detected: counter %lu already seen",
                 (unsigned long)counter);
        return REMOTE_ERR_REPLAY;
    }

    // Verify random part matches
    if (ctx->incoming_nonce_initialized) {
        if (memcmp(&nonce[4], ctx->incoming_nonce_random, 8) != 0) {
            ESP_LOGW(TAG, "Nonce random mismatch");
            return REMOTE_ERR_DECRYPT;
        }
    }

    // Decrypt and verify tag
    int ret = mbedtls_gcm_auth_decrypt(&ctx->gcm_decrypt,
                                        ciphertext_len,
                                        nonce, AES_GCM_NONCE_SIZE,
                                        NULL, 0,  // No AAD
                                        tag, AES_GCM_TAG_SIZE,
                                        ciphertext,
                                        plaintext);
    if (ret != 0) {
        if (ret == MBEDTLS_ERR_GCM_AUTH_FAILED) {
            ESP_LOGW(TAG, "Authentication failed - message tampered");
        } else {
            ESP_LOGE(TAG, "Decryption failed: -0x%04X", -ret);
        }
        return REMOTE_ERR_DECRYPT;
    }

    // Update expected counter (store counter + 1)
    ctx->incoming_nonce_counter = counter + 1;

    return REMOTE_OK;
}

// =============================================================================
// Transcript MAC
// =============================================================================

remote_error_t crypto_compute_transcript_mac(crypto_context_t *ctx,
                                             const uint8_t *transcript,
                                             size_t transcript_len,
                                             uint8_t *mac_out) {
    if (!ctx->session_established) {
        return REMOTE_ERR_CRYPTO;
    }

    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int ret = mbedtls_md_hmac(md_info,
                              ctx->session_keys.hmac_key, HMAC_KEY_SIZE,
                              transcript, transcript_len,
                              mac_out);
    if (ret != 0) {
        ESP_LOGE(TAG, "HMAC computation failed: -0x%04X", -ret);
        return REMOTE_ERR_CRYPTO;
    }

    return REMOTE_OK;
}

// =============================================================================
// Utilities
// =============================================================================

bool crypto_constant_time_eq(const uint8_t *a, const uint8_t *b, size_t len) {
    uint8_t result = 0;
    for (size_t i = 0; i < len; i++) {
        result |= a[i] ^ b[i];
    }
    return result == 0;
}

void crypto_get_our_public_key(crypto_context_t *ctx, uint8_t *public_key_out) {
    compress_point(&ctx->our_static_key.MBEDTLS_PRIVATE(grp),
                   &ctx->our_static_key.MBEDTLS_PRIVATE(Q),
                   public_key_out);
}

bool crypto_is_session_established(const crypto_context_t *ctx) {
    return ctx->session_established;
}

void crypto_reset_session(crypto_context_t *ctx) {
    // Wipe session keys
    mbedtls_platform_zeroize(&ctx->session_keys, sizeof(ctx->session_keys));

    // Reset GCM contexts
    mbedtls_gcm_free(&ctx->gcm_encrypt);
    mbedtls_gcm_free(&ctx->gcm_decrypt);
    mbedtls_gcm_init(&ctx->gcm_encrypt);
    mbedtls_gcm_init(&ctx->gcm_decrypt);

    // Generate new nonce random
    esp_fill_random(ctx->outgoing_nonce.random, 8);
    ctx->outgoing_nonce.counter = 0;
    ctx->incoming_nonce_counter = 0;
    ctx->incoming_nonce_initialized = false;

    ctx->session_established = false;
    ESP_LOGI(TAG, "Session reset");
}

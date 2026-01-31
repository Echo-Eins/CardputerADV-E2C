/*
 * remote_desktop.cpp - Remote Desktop Module for Evil-Cardputer
 *
 * Secure remote desktop client with FULL PKI implementation.
 *
 * Security features:
 * - ECDSA mutual authentication (secp256r1) with static keypairs
 * - ECDH ephemeral key exchange for forward secrecy
 * - HKDF-SHA256 key derivation (RFC 5869)
 * - AES-128-GCM authenticated encryption
 * - Dual session keys (client→server, server→client)
 * - Salt = SHA256(client_nonce || server_nonce)
 * - Replay protection with monotonic nonce counters
 * - Transcript MAC for handshake verification
 *
 * Key storage (SD card):
 * - /rd_keys/client.key  - Our ECDSA private key (32 bytes)
 * - /rd_keys/client.pub  - Our ECDSA public key (33 bytes compressed)
 * - /rd_keys/server.pub  - Server's ECDSA public key (33 bytes compressed)
 */

#include "remote_desktop.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <ESPmDNS.h>
#include <SD.h>
#include <ArduinoJson.h>

// MbedTLS for cryptography
#include "mbedtls/ecdh.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/bignum.h"
#include "mbedtls/platform_util.h"  // For mbedtls_platform_zeroize

// ============================================================================
// Constants matching Rust server protocol
// ============================================================================

static const char* RD_HKDF_INFO = "cardputer-remote-v1-session-keys";
static const size_t RD_HKDF_INFO_LEN = 33;

// ============================================================================
// Configuration
// ============================================================================

static RDConfig rdConfig = {
    "",                     // serverHost
    RD_DEFAULT_PORT,        // serverPort
    false,                  // autoConnect
    70,                     // jpegQuality
    10                      // targetFps
};

static const char* RD_CONFIG_PATH = "/remote_desktop.json";

// ============================================================================
// Static Keys (loaded from SD card)
// ============================================================================

static struct {
    bool loaded;
    uint8_t privateKey[RD_ECDH_PRIVKEY_SIZE];      // Our ECDSA private key
    uint8_t publicKey[RD_ECDH_PUBKEY_SIZE];        // Our ECDSA public key (compressed)
    uint8_t serverPublicKey[RD_ECDH_PUBKEY_SIZE];  // Server's ECDSA public key
    mbedtls_ecp_keypair ecdsaKey;                   // Our signing keypair
    mbedtls_ecp_point serverPubPoint;               // Server's public key point
} rdKeys;

// ============================================================================
// Session State (matches Rust server crypto state)
// ============================================================================

static struct {
    RDSessionState state;
    WiFiClient client;

    // Crypto contexts
    mbedtls_ecdh_context ecdh;
    mbedtls_gcm_context gcmEncrypt;     // For client→server (c2s)
    mbedtls_gcm_context gcmDecrypt;     // For server→client (s2c)
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;

    // Session keys (dual keys for bidirectional encryption)
    uint8_t c2sKey[RD_AES_KEY_SIZE];    // Client to server key
    uint8_t s2cKey[RD_AES_KEY_SIZE];    // Server to client key
    uint8_t hmacKey[RD_HMAC_KEY_SIZE];  // For transcript MAC

    // Nonces for key derivation
    uint8_t clientNonce[RD_HANDSHAKE_NONCE_SIZE];
    uint8_t serverNonce[RD_HANDSHAKE_NONCE_SIZE];

    // Nonce counters for replay protection (per direction)
    uint32_t txCounter;     // Our send counter
    uint32_t rxCounter;     // Expected receive counter
    uint8_t txNonceRandom[8];  // Random part of TX nonce
    uint8_t rxNonceRandom[8];  // Random part of RX nonce (from server)

    // Our ephemeral public key (compressed, 33 bytes)
    uint8_t ourEphemeralPubKey[RD_ECDH_PUBKEY_SIZE];

    // Frame buffer
    uint8_t* frameBuffer;
    size_t frameBufferSize;

    // Statistics
    uint32_t framesReceived;
    uint32_t lastFrameTime;
    float currentFps;

    // Connection info
    char serverName[64];
    IPAddress serverIP;

} rdSession;

// ============================================================================
// Forward declarations
// ============================================================================

static RDError rdDiscover();
static RDError rdConnect();
static RDError rdHandshake();
static void rdDisconnect();
static void rdLoop();
static void rdDrawStatus(const char* line1, const char* line2 = nullptr);
static void rdDrawFrame(const uint8_t* jpegData, size_t jpegLen);
static void rdProcessInput();
static RDError rdSendPacket(RDPacketType type, const uint8_t* payload, uint16_t len);
static RDError rdSendEncrypted(RDPacketType type, const uint8_t* payload, uint16_t len);
static RDError rdReceivePacketEx(RDPacketType* type, uint8_t* payload, uint16_t* len,
                                  uint8_t* tag, bool encrypted, uint32_t timeout);
static RDError rdReceivePacket(RDPacketType* type, uint8_t* payload, uint16_t* len, uint32_t timeout);
static void rdClearKeyboard();

// ============================================================================
// Configuration Load/Save
// ============================================================================

void rdLoadConfig() {
    if (!SD.exists(RD_CONFIG_PATH)) return;

    File f = SD.open(RD_CONFIG_PATH, FILE_READ);
    if (!f) return;

    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        strlcpy(rdConfig.serverHost, doc["host"] | "", sizeof(rdConfig.serverHost));
        rdConfig.serverPort = doc["port"] | RD_DEFAULT_PORT;
        rdConfig.autoConnect = doc["autoConnect"] | false;
        rdConfig.jpegQuality = doc["jpegQuality"] | 70;
        rdConfig.targetFps = doc["targetFps"] | 10;
    }
    f.close();
}

void rdSaveConfig() {
    File f = SD.open(RD_CONFIG_PATH, FILE_WRITE);
    if (!f) return;

    StaticJsonDocument<512> doc;
    doc["host"] = rdConfig.serverHost;
    doc["port"] = rdConfig.serverPort;
    doc["autoConnect"] = rdConfig.autoConnect;
    doc["jpegQuality"] = rdConfig.jpegQuality;
    doc["targetFps"] = rdConfig.targetFps;

    serializeJson(doc, f);
    f.close();
}

// ============================================================================
// Key Management - ECDSA keypair for mutual authentication
// ============================================================================

bool rdKeysExist() {
    return SD.exists(RD_PRIVKEY_PATH) &&
           SD.exists(RD_PUBKEY_PATH) &&
           SD.exists(RD_SERVER_PUBKEY_PATH);
}

bool rdGenerateKeyPair() {
    Serial.println("[RD] Generating new ECDSA keypair...");

    // Initialize RNG
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    const char* pers = "rd_keygen";
    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                     (const uint8_t*)pers, strlen(pers));
    if (ret != 0) {
        Serial.printf("[RD] RNG seed failed: -0x%04X\n", -ret);
        mbedtls_entropy_free(&entropy);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        return false;
    }

    // Generate keypair
    mbedtls_ecp_keypair keypair;
    mbedtls_ecp_keypair_init(&keypair);

    ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, &keypair,
                               mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) {
        Serial.printf("[RD] Key generation failed: -0x%04X\n", -ret);
        mbedtls_ecp_keypair_free(&keypair);
        mbedtls_entropy_free(&entropy);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        return false;
    }

    // Export private key (32 bytes)
    uint8_t privKey[RD_ECDH_PRIVKEY_SIZE];
    ret = mbedtls_mpi_write_binary(&keypair.d, privKey, RD_ECDH_PRIVKEY_SIZE);
    if (ret != 0) {
        Serial.printf("[RD] Export private key failed: -0x%04X\n", -ret);
        mbedtls_ecp_keypair_free(&keypair);
        mbedtls_entropy_free(&entropy);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        return false;
    }

    // Export public key (33 bytes compressed)
    uint8_t pubKey[RD_ECDH_PUBKEY_SIZE];
    size_t pubLen = 0;
    ret = mbedtls_ecp_point_write_binary(&keypair.grp, &keypair.Q,
                                          MBEDTLS_ECP_PF_COMPRESSED,
                                          &pubLen, pubKey, sizeof(pubKey));
    if (ret != 0 || pubLen != RD_ECDH_PUBKEY_SIZE) {
        Serial.printf("[RD] Export public key failed: -0x%04X\n", -ret);
        mbedtls_ecp_keypair_free(&keypair);
        mbedtls_entropy_free(&entropy);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        return false;
    }

    // Create directory if not exists
    if (!SD.exists(RD_KEYS_DIR)) {
        SD.mkdir(RD_KEYS_DIR);
    }

    // Save private key
    File f = SD.open(RD_PRIVKEY_PATH, FILE_WRITE);
    if (!f) {
        Serial.println("[RD] Failed to open private key file for writing");
        mbedtls_ecp_keypair_free(&keypair);
        mbedtls_entropy_free(&entropy);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        return false;
    }
    f.write(privKey, RD_ECDH_PRIVKEY_SIZE);
    f.close();

    // Save public key
    f = SD.open(RD_PUBKEY_PATH, FILE_WRITE);
    if (!f) {
        Serial.println("[RD] Failed to open public key file for writing");
        mbedtls_ecp_keypair_free(&keypair);
        mbedtls_entropy_free(&entropy);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        return false;
    }
    f.write(pubKey, RD_ECDH_PUBKEY_SIZE);
    f.close();

    // Clear sensitive data
    mbedtls_platform_zeroize(privKey, sizeof(privKey));
    mbedtls_ecp_keypair_free(&keypair);
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctr_drbg);

    // Print public key in hex for registration with server
    Serial.print("[RD] Generated public key (add to server config): ");
    for (int i = 0; i < RD_ECDH_PUBKEY_SIZE; i++) {
        Serial.printf("%02x", pubKey[i]);
    }
    Serial.println();

    return true;
}

bool rdLoadKeys() {
    if (rdKeys.loaded) return true;

    // Initialize structures
    mbedtls_ecp_keypair_init(&rdKeys.ecdsaKey);
    mbedtls_ecp_point_init(&rdKeys.serverPubPoint);

    // Load private key
    File f = SD.open(RD_PRIVKEY_PATH, FILE_READ);
    if (!f || f.size() != RD_ECDH_PRIVKEY_SIZE) {
        Serial.println("[RD] Failed to read private key");
        if (f) f.close();
        return false;
    }
    f.read(rdKeys.privateKey, RD_ECDH_PRIVKEY_SIZE);
    f.close();

    // Load our public key
    f = SD.open(RD_PUBKEY_PATH, FILE_READ);
    if (!f || f.size() != RD_ECDH_PUBKEY_SIZE) {
        Serial.println("[RD] Failed to read public key");
        if (f) f.close();
        return false;
    }
    f.read(rdKeys.publicKey, RD_ECDH_PUBKEY_SIZE);
    f.close();

    // Load server's public key
    f = SD.open(RD_SERVER_PUBKEY_PATH, FILE_READ);
    if (!f || f.size() != RD_ECDH_PUBKEY_SIZE) {
        Serial.println("[RD] Failed to read server public key");
        if (f) f.close();
        return false;
    }
    f.read(rdKeys.serverPublicKey, RD_ECDH_PUBKEY_SIZE);
    f.close();

    // Setup ECDSA keypair
    int ret = mbedtls_ecp_group_load(&rdKeys.ecdsaKey.grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
        Serial.printf("[RD] ECP group load failed: -0x%04X\n", -ret);
        return false;
    }

    // Import private key
    ret = mbedtls_mpi_read_binary(&rdKeys.ecdsaKey.d, rdKeys.privateKey, RD_ECDH_PRIVKEY_SIZE);
    if (ret != 0) {
        Serial.printf("[RD] Import private key failed: -0x%04X\n", -ret);
        return false;
    }

    // Import our public key
    ret = mbedtls_ecp_point_read_binary(&rdKeys.ecdsaKey.grp, &rdKeys.ecdsaKey.Q,
                                         rdKeys.publicKey, RD_ECDH_PUBKEY_SIZE);
    if (ret != 0) {
        Serial.printf("[RD] Import our public key failed: -0x%04X\n", -ret);
        return false;
    }

    // Verify keypair consistency
    ret = mbedtls_ecp_check_pub_priv(&rdKeys.ecdsaKey, &rdKeys.ecdsaKey);
    if (ret != 0) {
        Serial.printf("[RD] Keypair validation failed: -0x%04X\n", -ret);
        return false;
    }

    // Import server's public key
    mbedtls_ecp_group grp;
    mbedtls_ecp_group_init(&grp);
    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
        mbedtls_ecp_group_free(&grp);
        return false;
    }

    ret = mbedtls_ecp_point_read_binary(&grp, &rdKeys.serverPubPoint,
                                         rdKeys.serverPublicKey, RD_ECDH_PUBKEY_SIZE);
    mbedtls_ecp_group_free(&grp);
    if (ret != 0) {
        Serial.printf("[RD] Import server public key failed: -0x%04X\n", -ret);
        return false;
    }

    rdKeys.loaded = true;
    Serial.println("[RD] Keys loaded successfully");
    return true;
}

// ECDSA sign data using our private key
// Returns signature in r||s format (64 bytes)
static bool rdSign(const uint8_t* data, size_t dataLen, uint8_t* signature) {
    if (!rdKeys.loaded) return false;

    // Hash the data first (ECDSA signs the hash)
    uint8_t hash[32];
    mbedtls_sha256(data, dataLen, hash, 0);

    mbedtls_mpi r, s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    int ret = mbedtls_ecdsa_sign(&rdKeys.ecdsaKey.grp, &r, &s, &rdKeys.ecdsaKey.d,
                                  hash, sizeof(hash),
                                  mbedtls_ctr_drbg_random, &rdSession.ctr_drbg);
    if (ret != 0) {
        Serial.printf("[RD] ECDSA sign failed: -0x%04X\n", -ret);
        mbedtls_mpi_free(&r);
        mbedtls_mpi_free(&s);
        return false;
    }

    // Export r and s (each 32 bytes, big-endian, zero-padded)
    ret = mbedtls_mpi_write_binary(&r, signature, 32);
    if (ret != 0) {
        mbedtls_mpi_free(&r);
        mbedtls_mpi_free(&s);
        return false;
    }

    ret = mbedtls_mpi_write_binary(&s, signature + 32, 32);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);

    return ret == 0;
}

// Verify ECDSA signature from server
// Signature is in r||s format (64 bytes)
static bool rdVerifyServerSignature(const uint8_t* data, size_t dataLen, const uint8_t* signature) {
    if (!rdKeys.loaded) return false;

    // Hash the data
    uint8_t hash[32];
    mbedtls_sha256(data, dataLen, hash, 0);

    // Import r and s
    mbedtls_mpi r, s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    int ret = mbedtls_mpi_read_binary(&r, signature, 32);
    if (ret != 0) {
        mbedtls_mpi_free(&r);
        mbedtls_mpi_free(&s);
        return false;
    }

    ret = mbedtls_mpi_read_binary(&s, signature + 32, 32);
    if (ret != 0) {
        mbedtls_mpi_free(&r);
        mbedtls_mpi_free(&s);
        return false;
    }

    // Need a group for verification
    mbedtls_ecp_group grp;
    mbedtls_ecp_group_init(&grp);
    ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
        mbedtls_ecp_group_free(&grp);
        mbedtls_mpi_free(&r);
        mbedtls_mpi_free(&s);
        return false;
    }

    // Verify
    ret = mbedtls_ecdsa_verify(&grp, hash, sizeof(hash),
                                &rdKeys.serverPubPoint, &r, &s);

    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);

    if (ret != 0) {
        Serial.printf("[RD] Signature verification failed: -0x%04X\n", -ret);
        return false;
    }

    return true;
}

// ============================================================================
// Error/State strings
// ============================================================================

const char* rdErrorToString(RDError err) {
    switch (err) {
        case RD_OK:                 return "OK";
        case RD_ERR_NO_WIFI:        return "No WiFi";
        case RD_ERR_NO_SERVER:      return "No server found";
        case RD_ERR_CONNECT_FAILED: return "Connection failed";
        case RD_ERR_HANDSHAKE:      return "Handshake failed";
        case RD_ERR_CRYPTO:         return "Crypto error";
        case RD_ERR_TIMEOUT:        return "Timeout";
        case RD_ERR_PROTOCOL:       return "Protocol error";
        case RD_ERR_JPEG:           return "JPEG decode error";
        case RD_ERR_USER_CANCEL:    return "Cancelled";
        case RD_ERR_REPLAY:         return "Replay attack";
        case RD_ERR_NONCE_OVERFLOW: return "Nonce overflow";
        default:                    return "Unknown error";
    }
}

const char* rdStateToString(RDSessionState state) {
    switch (state) {
        case RD_STATE_DISCONNECTED: return "Disconnected";
        case RD_STATE_DISCOVERING:  return "Discovering...";
        case RD_STATE_CONNECTING:   return "Connecting...";
        case RD_STATE_HANDSHAKE:    return "Handshake...";
        case RD_STATE_CONNECTED:    return "Connected";
        case RD_STATE_ERROR:        return "Error";
        default:                    return "Unknown";
    }
}

// ============================================================================
// Crypto initialization
// ============================================================================

static RDError rdInitCrypto() {
    mbedtls_ecdh_init(&rdSession.ecdh);
    mbedtls_gcm_init(&rdSession.gcmEncrypt);
    mbedtls_gcm_init(&rdSession.gcmDecrypt);
    mbedtls_entropy_init(&rdSession.entropy);
    mbedtls_ctr_drbg_init(&rdSession.ctr_drbg);

    // Seed RNG with additional entropy
    const char* pers = "cardputer_rd_v2";
    int ret = mbedtls_ctr_drbg_seed(&rdSession.ctr_drbg, mbedtls_entropy_func,
                                     &rdSession.entropy, (const uint8_t*)pers, strlen(pers));
    if (ret != 0) {
        Serial.printf("[RD] RNG seed failed: -0x%04X\n", -ret);
        return RD_ERR_CRYPTO;
    }

    // Setup ECDH with secp256r1
    ret = mbedtls_ecp_group_load(&rdSession.ecdh.grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
        Serial.printf("[RD] ECP group load failed: -0x%04X\n", -ret);
        return RD_ERR_CRYPTO;
    }

    // Generate ephemeral keypair
    ret = mbedtls_ecdh_gen_public(&rdSession.ecdh.grp,
                                   &rdSession.ecdh.d,
                                   &rdSession.ecdh.Q,
                                   mbedtls_ctr_drbg_random,
                                   &rdSession.ctr_drbg);
    if (ret != 0) {
        Serial.printf("[RD] ECDH keygen failed: -0x%04X\n", -ret);
        return RD_ERR_CRYPTO;
    }

    // Export compressed public key (33 bytes)
    size_t pubKeyLen = 0;
    ret = mbedtls_ecp_point_write_binary(&rdSession.ecdh.grp,
                                          &rdSession.ecdh.Q,
                                          MBEDTLS_ECP_PF_COMPRESSED,
                                          &pubKeyLen, rdSession.ourEphemeralPubKey,
                                          sizeof(rdSession.ourEphemeralPubKey));
    if (ret != 0 || pubKeyLen != RD_ECDH_PUBKEY_SIZE) {
        Serial.printf("[RD] Export pubkey failed: -0x%04X, len=%zu\n", -ret, pubKeyLen);
        return RD_ERR_CRYPTO;
    }

    // Generate client nonce (32 bytes)
    ret = mbedtls_ctr_drbg_random(&rdSession.ctr_drbg,
                                   rdSession.clientNonce,
                                   RD_HANDSHAKE_NONCE_SIZE);
    if (ret != 0) {
        Serial.printf("[RD] Nonce generation failed: -0x%04X\n", -ret);
        return RD_ERR_CRYPTO;
    }

    // Generate random part of TX nonce
    ret = mbedtls_ctr_drbg_random(&rdSession.ctr_drbg, rdSession.txNonceRandom, 8);
    if (ret != 0) {
        return RD_ERR_CRYPTO;
    }

    rdSession.txCounter = 0;
    rdSession.rxCounter = 0;

    return RD_OK;
}

static void rdFreeCrypto() {
    // Securely clear sensitive data (compiler cannot optimize out)
    mbedtls_platform_zeroize(rdSession.c2sKey, sizeof(rdSession.c2sKey));
    mbedtls_platform_zeroize(rdSession.s2cKey, sizeof(rdSession.s2cKey));
    mbedtls_platform_zeroize(rdSession.hmacKey, sizeof(rdSession.hmacKey));
    mbedtls_platform_zeroize(rdSession.clientNonce, sizeof(rdSession.clientNonce));
    mbedtls_platform_zeroize(rdSession.serverNonce, sizeof(rdSession.serverNonce));
    mbedtls_platform_zeroize(rdSession.txNonceRandom, sizeof(rdSession.txNonceRandom));
    mbedtls_platform_zeroize(rdSession.rxNonceRandom, sizeof(rdSession.rxNonceRandom));

    mbedtls_ecdh_free(&rdSession.ecdh);
    mbedtls_gcm_free(&rdSession.gcmEncrypt);
    mbedtls_gcm_free(&rdSession.gcmDecrypt);
    mbedtls_entropy_free(&rdSession.entropy);
    mbedtls_ctr_drbg_free(&rdSession.ctr_drbg);
}

// ============================================================================
// HKDF key derivation (manual implementation - mbedtls_hkdf not in ESP32 Arduino)
// ============================================================================

// HKDF-Extract: PRK = HMAC-SHA256(salt, IKM)
static int hkdf_extract(const uint8_t* salt, size_t salt_len,
                        const uint8_t* ikm, size_t ikm_len,
                        uint8_t* prk) {
    return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                           salt, salt_len, ikm, ikm_len, prk);
}

// HKDF-Expand: OKM = T(1) || T(2) || ...
static int hkdf_expand(const uint8_t* prk, size_t prk_len,
                       const uint8_t* info, size_t info_len,
                       uint8_t* okm, size_t okm_len) {
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    size_t hash_len = 32;  // SHA256
    uint8_t t[32];
    size_t t_len = 0;
    uint8_t counter = 1;
    size_t offset = 0;

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);

    int ret = mbedtls_md_setup(&ctx, md, 1);  // 1 = use HMAC
    if (ret != 0) {
        mbedtls_md_free(&ctx);
        return ret;
    }

    while (offset < okm_len) {
        mbedtls_md_hmac_starts(&ctx, prk, prk_len);
        if (t_len > 0) {
            mbedtls_md_hmac_update(&ctx, t, t_len);
        }
        if (info_len > 0) {
            mbedtls_md_hmac_update(&ctx, info, info_len);
        }
        mbedtls_md_hmac_update(&ctx, &counter, 1);
        mbedtls_md_hmac_finish(&ctx, t);
        t_len = hash_len;

        size_t copy_len = (okm_len - offset < hash_len) ? (okm_len - offset) : hash_len;
        memcpy(okm + offset, t, copy_len);
        offset += copy_len;
        counter++;
    }

    mbedtls_md_free(&ctx);
    memset(t, 0, sizeof(t));  // Clear intermediate value
    return 0;
}

// Compute salt as SHA256(client_nonce || server_nonce)
static void rdComputeSalt(uint8_t* salt) {
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);  // 0 = SHA256 (not SHA224)
    mbedtls_sha256_update(&sha, rdSession.clientNonce, RD_HANDSHAKE_NONCE_SIZE);
    mbedtls_sha256_update(&sha, rdSession.serverNonce, RD_HANDSHAKE_NONCE_SIZE);
    mbedtls_sha256_finish(&sha, salt);
    mbedtls_sha256_free(&sha);
}

static RDError rdDeriveKeys(const uint8_t* sharedSecret, size_t secretLen) {
    // Compute salt = SHA256(client_nonce || server_nonce)
    uint8_t salt[RD_HKDF_SALT_SIZE];
    rdComputeSalt(salt);

    // Derive key material: c2s(16) + s2c(16) + hmac(32) = 64 bytes
    uint8_t keyMaterial[RD_SESSION_KEY_MATERIAL];
    uint8_t prk[32];

    // HKDF-Extract
    int ret = hkdf_extract(salt, sizeof(salt), sharedSecret, secretLen, prk);
    if (ret != 0) {
        Serial.printf("[RD] HKDF extract failed: -0x%04X\n", -ret);
        memset(salt, 0, sizeof(salt));
        return RD_ERR_CRYPTO;
    }

    // HKDF-Expand with correct info string
    ret = hkdf_expand(prk, sizeof(prk),
                      (const uint8_t*)RD_HKDF_INFO, RD_HKDF_INFO_LEN,
                      keyMaterial, sizeof(keyMaterial));

    memset(prk, 0, sizeof(prk));  // Clear PRK immediately
    memset(salt, 0, sizeof(salt));

    if (ret != 0) {
        Serial.printf("[RD] HKDF expand failed: -0x%04X\n", -ret);
        return RD_ERR_CRYPTO;
    }

    // Split key material: c2s_key(16) + s2c_key(16) + hmac_key(32)
    memcpy(rdSession.c2sKey, keyMaterial, RD_AES_KEY_SIZE);
    memcpy(rdSession.s2cKey, keyMaterial + RD_AES_KEY_SIZE, RD_AES_KEY_SIZE);
    memcpy(rdSession.hmacKey, keyMaterial + 2 * RD_AES_KEY_SIZE, RD_HMAC_KEY_SIZE);

    memset(keyMaterial, 0, sizeof(keyMaterial));  // Clear

    // Initialize GCM contexts with separate keys
    ret = mbedtls_gcm_setkey(&rdSession.gcmEncrypt, MBEDTLS_CIPHER_ID_AES,
                              rdSession.c2sKey, RD_AES_KEY_SIZE * 8);
    if (ret != 0) {
        Serial.printf("[RD] GCM encrypt setkey failed: -0x%04X\n", -ret);
        return RD_ERR_CRYPTO;
    }

    ret = mbedtls_gcm_setkey(&rdSession.gcmDecrypt, MBEDTLS_CIPHER_ID_AES,
                              rdSession.s2cKey, RD_AES_KEY_SIZE * 8);
    if (ret != 0) {
        Serial.printf("[RD] GCM decrypt setkey failed: -0x%04X\n", -ret);
        return RD_ERR_CRYPTO;
    }

    Serial.println("[RD] Session keys derived successfully");
    return RD_OK;
}

// ============================================================================
// Transcript MAC computation
// ============================================================================

static void rdComputeTranscriptMAC(const uint8_t* clientPubKey, const uint8_t* serverPubKey,
                                    uint8_t* mac) {
    // MAC = HMAC-SHA256(hmac_key, client_pubkey || server_pubkey || client_nonce || server_nonce)
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);

    mbedtls_md_hmac_starts(&ctx, rdSession.hmacKey, RD_HMAC_KEY_SIZE);
    mbedtls_md_hmac_update(&ctx, clientPubKey, RD_ECDH_PUBKEY_SIZE);
    mbedtls_md_hmac_update(&ctx, serverPubKey, RD_ECDH_PUBKEY_SIZE);
    mbedtls_md_hmac_update(&ctx, rdSession.clientNonce, RD_HANDSHAKE_NONCE_SIZE);
    mbedtls_md_hmac_update(&ctx, rdSession.serverNonce, RD_HANDSHAKE_NONCE_SIZE);
    mbedtls_md_hmac_finish(&ctx, mac);

    mbedtls_md_free(&ctx);
}



// ============================================================================
// Packet send/receive
// ============================================================================

// Send unencrypted packet: header(4) + payload + zero_tag(16)
static RDError rdSendPacket(RDPacketType type, const uint8_t* payload, uint16_t len) {
    if (!rdSession.client.connected()) return RD_ERR_CONNECT_FAILED;

    uint8_t header[RD_PACKET_HEADER_SIZE];
    header[0] = RD_PROTOCOL_VERSION;
    header[1] = type;
    header[2] = (len >> 8) & 0xFF;
    header[3] = len & 0xFF;

    if (rdSession.client.write(header, RD_PACKET_HEADER_SIZE) != RD_PACKET_HEADER_SIZE) {
        return RD_ERR_CONNECT_FAILED;
    }

    if (len > 0 && payload) {
        if (rdSession.client.write(payload, len) != len) {
            return RD_ERR_CONNECT_FAILED;
        }
    }

    // Send zero tag (required by protocol)
    static const uint8_t zeroTag[RD_AES_GCM_TAG_SIZE] = {0};
    if (rdSession.client.write(zeroTag, RD_AES_GCM_TAG_SIZE) != RD_AES_GCM_TAG_SIZE) {
        return RD_ERR_CONNECT_FAILED;
    }

    return RD_OK;
}

// Send encrypted packet (after handshake)
// Format matches Rust server: header + (nonce + ciphertext) + tag_separately
// Max outgoing payload: 256 bytes (commands are 2 bytes, transcript MAC is 32 bytes)
static const size_t RD_MAX_SEND_PAYLOAD = 256;

static RDError rdSendEncrypted(RDPacketType type, const uint8_t* payload, uint16_t len) {
    // Static buffer to avoid VLA stack overflow
    static uint8_t ciphertext[RD_MAX_SEND_PAYLOAD];

    if (!rdSession.client.connected()) return RD_ERR_CONNECT_FAILED;

    // Validate payload size
    if (len > RD_MAX_SEND_PAYLOAD) {
        Serial.printf("[RD] Payload too large: %u > %zu\n", len, RD_MAX_SEND_PAYLOAD);
        return RD_ERR_PROTOCOL;
    }

    // Check for nonce overflow
    if (rdSession.txCounter == 0xFFFFFFFF) {
        return RD_ERR_NONCE_OVERFLOW;
    }

    // Build nonce: counter(4 BE) + random(8)
    uint8_t nonce[RD_AES_GCM_NONCE_SIZE];
    nonce[0] = (rdSession.txCounter >> 24) & 0xFF;
    nonce[1] = (rdSession.txCounter >> 16) & 0xFF;
    nonce[2] = (rdSession.txCounter >> 8) & 0xFF;
    nonce[3] = rdSession.txCounter & 0xFF;
    memcpy(nonce + 4, rdSession.txNonceRandom, 8);
    rdSession.txCounter++;

    // Encrypt payload (using static buffer)
    uint8_t tag[RD_AES_GCM_TAG_SIZE];
    int ret = mbedtls_gcm_crypt_and_tag(&rdSession.gcmEncrypt, MBEDTLS_GCM_ENCRYPT,
                                         len, nonce, RD_AES_GCM_NONCE_SIZE,
                                         NULL, 0,  // No AAD
                                         payload, ciphertext,
                                         RD_AES_GCM_TAG_SIZE, tag);
    if (ret != 0) {
        Serial.printf("[RD] Encrypt failed: -0x%04X\n", -ret);
        return RD_ERR_CRYPTO;
    }

    // Send header with length = nonce + ciphertext (tag is separate!)
    uint16_t payloadLen = RD_AES_GCM_NONCE_SIZE + len;
    uint8_t header[RD_PACKET_HEADER_SIZE];
    header[0] = RD_PROTOCOL_VERSION;
    header[1] = type;
    header[2] = (payloadLen >> 8) & 0xFF;
    header[3] = payloadLen & 0xFF;

    if (rdSession.client.write(header, RD_PACKET_HEADER_SIZE) != RD_PACKET_HEADER_SIZE) {
        return RD_ERR_CONNECT_FAILED;
    }

    // Send nonce + ciphertext
    if (rdSession.client.write(nonce, RD_AES_GCM_NONCE_SIZE) != RD_AES_GCM_NONCE_SIZE) {
        return RD_ERR_CONNECT_FAILED;
    }
    if (len > 0 && rdSession.client.write(ciphertext, len) != len) {
        return RD_ERR_CONNECT_FAILED;
    }

    // Send tag separately (Rust server format)
    if (rdSession.client.write(tag, RD_AES_GCM_TAG_SIZE) != RD_AES_GCM_TAG_SIZE) {
        return RD_ERR_CONNECT_FAILED;
    }

    return RD_OK;
}

// Receive packet: header(4) + payload + tag(16)
// All packets have TAG (may be zero for unencrypted)
// encrypted parameter indicates if TAG should be returned for decryption
static RDError rdReceivePacketEx(RDPacketType* type, uint8_t* payload, uint16_t* len,
                                   uint8_t* tag, bool encrypted, uint32_t timeout) {
    unsigned long start = millis();

    // Wait for header
    while (rdSession.client.available() < RD_PACKET_HEADER_SIZE) {
        if (millis() - start > timeout) return RD_ERR_TIMEOUT;
        if (!rdSession.client.connected()) return RD_ERR_CONNECT_FAILED;
        delay(1);
    }

    uint8_t header[RD_PACKET_HEADER_SIZE];
    rdSession.client.readBytes(header, RD_PACKET_HEADER_SIZE);

    if (header[0] != RD_PROTOCOL_VERSION) {
        Serial.printf("[RD] Bad version: 0x%02X\n", header[0]);
        return RD_ERR_PROTOCOL;
    }

    *type = (RDPacketType)header[1];
    uint16_t payloadLen = ((uint16_t)header[2] << 8) | header[3];

    // All packets have TAG after payload
    size_t totalBytes = payloadLen + RD_AES_GCM_TAG_SIZE;

    // Wait for payload + tag
    while ((size_t)rdSession.client.available() < totalBytes) {
        if (millis() - start > timeout) return RD_ERR_TIMEOUT;
        if (!rdSession.client.connected()) return RD_ERR_CONNECT_FAILED;
        delay(1);
    }

    if (payloadLen > 0) {
        rdSession.client.readBytes(payload, payloadLen);
    }
    *len = payloadLen;

    // Always read TAG (protocol requires it)
    uint8_t readTag[RD_AES_GCM_TAG_SIZE];
    rdSession.client.readBytes(readTag, RD_AES_GCM_TAG_SIZE);

    // Return TAG if caller needs it for decryption
    if (encrypted && tag != NULL) {
        memcpy(tag, readTag, RD_AES_GCM_TAG_SIZE);
    }

    return RD_OK;
}

// Simple wrapper for unencrypted packets
static RDError rdReceivePacket(RDPacketType* type, uint8_t* payload, uint16_t* len, uint32_t timeout) {
    return rdReceivePacketEx(type, payload, len, NULL, false, timeout);
}

// ============================================================================
// mDNS Discovery with timeout
// ============================================================================

static RDError rdDiscover() {
    rdSession.state = RD_STATE_DISCOVERING;
    rdDrawStatus("Searching for server...", "via mDNS");

    if (!MDNS.begin("cardputer")) {
        Serial.println("[RD] mDNS init failed");
    }

    unsigned long start = millis();
    int n = 0;

    // Query with timeout
    while (millis() - start < RD_MDNS_TIMEOUT_MS) {
        n = MDNS.queryService("cardputer-remote", "tcp");
        if (n > 0) break;

        // Check for user cancel
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
            MDNS.end();
            return RD_ERR_USER_CANCEL;
        }

        delay(500);
    }

    if (n == 0) {
        Serial.println("[RD] No servers found via mDNS");
        MDNS.end();

        // Try manual entry or saved config
        if (strlen(rdConfig.serverHost) > 0) {
            rdSession.serverIP.fromString(rdConfig.serverHost);
            strlcpy(rdSession.serverName, rdConfig.serverHost, sizeof(rdSession.serverName));
            return RD_OK;
        }
        return RD_ERR_NO_SERVER;
    }

    // Use first found server
    rdSession.serverIP = MDNS.IP(0);
    strlcpy(rdSession.serverName, MDNS.hostname(0).c_str(), sizeof(rdSession.serverName));

    Serial.printf("[RD] Found server: %s at %s:%d\n",
                  rdSession.serverName,
                  rdSession.serverIP.toString().c_str(),
                  MDNS.port(0));

    if (MDNS.port(0) > 0) {
        rdConfig.serverPort = MDNS.port(0);
    }

    MDNS.end();
    return RD_OK;
}

// ============================================================================
// TCP Connection
// ============================================================================

static RDError rdConnect() {
    rdSession.state = RD_STATE_CONNECTING;

    char statusMsg[64];
    snprintf(statusMsg, sizeof(statusMsg), "to %s:%d",
             rdSession.serverIP.toString().c_str(), rdConfig.serverPort);
    rdDrawStatus("Connecting...", statusMsg);

    if (!rdSession.client.connect(rdSession.serverIP, rdConfig.serverPort)) {
        Serial.println("[RD] TCP connect failed");
        return RD_ERR_CONNECT_FAILED;
    }

    rdSession.client.setNoDelay(true);
    Serial.println("[RD] TCP connected");

    return RD_OK;
}

// ============================================================================
// Cryptographic Handshake (Full PKI with ECDSA signatures)
// ============================================================================

static RDError rdHandshake() {
    rdSession.state = RD_STATE_HANDSHAKE;
    rdDrawStatus("Handshake...", "Loading keys");

    // ===== LOAD KEYS =====
    if (!rdLoadKeys()) {
        Serial.println("[RD] Failed to load keys");
        return RD_ERR_CRYPTO;
    }

    rdDrawStatus("Handshake...", "ECDH + ECDSA");

    RDError err = rdInitCrypto();
    if (err != RD_OK) return err;

    // ===== BUILD HANDSHAKE INIT =====
    // Format: ephemeral_pubkey(33) + nonce(32) + signature(64) = 129 bytes
    // Signature covers: ephemeral_pubkey || nonce
    uint8_t initPayload[RD_HANDSHAKE_INIT_SIZE];

    // Copy ephemeral public key
    memcpy(initPayload, rdSession.ourEphemeralPubKey, RD_ECDH_PUBKEY_SIZE);
    // Copy client nonce
    memcpy(initPayload + RD_ECDH_PUBKEY_SIZE, rdSession.clientNonce, RD_HANDSHAKE_NONCE_SIZE);

    // Sign: ephemeral_pubkey || nonce
    uint8_t signData[RD_ECDH_PUBKEY_SIZE + RD_HANDSHAKE_NONCE_SIZE];
    memcpy(signData, rdSession.ourEphemeralPubKey, RD_ECDH_PUBKEY_SIZE);
    memcpy(signData + RD_ECDH_PUBKEY_SIZE, rdSession.clientNonce, RD_HANDSHAKE_NONCE_SIZE);

    if (!rdSign(signData, sizeof(signData), initPayload + RD_ECDH_PUBKEY_SIZE + RD_HANDSHAKE_NONCE_SIZE)) {
        Serial.println("[RD] Failed to sign handshake init");
        return RD_ERR_CRYPTO;
    }

    // Send with separate TAG (standard format)
    err = rdSendPacket(RD_PKT_HANDSHAKE_INIT, initPayload, sizeof(initPayload));
    if (err != RD_OK) {
        Serial.println("[RD] Failed to send HANDSHAKE_INIT");
        return err;
    }

    Serial.println("[RD] Sent signed HANDSHAKE_INIT");

    // ===== RECEIVE HANDSHAKE RESPONSE =====
    // Format: ephemeral_pubkey(33) + nonce(32) + signature(64) = 129 bytes
    uint8_t response[256];
    uint16_t respLen;
    RDPacketType respType;
    uint8_t tag[RD_AES_GCM_TAG_SIZE];

    // Receive with TAG (server sends standard format)
    err = rdReceivePacketEx(&respType, response, &respLen, tag, true, RD_HANDSHAKE_TIMEOUT_MS);
    if (err != RD_OK) {
        Serial.printf("[RD] Failed to receive response: %s\n", rdErrorToString(err));
        return err;
    }

    if (respType == RD_PKT_ERROR) {
        Serial.println("[RD] Server returned error");
        return RD_ERR_HANDSHAKE;
    }

    if (respType != RD_PKT_HANDSHAKE_RESPONSE) {
        Serial.printf("[RD] Expected HANDSHAKE_RESPONSE, got 0x%02X\n", respType);
        return RD_ERR_HANDSHAKE;
    }

    // Must have: pubkey(33) + nonce(32) + signature(64) = 129 bytes
    if (respLen < RD_HANDSHAKE_RESP_SIZE) {
        Serial.printf("[RD] Response too short: %u (need %u)\n", respLen, RD_HANDSHAKE_RESP_SIZE);
        return RD_ERR_HANDSHAKE;
    }

    // Extract components
    const uint8_t* serverEphemeralPubKey = response;
    const uint8_t* serverNonce = response + RD_ECDH_PUBKEY_SIZE;
    const uint8_t* serverSignature = response + RD_ECDH_PUBKEY_SIZE + RD_HANDSHAKE_NONCE_SIZE;

    // ===== VERIFY SERVER SIGNATURE =====
    // Server signs: server_ephemeral_pubkey || client_nonce || server_nonce
    uint8_t verifyData[RD_ECDH_PUBKEY_SIZE + RD_HANDSHAKE_NONCE_SIZE + RD_HANDSHAKE_NONCE_SIZE];
    memcpy(verifyData, serverEphemeralPubKey, RD_ECDH_PUBKEY_SIZE);
    memcpy(verifyData + RD_ECDH_PUBKEY_SIZE, rdSession.clientNonce, RD_HANDSHAKE_NONCE_SIZE);
    memcpy(verifyData + RD_ECDH_PUBKEY_SIZE + RD_HANDSHAKE_NONCE_SIZE, serverNonce, RD_HANDSHAKE_NONCE_SIZE);

    if (!rdVerifyServerSignature(verifyData, sizeof(verifyData), serverSignature)) {
        Serial.println("[RD] SERVER SIGNATURE VERIFICATION FAILED!");
        Serial.println("[RD] Possible MITM attack - aborting connection");
        return RD_ERR_HANDSHAKE;
    }

    Serial.println("[RD] Server signature verified");

    // Save server nonce
    memcpy(rdSession.serverNonce, serverNonce, RD_HANDSHAKE_NONCE_SIZE);

    // Import server's ephemeral public key (compressed format)
    int ret = mbedtls_ecp_point_read_binary(&rdSession.ecdh.grp,
                                             &rdSession.ecdh.Qp,
                                             serverEphemeralPubKey, RD_ECDH_PUBKEY_SIZE);
    if (ret != 0) {
        Serial.printf("[RD] Import server pubkey failed: -0x%04X\n", -ret);
        return RD_ERR_CRYPTO;
    }

    // Validate the point is on the curve
    ret = mbedtls_ecp_check_pubkey(&rdSession.ecdh.grp, &rdSession.ecdh.Qp);
    if (ret != 0) {
        Serial.printf("[RD] Server pubkey validation failed: -0x%04X\n", -ret);
        return RD_ERR_CRYPTO;
    }

    // ===== COMPUTE SHARED SECRET =====
    mbedtls_mpi sharedSecret;
    mbedtls_mpi_init(&sharedSecret);

    ret = mbedtls_ecdh_compute_shared(&rdSession.ecdh.grp,
                                       &sharedSecret,
                                       &rdSession.ecdh.Qp,
                                       &rdSession.ecdh.d,
                                       mbedtls_ctr_drbg_random,
                                       &rdSession.ctr_drbg);
    if (ret != 0) {
        mbedtls_mpi_free(&sharedSecret);
        Serial.printf("[RD] ECDH compute failed: -0x%04X\n", -ret);
        return RD_ERR_CRYPTO;
    }

    // Export shared secret (fixed 32 bytes with leading zeros)
    uint8_t secretBytes[32];
    memset(secretBytes, 0, sizeof(secretBytes));
    size_t secretLen = mbedtls_mpi_size(&sharedSecret);
    if (secretLen > 32) secretLen = 32;
    ret = mbedtls_mpi_write_binary(&sharedSecret, secretBytes + (32 - secretLen), secretLen);
    mbedtls_mpi_free(&sharedSecret);

    if (ret != 0) {
        Serial.printf("[RD] Export shared secret failed: -0x%04X\n", -ret);
        return RD_ERR_CRYPTO;
    }

    // ===== DERIVE SESSION KEYS =====
    err = rdDeriveKeys(secretBytes, 32);
    mbedtls_platform_zeroize(secretBytes, sizeof(secretBytes));
    if (err != RD_OK) return err;

    // ===== SEND ENCRYPTED HANDSHAKE COMPLETE =====
    // Compute transcript MAC
    uint8_t transcriptMAC[RD_TRANSCRIPT_MAC_SIZE];
    rdComputeTranscriptMAC(rdSession.ourEphemeralPubKey, serverEphemeralPubKey, transcriptMAC);

    // Send encrypted (using session keys)
    err = rdSendEncrypted(RD_PKT_HANDSHAKE_COMPLETE, transcriptMAC, sizeof(transcriptMAC));
    if (err != RD_OK) {
        Serial.println("[RD] Failed to send HANDSHAKE_COMPLETE");
        return err;
    }

    Serial.println("[RD] Sent encrypted HANDSHAKE_COMPLETE");

    // ===== WAIT FOR SESSION START =====
    err = rdReceivePacketEx(&respType, response, &respLen, tag, true, RD_HANDSHAKE_TIMEOUT_MS);
    if (err != RD_OK) {
        Serial.printf("[RD] No session start: %s\n", rdErrorToString(err));
        // Some server versions may not send this - continue
    } else if (respType == RD_PKT_SESSION_START) {
        Serial.println("[RD] Session started - secure channel established");
    } else if (respType == RD_PKT_ERROR) {
        Serial.println("[RD] Server rejected handshake");
        return RD_ERR_HANDSHAKE;
    }

    Serial.println("[RD] Handshake complete - secure channel established");
    rdSession.state = RD_STATE_CONNECTED;
    return RD_OK;
}

// ============================================================================
// Disconnect and cleanup
// ============================================================================

static void rdDisconnect() {
    if (rdSession.client.connected()) {
        // Try to send session end (ignore errors)
        rdSendPacket(RD_PKT_SESSION_END, NULL, 0);
        delay(50);
        rdSession.client.stop();
    }

    rdFreeCrypto();

    if (rdSession.frameBuffer) {
        free(rdSession.frameBuffer);
        rdSession.frameBuffer = NULL;
    }

    rdSession.state = RD_STATE_DISCONNECTED;
    Serial.println("[RD] Disconnected");
}

// ============================================================================
// Status display
// ============================================================================

static void rdDrawStatus(const char* line1, const char* line2) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(1.5);

    int y = 50;
    M5.Display.setCursor(10, y);
    M5.Display.println(line1);

    if (line2) {
        M5.Display.setCursor(10, y + 20);
        M5.Display.setTextColor(TFT_CYAN);
        M5.Display.println(line2);
    }

    M5.Display.setCursor(10, 115);
    M5.Display.setTextColor(TFT_DARKGREY);
    M5.Display.setTextSize(1);
    M5.Display.println("BACKSPACE to cancel");

    M5.Display.display();
}

// ============================================================================
// Frame rendering using M5GFX JPEG decoder
// ============================================================================

static void rdDrawFrame(const uint8_t* jpegData, size_t jpegLen) {
    // M5GFX has built-in JPEG support
    M5.Display.drawJpg(jpegData, jpegLen, 0, 0, RD_DISPLAY_WIDTH, RD_DISPLAY_HEIGHT);

    rdSession.framesReceived++;

    // Calculate FPS
    uint32_t now = millis();
    if (rdSession.lastFrameTime > 0) {
        uint32_t delta = now - rdSession.lastFrameTime;
        if (delta > 0) {
            rdSession.currentFps = 1000.0f / delta;
        }
    }
    rdSession.lastFrameTime = now;
}

// ============================================================================
// Clear keyboard state (for debounce fix)
// ============================================================================

static void rdClearKeyboard() {
    // Update multiple times to clear any pending key states
    for (int i = 0; i < 5; i++) {
        M5Cardputer.update();
        delay(20);
    }
}

// ============================================================================
// Input processing
// ============================================================================

static void rdProcessInput() {
    if (!M5Cardputer.Keyboard.isChange()) return;
    if (!M5Cardputer.Keyboard.isPressed()) return;

    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

    // Handle FN combinations for mouse control
    if (status.fn) {
        int8_t dx = 0, dy = 0;

        if (M5Cardputer.Keyboard.isKeyPressed(';')) dy = -10;  // Up
        if (M5Cardputer.Keyboard.isKeyPressed('.')) dy = 10;   // Down
        if (M5Cardputer.Keyboard.isKeyPressed(',')) dx = -10;  // Left
        if (M5Cardputer.Keyboard.isKeyPressed('/')) dx = 10;   // Right

        if (dx != 0 || dy != 0) {
            uint8_t data[2] = {(uint8_t)dx, (uint8_t)dy};
            rdSendEncrypted(RD_PKT_MOUSE_MOVE, data, 2);
        }

        if (status.enter) {
            uint8_t data[2] = {0, 2};  // Left button, click action
            rdSendEncrypted(RD_PKT_MOUSE_CLICK, data, 2);
        }
        return;
    }

    // Regular key presses - send as HID keycodes
    for (auto ch : status.word) {
        uint8_t keycode = 0;
        uint8_t modifier = 0;

        // ASCII to HID conversion
        if (ch >= 'a' && ch <= 'z') {
            keycode = 0x04 + (ch - 'a');
        } else if (ch >= 'A' && ch <= 'Z') {
            keycode = 0x04 + (ch - 'A');
            modifier = 0x02;  // Shift
        } else if (ch >= '1' && ch <= '9') {
            keycode = 0x1E + (ch - '1');
        } else if (ch == '0') {
            keycode = 0x27;
        } else if (ch == ' ') {
            keycode = 0x2C;
        } else if (ch == '\n' || ch == '\r') {
            keycode = 0x28;  // Enter
        }

        if (keycode != 0) {
            uint8_t data[2] = {keycode, modifier};
            rdSendEncrypted(RD_PKT_KEY_PRESS, data, 2);
            delay(10);
            rdSendEncrypted(RD_PKT_KEY_RELEASE, data, 2);
        }
    }

    // Special keys
    if (status.del) {
        uint8_t data[2] = {0x2A, 0};  // Backspace
        rdSendEncrypted(RD_PKT_KEY_PRESS, data, 2);
        delay(10);
        rdSendEncrypted(RD_PKT_KEY_RELEASE, data, 2);
    }

    if (status.enter && !status.fn) {
        uint8_t data[2] = {0x28, 0};  // Enter
        rdSendEncrypted(RD_PKT_KEY_PRESS, data, 2);
        delay(10);
        rdSendEncrypted(RD_PKT_KEY_RELEASE, data, 2);
    }
}

// ============================================================================
// Main session loop
// ============================================================================

static void rdLoop() {
    static uint8_t rxBuffer[32768];   // 32KB for receiving encrypted frames
    static uint8_t decBuffer[32768];  // 32KB for decrypted data
    static uint8_t tag[RD_AES_GCM_TAG_SIZE];
    static uint32_t lastHeartbeat = 0;

    while (rdSession.state == RD_STATE_CONNECTED) {
        M5Cardputer.update();
        M5.update();

        // Check for exit (FN + Backspace)
        if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE) &&
            M5Cardputer.Keyboard.isKeyPressed(KEY_FN)) {
            Serial.println("[RD] User requested disconnect");
            break;
        }

        // Process keyboard input
        rdProcessInput();

        // Check for incoming data (need at least header)
        if (rdSession.client.available() >= RD_PACKET_HEADER_SIZE) {
            RDPacketType type;
            uint16_t len;

            // All packets have TAG (zero for unencrypted, real for encrypted)
            // Always read TAG, then determine handling based on packet type
            RDError err = rdReceivePacketEx(&type, rxBuffer, &len, tag, true, 100);
            if (err != RD_OK) {
                if (err != RD_ERR_TIMEOUT) {
                    Serial.printf("[RD] Receive error: %s\n", rdErrorToString(err));
                    rdSession.state = RD_STATE_ERROR;
                    break;
                }
                continue;
            }

            switch (type) {
                case RD_PKT_SCREEN_FRAME: {
                    // Decrypt frame (payload = nonce + ciphertext, tag was read separately)
                    if (len < RD_AES_GCM_NONCE_SIZE) {
                        Serial.println("[RD] Frame too short");
                        break;
                    }

                    const uint8_t* nonce = rxBuffer;
                    const uint8_t* ciphertext = rxBuffer + RD_AES_GCM_NONCE_SIZE;
                    size_t ciphertextLen = len - RD_AES_GCM_NONCE_SIZE;

                    // Verify nonce counter (replay protection)
                    uint32_t counter = ((uint32_t)nonce[0] << 24) | ((uint32_t)nonce[1] << 16) |
                                       ((uint32_t)nonce[2] << 8) | nonce[3];
                    if (counter < rdSession.rxCounter) {
                        Serial.printf("[RD] Replay! Got %u, expected >= %u\n", counter, rdSession.rxCounter);
                        break;
                    }
                    if (rdSession.rxCounter == 0) {
                        memcpy(rdSession.rxNonceRandom, nonce + 4, 8);
                    }
                    rdSession.rxCounter = counter + 1;

                    // Decrypt using s2c key
                    int ret = mbedtls_gcm_auth_decrypt(&rdSession.gcmDecrypt, ciphertextLen,
                                                        nonce, RD_AES_GCM_NONCE_SIZE,
                                                        NULL, 0,
                                                        tag, RD_AES_GCM_TAG_SIZE,
                                                        ciphertext, decBuffer);
                    if (ret != 0) {
                        Serial.printf("[RD] Frame decrypt failed: -0x%04X\n", -ret);
                        break;
                    }

                    // Frame format: sequence(4) + timestamp(4) + jpeg_data
                    if (ciphertextLen > 8) {
                        rdDrawFrame(decBuffer + 8, ciphertextLen - 8);
                    }
                    break;
                }

                case RD_PKT_HEARTBEAT:
                    rdSendPacket(RD_PKT_HEARTBEAT_ACK, NULL, 0);
                    break;

                case RD_PKT_SESSION_END:
                    Serial.println("[RD] Server ended session");
                    rdSession.state = RD_STATE_DISCONNECTED;
                    break;

                case RD_PKT_SESSION_TIMEOUT:
                    Serial.println("[RD] Session timeout from server");
                    rdSession.state = RD_STATE_DISCONNECTED;
                    break;

                case RD_PKT_ERROR:
                    Serial.println("[RD] Server error packet received");
                    rdSession.state = RD_STATE_ERROR;
                    break;

                default:
                    Serial.printf("[RD] Unknown packet type: 0x%02X\n", type);
                    break;
            }
        }

        // Send heartbeat every 5 seconds
        if (millis() - lastHeartbeat > 5000) {
            rdSendPacket(RD_PKT_HEARTBEAT, NULL, 0);
            lastHeartbeat = millis();
        }

        // Small delay to prevent CPU hogging
        delay(1);
    }
}

// ============================================================================
// Settings menu
// ============================================================================

static void rdShowSettings() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(1.5);

    int y = 10;
    M5.Display.setCursor(10, y);
    M5.Display.println("Remote Desktop Settings");

    y += 25;
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.setCursor(10, y);
    M5.Display.printf("Server: %s", strlen(rdConfig.serverHost) ? rdConfig.serverHost : "(auto)");

    y += 15;
    M5.Display.setCursor(10, y);
    M5.Display.printf("Port: %d", rdConfig.serverPort);

    y += 15;
    M5.Display.setCursor(10, y);
    M5.Display.printf("Quality: %d%%", rdConfig.jpegQuality);

    y += 15;
    M5.Display.setCursor(10, y);
    M5.Display.printf("FPS: %d", rdConfig.targetFps);

    y += 25;
    M5.Display.setTextColor(TFT_GREEN);
    M5.Display.setCursor(10, y);
    M5.Display.println("ENTER: Connect");

    M5.Display.setTextColor(TFT_DARKGREY);
    M5.Display.setCursor(10, y + 15);
    M5.Display.println("S: Set server  Q: Quality");

    M5.Display.setCursor(10, y + 30);
    M5.Display.println("BACKSPACE: Back");

    M5.Display.display();
}

// ============================================================================
// Main entry point
// ============================================================================

void remoteDesktop() {
    inMenu = false;

    // Check WiFi
    if (WiFi.status() != WL_CONNECTED) {
        waitAndReturnToMenu("WiFi not connected!");
        return;
    }

    rdLoadConfig();

    // Initialize session state
    memset(&rdSession, 0, sizeof(rdSession));
    rdSession.state = RD_STATE_DISCONNECTED;

    // === UI DEBOUNCE FIX ===
    // Clear keyboard state and wait to prevent immediate action
    rdClearKeyboard();
    delay(300);  // Extra delay to ensure menu is shown first

    // Show settings / connect menu
    rdShowSettings();

    while (true) {
        M5Cardputer.update();
        M5.update();

        // Exit
        if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
            delay(200);  // Debounce
            break;
        }

        // Set server manually
        if (M5Cardputer.Keyboard.isKeyPressed('s') || M5Cardputer.Keyboard.isKeyPressed('S')) {
            delay(200);
            M5.Display.fillScreen(TFT_BLACK);
            M5.Display.setCursor(10, 10);
            M5.Display.println("Enter server IP:");
            String host = getUserInput(false);
            if (host.length() > 0) {
                strlcpy(rdConfig.serverHost, host.c_str(), sizeof(rdConfig.serverHost));
                rdSaveConfig();
            }
            rdClearKeyboard();
            rdShowSettings();
            continue;
        }

        // Set quality
        if (M5Cardputer.Keyboard.isKeyPressed('q') || M5Cardputer.Keyboard.isKeyPressed('Q')) {
            delay(200);
            M5.Display.fillScreen(TFT_BLACK);
            M5.Display.setCursor(10, 10);
            M5.Display.println("Enter quality (1-100):");
            String q = getUserInput(false);
            int qval = q.toInt();
            if (qval >= 1 && qval <= 100) {
                rdConfig.jpegQuality = qval;
                rdSaveConfig();
            }
            rdClearKeyboard();
            rdShowSettings();
            continue;
        }

        // Connect
        if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
            delay(200);  // Debounce

            RDError err;

            // Discover server
            err = rdDiscover();
            if (err != RD_OK) {
                waitAndReturnToMenu(rdErrorToString(err));
                break;
            }

            // Connect
            err = rdConnect();
            if (err != RD_OK) {
                rdDisconnect();
                waitAndReturnToMenu(rdErrorToString(err));
                break;
            }

            // Handshake
            err = rdHandshake();
            if (err != RD_OK) {
                rdDisconnect();
                waitAndReturnToMenu(rdErrorToString(err));
                break;
            }

            // Request first frame
            rdSendPacket(RD_PKT_SCREEN_REQUEST, NULL, 0);

            // Main loop
            rdLoop();

            // Cleanup
            rdDisconnect();

            // Clear keyboard and return to settings
            rdClearKeyboard();
            rdShowSettings();
        }

        delay(50);
    }

    // Return to main menu
    inMenu = true;
}

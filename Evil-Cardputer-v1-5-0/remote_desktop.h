/*
 * remote_desktop.h - Remote Desktop Module for Evil-Cardputer
 *
 * Secure remote desktop client with:
 * - ECDH key exchange (secp256r1) with uncompressed public keys
 * - HKDF-SHA256 key derivation (RFC 5869)
 * - AES-128-GCM authenticated encryption
 * - Dual session keys (client→server, server→client)
 * - Replay protection with nonce validation
 * - mDNS service discovery
 * - JPEG compressed screen streaming
 *
 * Protocol compatible with cardputer-remote Rust server
 */

#ifndef REMOTE_DESKTOP_H
#define REMOTE_DESKTOP_H

#include <Arduino.h>
#include <WiFi.h>

// ============================================================================
// External dependencies from main file
// ============================================================================

extern void waitAndReturnToMenu(String message);
extern String getUserInput(bool isPassword);
extern bool inMenu;
extern bool ledOn;
extern bool soundOn;

// ============================================================================
// Protocol Constants (must match Rust server)
// ============================================================================

#define RD_PROTOCOL_VERSION     0x01
#define RD_DEFAULT_PORT         19847
#define RD_SERVICE_TYPE         "_cardputer-remote._tcp.local."

// Crypto constants
#define RD_AES_KEY_SIZE         16      // AES-128
#define RD_AES_GCM_TAG_SIZE     16      // GCM authentication tag
#define RD_AES_GCM_NONCE_SIZE   12      // 96-bit nonce (4 counter + 8 random)
#define RD_ECDH_PUBKEY_SIZE     65      // Uncompressed secp256r1 point (0x04 prefix)
                                        // ESP32 Arduino mbedtls lacks POINT_COMPRESSION
                                        // so compressed (33-byte) format cannot be parsed
#define RD_ECDH_PRIVKEY_SIZE    32      // secp256r1 private key
#define RD_ECDSA_SIG_SIZE       64      // ECDSA signature (r + s, 32 bytes each)
#define RD_HANDSHAKE_NONCE_SIZE 32      // Random nonce for key derivation
#define RD_HKDF_SALT_SIZE       32      // SHA256 output size
#define RD_HMAC_KEY_SIZE        32      // HMAC-SHA256 key
#define RD_COOKIE_SIZE          16      // mDNS discovery cookie
#define RD_TRANSCRIPT_MAC_SIZE  32      // Handshake verification MAC

// Handshake message sizes (with signatures)
#define RD_HANDSHAKE_INIT_SIZE  (RD_ECDH_PUBKEY_SIZE + RD_HANDSHAKE_NONCE_SIZE + RD_ECDSA_SIG_SIZE)  // 65+32+64 = 161
#define RD_HANDSHAKE_RESP_SIZE  (RD_ECDH_PUBKEY_SIZE + RD_HANDSHAKE_NONCE_SIZE + RD_ECDSA_SIG_SIZE)  // 161

// Derived key material sizes
#define RD_SESSION_KEY_MATERIAL 64      // c2s(16) + s2c(16) + hmac(32)

// Packet structure
#define RD_PACKET_HEADER_SIZE   4       // version(1) + type(1) + length(2)

// Display (M5Stack Cardputer)
#define RD_DISPLAY_WIDTH        240
#define RD_DISPLAY_HEIGHT       135

// Timeouts
#define RD_MDNS_TIMEOUT_MS      10000   // mDNS discovery timeout (server re-announces every 5s)
#define RD_HANDSHAKE_TIMEOUT_MS 10000   // Handshake timeout
#define RD_RECEIVE_TIMEOUT_MS   5000    // General receive timeout

// ============================================================================
// Packet Types (must match Rust server protocol/mod.rs)
// ============================================================================

enum RDPacketType : uint8_t {
    // Discovery
    RD_PKT_DISCOVERY_REQUEST    = 0x00,
    RD_PKT_DISCOVERY_RESPONSE   = 0x01,

    // Handshake
    RD_PKT_HANDSHAKE_INIT       = 0x02,
    RD_PKT_HANDSHAKE_RESPONSE   = 0x03,
    RD_PKT_HANDSHAKE_COMPLETE   = 0x04,

    // Session
    RD_PKT_SESSION_START        = 0x10,
    RD_PKT_SESSION_END          = 0x11,
    RD_PKT_SESSION_TIMEOUT      = 0x12,
    RD_PKT_HEARTBEAT            = 0x13,
    RD_PKT_HEARTBEAT_ACK        = 0x14,

    // Screen
    RD_PKT_SCREEN_FRAME         = 0x20,
    RD_PKT_SCREEN_DELTA         = 0x21,
    RD_PKT_SCREEN_REQUEST       = 0x22,

    // Input
    RD_PKT_MOUSE_MOVE           = 0x30,
    RD_PKT_MOUSE_CLICK          = 0x31,
    RD_PKT_KEY_PRESS            = 0x32,
    RD_PKT_KEY_RELEASE          = 0x33,
    RD_PKT_KEY_TYPE             = 0x34,
    RD_PKT_MOUSE_SCROLL        = 0x35,

    // Mode
    RD_PKT_MODE_SWITCH          = 0x40,
    RD_PKT_MODE_ACK             = 0x41,

    // Error
    RD_PKT_ERROR                = 0xF0,
};

// ============================================================================
// Error Codes
// ============================================================================

enum RDError : int8_t {
    RD_OK                   = 0,
    RD_ERR_NO_WIFI          = -1,
    RD_ERR_NO_SERVER        = -2,
    RD_ERR_CONNECT_FAILED   = -3,
    RD_ERR_HANDSHAKE        = -4,
    RD_ERR_CRYPTO           = -5,
    RD_ERR_TIMEOUT          = -6,
    RD_ERR_PROTOCOL         = -7,
    RD_ERR_JPEG             = -8,
    RD_ERR_USER_CANCEL      = -9,
    RD_ERR_REPLAY           = -10,
    RD_ERR_NONCE_OVERFLOW   = -11,
    RD_ERR_REJECTED         = -12,    // User rejected connection
    RD_ERR_NO_COOKIE        = -13,    // Cookie file missing
};

// ============================================================================
// Key file paths on SD card
// ============================================================================

#define RD_KEYS_DIR             "/rd_keys"
#define RD_PRIVKEY_PATH         "/rd_keys/client.key"      // 32 bytes binary
#define RD_PUBKEY_PATH          "/rd_keys/client.pub"      // 65 bytes uncompressed
#define RD_SERVER_PUBKEY_PATH   "/rd_keys/server.pub"      // 65 bytes uncompressed
#define RD_COOKIE_PATH          "/rd_keys/cookie"          // 16 bytes binary

// ============================================================================
// Configuration (saved to SD card)
// ============================================================================

struct RDConfig {
    char serverHost[64];
    uint16_t serverPort;
    bool autoConnect;
    uint8_t jpegQuality;        // 1-100, server-side
    uint8_t targetFps;          // 1-30
    char mdnsServiceType[64];   // Full service type, e.g. "_cardputer-remote._tcp.local."
};

// ============================================================================
// Key Management Functions
// ============================================================================

// Generate new ECDSA keypair and save to SD card
// Returns true on success
bool rdGenerateKeyPair();

// Load keys from SD card
// Returns true if all keys loaded successfully
bool rdLoadKeys();

// Check if keys exist on SD card
bool rdKeysExist();

// Check if discovery cookie exists on SD card
bool rdCookieExists();

// ============================================================================
// Session State
// ============================================================================

enum RDSessionState : uint8_t {
    RD_STATE_DISCONNECTED = 0,
    RD_STATE_DISCOVERING,
    RD_STATE_CONNECTING,
    RD_STATE_HANDSHAKE,
    RD_STATE_CONNECTED,
    RD_STATE_ERROR,
};

// ============================================================================
// Public Functions
// ============================================================================

// Main entry point - called from menu
void remoteDesktop();

// Configuration
void rdLoadConfig();
void rdSaveConfig();

// Utility
const char* rdErrorToString(RDError err);
const char* rdStateToString(RDSessionState state);

#endif // REMOTE_DESKTOP_H

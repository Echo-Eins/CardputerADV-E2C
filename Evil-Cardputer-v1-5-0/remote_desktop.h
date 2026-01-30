/*
 * remote_desktop.h - Remote Desktop Module for Evil-Cardputer
 *
 * Secure remote desktop client with:
 * - ECDH key exchange (secp256r1)
 * - AES-128-GCM encryption
 * - mDNS service discovery
 * - JPEG compressed screen streaming
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
// Protocol Constants
// ============================================================================

#define RD_PROTOCOL_VERSION     0x01
#define RD_DEFAULT_PORT         19847
#define RD_SERVICE_TYPE         "_cardputer-remote._tcp"

// Crypto constants
#define RD_AES_KEY_SIZE         16      // AES-128
#define RD_AES_GCM_TAG_SIZE     16      // GCM tag
#define RD_AES_GCM_NONCE_SIZE   12      // 96-bit nonce
#define RD_ECDH_PUBKEY_SIZE     65      // Uncompressed point
#define RD_HKDF_SALT_SIZE       32
#define RD_HMAC_KEY_SIZE        32
#define RD_COOKIE_SIZE          16      // mDNS cookie

// Packet structure
#define RD_PACKET_HEADER_SIZE   4       // version(1) + type(1) + length(2)

// Display (M5Stack Cardputer)
#define RD_DISPLAY_WIDTH        240
#define RD_DISPLAY_HEIGHT       135

// ============================================================================
// Packet Types
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
    RD_PKT_HEARTBEAT            = 0x13,
    RD_PKT_HEARTBEAT_ACK        = 0x14,

    // Screen
    RD_PKT_SCREEN_FRAME         = 0x20,
    RD_PKT_SCREEN_REQUEST       = 0x22,

    // Input
    RD_PKT_MOUSE_MOVE           = 0x30,
    RD_PKT_MOUSE_CLICK          = 0x31,
    RD_PKT_KEY_PRESS            = 0x32,
    RD_PKT_KEY_RELEASE          = 0x33,

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
};

// ============================================================================
// Configuration (saved to SD card)
// ============================================================================

struct RDConfig {
    char serverHost[64];
    uint16_t serverPort;
    bool autoConnect;
    uint8_t jpegQuality;        // 1-100, server-side
    uint8_t targetFps;          // 1-30
};

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

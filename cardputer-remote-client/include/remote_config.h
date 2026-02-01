/**
 * @file remote_config.h
 * @brief Configuration and constants for Cardputer Remote Client
 *
 * Protocol: secp256r1 ECDH + AES-128-GCM + HKDF-SHA256
 * Compatible with cardputer-remote Rust server v1.0
 */

#ifndef REMOTE_CONFIG_H
#define REMOTE_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Protocol Constants
// =============================================================================

#define PROTOCOL_VERSION        0x01
#define PROTOCOL_MAGIC          "CPRT"  // Cardputer Remote

// Cryptographic constants
#define AES_KEY_SIZE            16      // AES-128
#define AES_GCM_TAG_SIZE        16      // GCM authentication tag
#define AES_GCM_NONCE_SIZE      12      // 96-bit nonce
#define ECDH_PRIVATE_KEY_SIZE   32      // secp256r1
#define ECDH_PUBLIC_KEY_SIZE    33      // Compressed point
#define ECDSA_SIGNATURE_SIZE    64      // r || s
#define HKDF_SALT_SIZE          32      // SHA-256 output
#define HMAC_KEY_SIZE           32      // For transcript MAC
#define DISCOVERY_COOKIE_SIZE   16      // mDNS validation cookie
#define HANDSHAKE_NONCE_SIZE    32      // Random nonce for handshake

// Packet structure
#define PACKET_HEADER_SIZE      4       // version(1) + type(1) + length(2)
#define PACKET_MAX_PAYLOAD      65535   // 2-byte length field
#define PACKET_OVERHEAD         (PACKET_HEADER_SIZE + AES_GCM_NONCE_SIZE + AES_GCM_TAG_SIZE)

// Display constants (M5Stack Cardputer)
#define DISPLAY_WIDTH           240
#define DISPLAY_HEIGHT          135
#define DISPLAY_BPP             16      // RGB565
#define DISPLAY_BUFFER_SIZE     (DISPLAY_WIDTH * DISPLAY_HEIGHT * 2)

// Network constants
#define REMOTE_DEFAULT_PORT     19847
#define MDNS_SERVICE_TYPE       "_cardputer-remote._tcp"
#define TCP_RX_BUFFER_SIZE      32768   // 32KB receive buffer
#define TCP_TX_BUFFER_SIZE      4096    // 4KB transmit buffer
#define HEARTBEAT_INTERVAL_MS   5000
#define SESSION_TIMEOUT_MS      30000

// Input constants
#define INPUT_QUEUE_SIZE        32
#define MOUSE_MOVE_STEP         5       // Pixels per arrow key

// =============================================================================
// Packet Types (must match Rust server)
// =============================================================================

typedef enum {
    // Discovery (0x00-0x0F)
    PKT_DISCOVERY_REQUEST   = 0x00,
    PKT_DISCOVERY_RESPONSE  = 0x01,

    // Handshake (0x02-0x0F)
    PKT_HANDSHAKE_INIT      = 0x02,
    PKT_HANDSHAKE_RESPONSE  = 0x03,
    PKT_HANDSHAKE_COMPLETE  = 0x04,

    // Session (0x10-0x1F)
    PKT_SESSION_START       = 0x10,
    PKT_SESSION_END         = 0x11,
    PKT_SESSION_TIMEOUT     = 0x12,
    PKT_HEARTBEAT           = 0x13,
    PKT_HEARTBEAT_ACK       = 0x14,

    // Screen (0x20-0x2F)
    PKT_SCREEN_FRAME        = 0x20,
    PKT_SCREEN_DELTA        = 0x21,
    PKT_SCREEN_REQUEST      = 0x22,

    // Input (0x30-0x3F)
    PKT_MOUSE_MOVE          = 0x30,
    PKT_MOUSE_CLICK         = 0x31,
    PKT_KEY_PRESS           = 0x32,
    PKT_KEY_RELEASE         = 0x33,
    PKT_KEY_TYPE            = 0x34,

    // Mode (0x40-0x4F)
    PKT_MODE_SWITCH         = 0x40,
    PKT_MODE_ACK            = 0x41,

    // Error (0xF0-0xFF)
    PKT_ERROR               = 0xF0,
} packet_type_t;

// =============================================================================
// Input Modes
// =============================================================================

typedef enum {
    INPUT_MODE_MOUSE    = 0,
    INPUT_MODE_KEYBOARD = 1,
} input_mode_t;

// =============================================================================
// Mouse Buttons
// =============================================================================

typedef enum {
    MOUSE_BUTTON_LEFT   = 0,
    MOUSE_BUTTON_RIGHT  = 1,
    MOUSE_BUTTON_MIDDLE = 2,
} mouse_button_t;

// =============================================================================
// Connection States
// =============================================================================

typedef enum {
    CONN_STATE_DISCONNECTED = 0,
    CONN_STATE_DISCOVERING,
    CONN_STATE_CONNECTING,
    CONN_STATE_HANDSHAKE_INIT,
    CONN_STATE_HANDSHAKE_RESPONSE,
    CONN_STATE_HANDSHAKE_COMPLETE,
    CONN_STATE_ESTABLISHED,
    CONN_STATE_ERROR,
} connection_state_t;

// =============================================================================
// Error Codes
// =============================================================================

typedef enum {
    REMOTE_OK = 0,
    REMOTE_ERR_INVALID_PARAM,
    REMOTE_ERR_NO_MEMORY,
    REMOTE_ERR_NETWORK,
    REMOTE_ERR_TIMEOUT,
    REMOTE_ERR_HANDSHAKE_FAILED,
    REMOTE_ERR_CRYPTO,
    REMOTE_ERR_PROTOCOL,
    REMOTE_ERR_SIGNATURE,
    REMOTE_ERR_REPLAY,
    REMOTE_ERR_DECRYPT,
    REMOTE_ERR_DISPLAY,
    REMOTE_ERR_JPEG,
} remote_error_t;

// =============================================================================
// Data Structures
// =============================================================================

/**
 * @brief Packet header (4 bytes, unencrypted)
 */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t type;
    uint16_t length;    // Big-endian, payload length (excluding header/nonce/tag)
} packet_header_t;

/**
 * @brief Session keys derived from ECDH
 */
typedef struct {
    uint8_t client_to_server_key[AES_KEY_SIZE];
    uint8_t server_to_client_key[AES_KEY_SIZE];
    uint8_t hmac_key[HMAC_KEY_SIZE];
} session_keys_t;

/**
 * @brief Nonce structure: 4-byte counter + 8-byte random
 */
typedef struct {
    uint32_t counter;       // Big-endian on wire
    uint8_t random[8];
} nonce_state_t;

/**
 * @brief Mouse move event
 */
typedef struct __attribute__((packed)) {
    int8_t dx;
    int8_t dy;
} mouse_move_t;

/**
 * @brief Mouse click event
 */
typedef struct __attribute__((packed)) {
    uint8_t button;         // mouse_button_t
    uint8_t pressed;        // 1 = down, 0 = up
} mouse_click_t;

/**
 * @brief Key event
 */
typedef struct __attribute__((packed)) {
    uint8_t keycode;        // USB HID keycode
    uint8_t modifiers;      // Modifier bits
} key_event_t;

/**
 * @brief Screen frame header
 */
typedef struct __attribute__((packed)) {
    uint32_t sequence;      // Frame sequence number
    uint32_t timestamp;     // Server timestamp (ms)
    // JPEG data follows
} screen_frame_header_t;

/**
 * @brief Handshake init payload
 */
typedef struct __attribute__((packed)) {
    uint8_t ephemeral_public_key[ECDH_PUBLIC_KEY_SIZE];
    uint8_t nonce[HANDSHAKE_NONCE_SIZE];
    uint8_t signature[ECDSA_SIGNATURE_SIZE];
} handshake_init_t;

/**
 * @brief Handshake response payload
 */
typedef struct __attribute__((packed)) {
    uint8_t ephemeral_public_key[ECDH_PUBLIC_KEY_SIZE];
    uint8_t nonce[HANDSHAKE_NONCE_SIZE];
    uint8_t signature[ECDSA_SIGNATURE_SIZE];
} handshake_response_t;

/**
 * @brief Handshake complete payload
 */
typedef struct __attribute__((packed)) {
    uint8_t transcript_mac[32];
} handshake_complete_t;

/**
 * @brief Configuration structure
 */
typedef struct {
    // Network
    char wifi_ssid[33];
    char wifi_password[65];
    char server_host[64];
    uint16_t server_port;

    // Security
    uint8_t discovery_cookie[DISCOVERY_COOKIE_SIZE];
    uint8_t private_key[ECDH_PRIVATE_KEY_SIZE];
    uint8_t server_public_key[ECDH_PUBLIC_KEY_SIZE];

    // Display
    uint8_t brightness;
    bool auto_sleep;
    uint32_t sleep_timeout_ms;
} remote_config_t;

/**
 * @brief Runtime statistics
 */
typedef struct {
    uint32_t frames_received;
    uint32_t bytes_received;
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t decode_errors;
    uint32_t crypto_errors;
    uint32_t last_frame_time_ms;
    float avg_fps;
    float avg_latency_ms;
} remote_stats_t;

#ifdef __cplusplus
}
#endif

#endif // REMOTE_CONFIG_H

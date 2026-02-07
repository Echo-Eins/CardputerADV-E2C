/*
 * llm_gateway.h - LLM Gateway Client for Evil-Cardputer
 *
 * Secure client for connecting to TUI LLM Gateway with:
 * - ECDSA-P256 challenge-response authentication
 * - Session token management
 * - Multi-turn chat support
 * - Model selection
 * - Scrollable chat UI
 *
 * Protocol: HTTP/1.1 over TCP (no TLS - designed for local network)
 * Gateway default port: 52525
 */

#ifndef LLM_GATEWAY_H
#define LLM_GATEWAY_H

#include <Arduino.h>
#include <WiFi.h>

// ============================================================================
// External dependencies from main file
// ============================================================================

extern void waitAndReturnToMenu(String message);
extern String getUserInput(bool isPassword);
extern bool inMenu;

// ============================================================================
// Protocol Constants
// ============================================================================

#define LLM_GW_DEFAULT_PORT         52525
#define LLM_GW_HTTP_TIMEOUT_MS      10000
#define LLM_GW_CHAT_TIMEOUT_MS      120000  // 2 min for LLM response

// Crypto constants (same as remote_desktop)
#define LLM_GW_ECDSA_PRIVKEY_SIZE   32      // secp256r1 private key
#define LLM_GW_ECDSA_PUBKEY_SIZE    65      // Uncompressed public key (0x04 prefix)
#define LLM_GW_ECDSA_SIG_SIZE       64      // r || s (32 bytes each)
#define LLM_GW_CHALLENGE_SIZE       32      // Server challenge
#define LLM_GW_NONCE_SIZE           16      // Server nonce

// Display constants (M5Stack Cardputer)
#define LLM_GW_SCREEN_WIDTH         240
#define LLM_GW_SCREEN_HEIGHT        135
#define LLM_GW_CHAR_WIDTH           6
#define LLM_GW_LINE_HEIGHT          12
#define LLM_GW_LINES_PER_PAGE       10
#define LLM_GW_CHARS_PER_LINE       38      // 240 / 6 - 2 margin

// ============================================================================
// Error Codes
// ============================================================================

enum LLMGWError : int8_t {
    LLM_GW_OK                       = 0,
    LLM_GW_ERR_NO_WIFI              = -1,
    LLM_GW_ERR_NO_SERVER            = -2,
    LLM_GW_ERR_CONNECT_FAILED       = -3,
    LLM_GW_ERR_HTTP_ERROR           = -4,
    LLM_GW_ERR_AUTH_FAILED          = -5,
    LLM_GW_ERR_SIGNATURE_FAILED     = -6,
    LLM_GW_ERR_SESSION_EXPIRED      = -7,
    LLM_GW_ERR_CHAT_FAILED          = -8,
    LLM_GW_ERR_TIMEOUT              = -9,
    LLM_GW_ERR_PARSE_ERROR          = -10,
    LLM_GW_ERR_NO_KEYS              = -11,
    LLM_GW_ERR_USER_CANCEL          = -12,
    LLM_GW_ERR_OLLAMA_UNAVAILABLE   = -13,
    LLM_GW_ERR_MODEL_NOT_FOUND      = -14,
};

// ============================================================================
// Key file paths on SD card - uses same keys as Remote Desktop
// ============================================================================

#define LLM_GW_KEYS_DIR             "/rd_keys"
#define LLM_GW_PRIVKEY_PATH         "/rd_keys/client.key"       // 32 bytes binary
#define LLM_GW_PUBKEY_PATH          "/rd_keys/client.pub"       // 65 bytes uncompressed
#define LLM_GW_SERVER_PUBKEY_PATH   "/rd_keys/server.pub"       // 65 bytes uncompressed (same as RDP)
#define LLM_GW_CONFIG_PATH          "/llm_gateway.json"

// ============================================================================
// Configuration
// ============================================================================

struct LLMGWConfig {
    char serverHost[64];            // Gateway server hostname/IP
    uint16_t serverPort;            // Gateway port (default 52525)
    char selectedModel[64];         // Currently selected model
    bool autoReconnect;             // Auto reconnect on session expiry
};

// ============================================================================
// Session State
// ============================================================================

enum LLMGWState : uint8_t {
    LLM_GW_STATE_DISCONNECTED = 0,
    LLM_GW_STATE_CONNECTING,
    LLM_GW_STATE_AUTHENTICATING,
    LLM_GW_STATE_READY,             // Authenticated, can start chat
    LLM_GW_STATE_CHATTING,          // In active chat session
    LLM_GW_STATE_ERROR,
};

// ============================================================================
// Model Information
// ============================================================================

struct LLMModel {
    char name[64];
    char displayName[64];
    bool isLocal;                   // true = Ollama local, false = cloud
    uint64_t size;                  // Model size in bytes (for local)
};

// ============================================================================
// Public Functions
// ============================================================================

// Main entry point - called from menu
void llmGatewayChat();

// Key Management (uses RDP keys from /rd_keys/)
bool llmGWLoadKeys();
bool llmGWKeysExist();

// Configuration
void llmGWLoadConfig();
void llmGWSaveConfig();

// Utility
const char* llmGWErrorToString(LLMGWError err);
const char* llmGWStateToString(LLMGWState state);

#endif // LLM_GATEWAY_H

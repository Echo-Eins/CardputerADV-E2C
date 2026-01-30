/*
 * remote_desktop.cpp - Remote Desktop Module for Evil-Cardputer
 *
 * Secure remote desktop client implementation
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
#include "mbedtls/ecp.h"
#include "mbedtls/gcm.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"

// ============================================================================
// Configuration
// ============================================================================

static RDConfig rdConfig = {
    .serverHost = "",
    .serverPort = RD_DEFAULT_PORT,
    .autoConnect = false,
    .jpegQuality = 70,
    .targetFps = 10,
};

static const char* RD_CONFIG_PATH = "/remote_desktop.json";

// ============================================================================
// Session State
// ============================================================================

static struct {
    RDSessionState state;
    WiFiClient client;

    // Crypto context
    mbedtls_ecdh_context ecdh;
    mbedtls_gcm_context gcm;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;

    // Session keys
    uint8_t aesKey[RD_AES_KEY_SIZE];
    uint8_t hmacKey[RD_HMAC_KEY_SIZE];

    // Nonce counters
    uint32_t txNonce;
    uint32_t rxNonce;
    uint8_t nonceRandom[8];

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
static RDError rdReceivePacket(RDPacketType* type, uint8_t* payload, uint16_t* len, uint32_t timeout);
static RDError rdEncrypt(const uint8_t* plain, size_t plainLen, uint8_t* cipher, size_t* cipherLen);
static RDError rdDecrypt(const uint8_t* cipher, size_t cipherLen, uint8_t* plain, size_t* plainLen);

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
    mbedtls_gcm_init(&rdSession.gcm);
    mbedtls_entropy_init(&rdSession.entropy);
    mbedtls_ctr_drbg_init(&rdSession.ctr_drbg);

    // Seed RNG
    const char* pers = "cardputer_rd";
    int ret = mbedtls_ctr_drbg_seed(&rdSession.ctr_drbg, mbedtls_entropy_func,
                                     &rdSession.entropy, (const uint8_t*)pers, strlen(pers));
    if (ret != 0) {
        Serial.printf("[RD] RNG seed failed: %d\n", ret);
        return RD_ERR_CRYPTO;
    }

    // Setup ECDH with secp256r1
    ret = mbedtls_ecp_group_load(&rdSession.ecdh.ctx.mbed_ecdh.grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
        Serial.printf("[RD] ECP group load failed: %d\n", ret);
        return RD_ERR_CRYPTO;
    }

    // Generate ephemeral keypair
    ret = mbedtls_ecdh_gen_public(&rdSession.ecdh.ctx.mbed_ecdh.grp,
                                   &rdSession.ecdh.ctx.mbed_ecdh.d,
                                   &rdSession.ecdh.ctx.mbed_ecdh.Q,
                                   mbedtls_ctr_drbg_random,
                                   &rdSession.ctr_drbg);
    if (ret != 0) {
        Serial.printf("[RD] ECDH keygen failed: %d\n", ret);
        return RD_ERR_CRYPTO;
    }

    return RD_OK;
}

static void rdFreeCrypto() {
    mbedtls_ecdh_free(&rdSession.ecdh);
    mbedtls_gcm_free(&rdSession.gcm);
    mbedtls_entropy_free(&rdSession.entropy);
    mbedtls_ctr_drbg_free(&rdSession.ctr_drbg);
}

// ============================================================================
// HKDF key derivation
// ============================================================================

static RDError rdDeriveKeys(const uint8_t* sharedSecret, size_t secretLen,
                            const uint8_t* salt, size_t saltLen) {
    uint8_t keyMaterial[RD_AES_KEY_SIZE + RD_HMAC_KEY_SIZE];

    int ret = mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                           salt, saltLen,
                           sharedSecret, secretLen,
                           (const uint8_t*)"cardputer-remote-v1", 19,
                           keyMaterial, sizeof(keyMaterial));
    if (ret != 0) {
        Serial.printf("[RD] HKDF failed: %d\n", ret);
        return RD_ERR_CRYPTO;
    }

    memcpy(rdSession.aesKey, keyMaterial, RD_AES_KEY_SIZE);
    memcpy(rdSession.hmacKey, keyMaterial + RD_AES_KEY_SIZE, RD_HMAC_KEY_SIZE);

    // Initialize GCM with derived key
    ret = mbedtls_gcm_setkey(&rdSession.gcm, MBEDTLS_CIPHER_ID_AES,
                              rdSession.aesKey, RD_AES_KEY_SIZE * 8);
    if (ret != 0) {
        Serial.printf("[RD] GCM setkey failed: %d\n", ret);
        return RD_ERR_CRYPTO;
    }

    // Generate random part of nonce
    mbedtls_ctr_drbg_random(&rdSession.ctr_drbg, rdSession.nonceRandom, 8);
    rdSession.txNonce = 0;
    rdSession.rxNonce = 0;

    return RD_OK;
}

// ============================================================================
// Encryption/Decryption
// ============================================================================

static RDError rdEncrypt(const uint8_t* plain, size_t plainLen,
                         uint8_t* cipher, size_t* cipherLen) {
    // Build nonce: counter(4) + random(8)
    uint8_t nonce[RD_AES_GCM_NONCE_SIZE];
    nonce[0] = (rdSession.txNonce >> 24) & 0xFF;
    nonce[1] = (rdSession.txNonce >> 16) & 0xFF;
    nonce[2] = (rdSession.txNonce >> 8) & 0xFF;
    nonce[3] = rdSession.txNonce & 0xFF;
    memcpy(nonce + 4, rdSession.nonceRandom, 8);
    rdSession.txNonce++;

    // Output: nonce + ciphertext + tag
    memcpy(cipher, nonce, RD_AES_GCM_NONCE_SIZE);

    uint8_t tag[RD_AES_GCM_TAG_SIZE];
    int ret = mbedtls_gcm_crypt_and_tag(&rdSession.gcm, MBEDTLS_GCM_ENCRYPT,
                                         plainLen, nonce, RD_AES_GCM_NONCE_SIZE,
                                         NULL, 0,
                                         plain, cipher + RD_AES_GCM_NONCE_SIZE,
                                         RD_AES_GCM_TAG_SIZE, tag);
    if (ret != 0) {
        return RD_ERR_CRYPTO;
    }

    memcpy(cipher + RD_AES_GCM_NONCE_SIZE + plainLen, tag, RD_AES_GCM_TAG_SIZE);
    *cipherLen = RD_AES_GCM_NONCE_SIZE + plainLen + RD_AES_GCM_TAG_SIZE;

    return RD_OK;
}

static RDError rdDecrypt(const uint8_t* cipher, size_t cipherLen,
                         uint8_t* plain, size_t* plainLen) {
    if (cipherLen < RD_AES_GCM_NONCE_SIZE + RD_AES_GCM_TAG_SIZE) {
        return RD_ERR_PROTOCOL;
    }

    const uint8_t* nonce = cipher;
    size_t dataLen = cipherLen - RD_AES_GCM_NONCE_SIZE - RD_AES_GCM_TAG_SIZE;
    const uint8_t* ciphertext = cipher + RD_AES_GCM_NONCE_SIZE;
    const uint8_t* tag = cipher + cipherLen - RD_AES_GCM_TAG_SIZE;

    // Verify nonce counter (replay protection)
    uint32_t counter = (nonce[0] << 24) | (nonce[1] << 16) | (nonce[2] << 8) | nonce[3];
    if (counter < rdSession.rxNonce) {
        Serial.println("[RD] Replay detected!");
        return RD_ERR_CRYPTO;
    }
    rdSession.rxNonce = counter + 1;

    int ret = mbedtls_gcm_auth_decrypt(&rdSession.gcm, dataLen,
                                        nonce, RD_AES_GCM_NONCE_SIZE,
                                        NULL, 0,
                                        tag, RD_AES_GCM_TAG_SIZE,
                                        ciphertext, plain);
    if (ret != 0) {
        Serial.printf("[RD] GCM decrypt failed: %d\n", ret);
        return RD_ERR_CRYPTO;
    }

    *plainLen = dataLen;
    return RD_OK;
}

// ============================================================================
// Packet send/receive
// ============================================================================

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

    return RD_OK;
}

static RDError rdReceivePacket(RDPacketType* type, uint8_t* payload, uint16_t* len, uint32_t timeout) {
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
        Serial.printf("[RD] Bad version: %02X\n", header[0]);
        return RD_ERR_PROTOCOL;
    }

    *type = (RDPacketType)header[1];
    uint16_t payloadLen = (header[2] << 8) | header[3];

    // Wait for payload
    while (rdSession.client.available() < payloadLen) {
        if (millis() - start > timeout) return RD_ERR_TIMEOUT;
        if (!rdSession.client.connected()) return RD_ERR_CONNECT_FAILED;
        delay(1);
    }

    if (payloadLen > 0) {
        rdSession.client.readBytes(payload, payloadLen);
    }
    *len = payloadLen;

    return RD_OK;
}

// ============================================================================
// mDNS Discovery
// ============================================================================

static RDError rdDiscover() {
    rdSession.state = RD_STATE_DISCOVERING;
    rdDrawStatus("Searching for server...", "via mDNS");

    if (!MDNS.begin("cardputer")) {
        Serial.println("[RD] mDNS init failed");
    }

    // Query for service
    int n = MDNS.queryService("cardputer-remote", "tcp");

    if (n == 0) {
        Serial.println("[RD] No servers found via mDNS");
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
// Connection
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
// Handshake
// ============================================================================

static RDError rdHandshake() {
    rdSession.state = RD_STATE_HANDSHAKE;
    rdDrawStatus("Handshake...", "Key exchange");

    RDError err = rdInitCrypto();
    if (err != RD_OK) return err;

    // Get our public key
    uint8_t ourPubKey[RD_ECDH_PUBKEY_SIZE];
    size_t pubKeyLen = 0;
    int ret = mbedtls_ecp_point_write_binary(&rdSession.ecdh.ctx.mbed_ecdh.grp,
                                              &rdSession.ecdh.ctx.mbed_ecdh.Q,
                                              MBEDTLS_ECP_PF_UNCOMPRESSED,
                                              &pubKeyLen, ourPubKey, sizeof(ourPubKey));
    if (ret != 0) {
        Serial.printf("[RD] Export pubkey failed: %d\n", ret);
        return RD_ERR_CRYPTO;
    }

    // Send HANDSHAKE_INIT with our public key
    err = rdSendPacket(RD_PKT_HANDSHAKE_INIT, ourPubKey, pubKeyLen);
    if (err != RD_OK) return err;

    // Receive HANDSHAKE_RESPONSE with server's public key
    uint8_t response[256];
    uint16_t respLen;
    RDPacketType respType;

    err = rdReceivePacket(&respType, response, &respLen, 5000);
    if (err != RD_OK) return err;

    if (respType != RD_PKT_HANDSHAKE_RESPONSE) {
        Serial.printf("[RD] Expected HANDSHAKE_RESPONSE, got %02X\n", respType);
        return RD_ERR_HANDSHAKE;
    }

    if (respLen < RD_ECDH_PUBKEY_SIZE) {
        Serial.println("[RD] Response too short");
        return RD_ERR_HANDSHAKE;
    }

    // Import server's public key
    ret = mbedtls_ecp_point_read_binary(&rdSession.ecdh.ctx.mbed_ecdh.grp,
                                         &rdSession.ecdh.ctx.mbed_ecdh.Qp,
                                         response, RD_ECDH_PUBKEY_SIZE);
    if (ret != 0) {
        Serial.printf("[RD] Import server pubkey failed: %d\n", ret);
        return RD_ERR_CRYPTO;
    }

    // Compute shared secret
    mbedtls_mpi sharedSecret;
    mbedtls_mpi_init(&sharedSecret);

    ret = mbedtls_ecdh_compute_shared(&rdSession.ecdh.ctx.mbed_ecdh.grp,
                                       &sharedSecret,
                                       &rdSession.ecdh.ctx.mbed_ecdh.Qp,
                                       &rdSession.ecdh.ctx.mbed_ecdh.d,
                                       mbedtls_ctr_drbg_random,
                                       &rdSession.ctr_drbg);
    if (ret != 0) {
        mbedtls_mpi_free(&sharedSecret);
        Serial.printf("[RD] ECDH compute failed: %d\n", ret);
        return RD_ERR_CRYPTO;
    }

    // Export shared secret
    uint8_t secretBytes[32];
    size_t secretLen = mbedtls_mpi_size(&sharedSecret);
    mbedtls_mpi_write_binary(&sharedSecret, secretBytes, secretLen);
    mbedtls_mpi_free(&sharedSecret);

    // Get salt from response (after pubkey)
    const uint8_t* salt = response + RD_ECDH_PUBKEY_SIZE;
    size_t saltLen = respLen - RD_ECDH_PUBKEY_SIZE;

    // Derive session keys
    err = rdDeriveKeys(secretBytes, secretLen, salt, saltLen);
    memset(secretBytes, 0, sizeof(secretBytes));  // Clear secret
    if (err != RD_OK) return err;

    // Send HANDSHAKE_COMPLETE
    err = rdSendPacket(RD_PKT_HANDSHAKE_COMPLETE, NULL, 0);
    if (err != RD_OK) return err;

    Serial.println("[RD] Handshake complete");
    rdSession.state = RD_STATE_CONNECTED;
    return RD_OK;
}

// ============================================================================
// Disconnect
// ============================================================================

static void rdDisconnect() {
    if (rdSession.client.connected()) {
        rdSendPacket(RD_PKT_SESSION_END, NULL, 0);
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
// Input processing
// ============================================================================

static void rdProcessInput() {
    if (!M5Cardputer.Keyboard.isChange()) return;
    if (!M5Cardputer.Keyboard.isPressed()) return;

    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

    // Handle special keys
    if (status.fn) {
        // FN combinations for mouse control
        // FN + arrows = mouse move
        // FN + Enter = left click
        // FN + Space = right click

        if (M5Cardputer.Keyboard.isKeyPressed(';')) {  // Up
            uint8_t data[4] = {0, 0, 0, (uint8_t)-10};  // dy = -10
            rdSendPacket(RD_PKT_MOUSE_MOVE, data, 4);
        }
        if (M5Cardputer.Keyboard.isKeyPressed('.')) {  // Down
            uint8_t data[4] = {0, 0, 0, 10};  // dy = 10
            rdSendPacket(RD_PKT_MOUSE_MOVE, data, 4);
        }
        if (M5Cardputer.Keyboard.isKeyPressed(',')) {  // Left (assuming)
            uint8_t data[4] = {0, (uint8_t)-10, 0, 0};  // dx = -10
            rdSendPacket(RD_PKT_MOUSE_MOVE, data, 4);
        }
        if (M5Cardputer.Keyboard.isKeyPressed('/')) {  // Right (assuming)
            uint8_t data[4] = {0, 10, 0, 0};  // dx = 10
            rdSendPacket(RD_PKT_MOUSE_MOVE, data, 4);
        }
        if (status.enter) {
            uint8_t data[1] = {0};  // Left click
            rdSendPacket(RD_PKT_MOUSE_CLICK, data, 1);
        }
        return;
    }

    // Regular key press - send as HID keycode
    for (auto ch : status.word) {
        uint8_t keycode = 0;
        uint8_t modifier = 0;

        // Simple ASCII to HID conversion
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
            rdSendPacket(RD_PKT_KEY_PRESS, data, 2);
            delay(10);
            rdSendPacket(RD_PKT_KEY_RELEASE, data, 2);
        }
    }

    // Special keys
    if (status.del) {
        uint8_t data[2] = {0x2A, 0};  // Backspace
        rdSendPacket(RD_PKT_KEY_PRESS, data, 2);
        delay(10);
        rdSendPacket(RD_PKT_KEY_RELEASE, data, 2);
    }

    if (status.enter) {
        uint8_t data[2] = {0x28, 0};  // Enter
        rdSendPacket(RD_PKT_KEY_PRESS, data, 2);
        delay(10);
        rdSendPacket(RD_PKT_KEY_RELEASE, data, 2);
    }
}

// ============================================================================
// Main session loop
// ============================================================================

static void rdLoop() {
    static uint8_t rxBuffer[32768];  // 32KB for receiving frames
    static uint32_t lastHeartbeat = 0;

    while (rdSession.state == RD_STATE_CONNECTED) {
        M5Cardputer.update();
        M5.update();

        // Check for exit
        if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE) &&
            M5Cardputer.Keyboard.isKeyPressed(KEY_FN)) {
            Serial.println("[RD] User requested disconnect");
            break;
        }

        // Process keyboard input
        rdProcessInput();

        // Check for incoming data
        if (rdSession.client.available() >= RD_PACKET_HEADER_SIZE) {
            RDPacketType type;
            uint16_t len;

            RDError err = rdReceivePacket(&type, rxBuffer, &len, 100);
            if (err != RD_OK) {
                if (err != RD_ERR_TIMEOUT) {
                    Serial.printf("[RD] Receive error: %s\n", rdErrorToString(err));
                    rdSession.state = RD_STATE_ERROR;
                    break;
                }
                continue;
            }

            switch (type) {
                case RD_PKT_SCREEN_FRAME:
                    // Decrypt and display frame
                    if (rdSession.state == RD_STATE_CONNECTED) {
                        uint8_t decrypted[32768];
                        size_t decLen;
                        err = rdDecrypt(rxBuffer, len, decrypted, &decLen);
                        if (err == RD_OK) {
                            rdDrawFrame(decrypted, decLen);
                        }
                    }
                    break;

                case RD_PKT_HEARTBEAT:
                    rdSendPacket(RD_PKT_HEARTBEAT_ACK, NULL, 0);
                    break;

                case RD_PKT_SESSION_END:
                    Serial.println("[RD] Server ended session");
                    rdSession.state = RD_STATE_DISCONNECTED;
                    break;

                default:
                    Serial.printf("[RD] Unknown packet type: %02X\n", type);
                    break;
            }
        }

        // Send heartbeat
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
            M5.Display.fillScreen(TFT_BLACK);
            M5.Display.setCursor(10, 10);
            M5.Display.println("Enter server IP:");
            String host = getUserInput(false);
            if (host.length() > 0) {
                strlcpy(rdConfig.serverHost, host.c_str(), sizeof(rdConfig.serverHost));
                rdSaveConfig();
            }
            rdShowSettings();
            continue;
        }

        // Set quality
        if (M5Cardputer.Keyboard.isKeyPressed('q') || M5Cardputer.Keyboard.isKeyPressed('Q')) {
            M5.Display.fillScreen(TFT_BLACK);
            M5.Display.setCursor(10, 10);
            M5.Display.println("Enter quality (1-100):");
            String q = getUserInput(false);
            int qval = q.toInt();
            if (qval >= 1 && qval <= 100) {
                rdConfig.jpegQuality = qval;
                rdSaveConfig();
            }
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

            // Return to settings
            rdShowSettings();
        }

        delay(50);
    }

    // Return to main menu
    inMenu = true;
}

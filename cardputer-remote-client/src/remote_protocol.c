/**
 * @file remote_protocol.c
 * @brief Protocol handling implementation
 */

#include "remote_protocol.h"
#include <string.h>
#include <esp_log.h>

static const char *TAG = "remote_protocol";

// Supported protocol versions
#define MIN_PROTOCOL_VERSION 0x01
#define MAX_PROTOCOL_VERSION 0x01

// =============================================================================
// Internal Helpers
// =============================================================================

static inline uint16_t read_u16_be(const uint8_t *buf) {
    return ((uint16_t)buf[0] << 8) | buf[1];
}

static inline void write_u16_be(uint8_t *buf, uint16_t val) {
    buf[0] = (val >> 8) & 0xFF;
    buf[1] = val & 0xFF;
}

static inline uint32_t read_u32_be(const uint8_t *buf) {
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) |
           (uint32_t)buf[3];
}

static inline void write_u32_be(uint8_t *buf, uint32_t val) {
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
}

/**
 * @brief Get total packet size from header
 */
static size_t get_packet_total_size(const uint8_t *header, bool encrypted) {
    uint16_t payload_len = read_u16_be(&header[2]);
    size_t total = PACKET_HEADER_SIZE + payload_len;

    if (encrypted) {
        total += AES_GCM_NONCE_SIZE + AES_GCM_TAG_SIZE;
    }

    return total;
}

// =============================================================================
// Initialization
// =============================================================================

remote_error_t protocol_init(protocol_context_t *ctx,
                             crypto_context_t *crypto,
                             uint8_t *rx_buffer, size_t rx_buffer_size,
                             uint8_t *tx_buffer, size_t tx_buffer_size) {
    if (!ctx || !crypto || !rx_buffer || !tx_buffer) {
        return REMOTE_ERR_INVALID_PARAM;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->crypto = crypto;
    ctx->protocol_version = PROTOCOL_VERSION;
    ctx->rx_buffer = rx_buffer;
    ctx->rx_buffer_size = rx_buffer_size;
    ctx->tx_buffer = tx_buffer;
    ctx->tx_buffer_size = tx_buffer_size;

    return REMOTE_OK;
}

void protocol_reset(protocol_context_t *ctx) {
    ctx->rx_buffer_len = 0;
    ctx->rx_sequence = 0;
    ctx->tx_sequence = 0;
}

// =============================================================================
// Packet Building
// =============================================================================

const uint8_t *protocol_build_unencrypted(protocol_context_t *ctx,
                                          packet_type_t type,
                                          const uint8_t *payload,
                                          size_t payload_len,
                                          size_t *out_len) {
    size_t total = PACKET_HEADER_SIZE + payload_len + AES_GCM_TAG_SIZE;

    if (total > ctx->tx_buffer_size) {
        ESP_LOGE(TAG, "TX buffer too small: need %zu, have %zu",
                 total, ctx->tx_buffer_size);
        return NULL;
    }

    // Build header
    ctx->tx_buffer[0] = ctx->protocol_version;
    ctx->tx_buffer[1] = (uint8_t)type;
    write_u16_be(&ctx->tx_buffer[2], (uint16_t)payload_len);

    // Copy payload
    if (payload_len > 0 && payload != NULL) {
        memcpy(&ctx->tx_buffer[PACKET_HEADER_SIZE], payload, payload_len);
    }

    // Zero tag for unencrypted packets
    memset(&ctx->tx_buffer[PACKET_HEADER_SIZE + payload_len], 0, AES_GCM_TAG_SIZE);

    *out_len = total;
    ctx->tx_sequence++;
    return ctx->tx_buffer;
}

const uint8_t *protocol_build_encrypted(protocol_context_t *ctx,
                                        packet_type_t type,
                                        const uint8_t *payload,
                                        size_t payload_len,
                                        size_t *out_len) {
    if (!crypto_is_session_established(ctx->crypto)) {
        ESP_LOGE(TAG, "Session not established for encrypted packet");
        return NULL;
    }

    // Total: header(4) + nonce(12) + ciphertext(payload_len) + tag(16)
    size_t total = PACKET_HEADER_SIZE + AES_GCM_NONCE_SIZE + payload_len + AES_GCM_TAG_SIZE;

    if (total > ctx->tx_buffer_size) {
        ESP_LOGE(TAG, "TX buffer too small: need %zu, have %zu",
                 total, ctx->tx_buffer_size);
        return NULL;
    }

    // Build header (length = nonce + ciphertext, tag is separate)
    ctx->tx_buffer[0] = ctx->protocol_version;
    ctx->tx_buffer[1] = (uint8_t)type;
    write_u16_be(&ctx->tx_buffer[2], (uint16_t)(AES_GCM_NONCE_SIZE + payload_len));

    // Encrypt payload
    uint8_t nonce[AES_GCM_NONCE_SIZE];
    uint8_t tag[AES_GCM_TAG_SIZE];

    remote_error_t err = crypto_encrypt(ctx->crypto,
                                        payload, payload_len,
                                        &ctx->tx_buffer[PACKET_HEADER_SIZE + AES_GCM_NONCE_SIZE],
                                        nonce, tag);
    if (err != REMOTE_OK) {
        ESP_LOGE(TAG, "Encryption failed");
        return NULL;
    }

    // Place nonce before ciphertext
    memcpy(&ctx->tx_buffer[PACKET_HEADER_SIZE], nonce, AES_GCM_NONCE_SIZE);

    // Place tag at end
    memcpy(&ctx->tx_buffer[PACKET_HEADER_SIZE + AES_GCM_NONCE_SIZE + payload_len],
           tag, AES_GCM_TAG_SIZE);

    *out_len = total;
    ctx->tx_sequence++;
    return ctx->tx_buffer;
}

// =============================================================================
// Packet Parsing
// =============================================================================

size_t protocol_feed_data(protocol_context_t *ctx,
                          const uint8_t *data, size_t len) {
    size_t available = ctx->rx_buffer_size - ctx->rx_buffer_len;
    size_t to_copy = len < available ? len : available;

    if (to_copy > 0) {
        memcpy(&ctx->rx_buffer[ctx->rx_buffer_len], data, to_copy);
        ctx->rx_buffer_len += to_copy;
    }

    return to_copy;
}

bool protocol_packet_ready(const protocol_context_t *ctx) {
    if (ctx->rx_buffer_len < PACKET_HEADER_SIZE) {
        return false;
    }

    // Determine if this is encrypted based on packet type
    packet_type_t type = (packet_type_t)ctx->rx_buffer[1];
    bool encrypted = (type >= PKT_SESSION_START);  // Session packets are encrypted

    size_t needed = get_packet_total_size(ctx->rx_buffer, encrypted);
    return ctx->rx_buffer_len >= needed;
}

remote_error_t protocol_parse_packet(protocol_context_t *ctx,
                                     parsed_packet_t *packet,
                                     bool encrypted) {
    if (ctx->rx_buffer_len < PACKET_HEADER_SIZE) {
        return REMOTE_ERR_PROTOCOL;
    }

    // Parse header
    packet->version = ctx->rx_buffer[0];
    packet->type = (packet_type_t)ctx->rx_buffer[1];
    uint16_t wire_len = read_u16_be(&ctx->rx_buffer[2]);

    // Version check
    if (!protocol_version_supported(packet->version)) {
        ESP_LOGE(TAG, "Unsupported protocol version: %d", packet->version);
        return REMOTE_ERR_PROTOCOL;
    }

    size_t total_size = get_packet_total_size(ctx->rx_buffer, encrypted);
    if (ctx->rx_buffer_len < total_size) {
        return REMOTE_ERR_PROTOCOL;  // Incomplete packet
    }

    packet->encrypted = encrypted;

    if (encrypted) {
        // Wire format for encrypted: header(4) + nonce(12) + ciphertext + tag(16)
        // wire_len = nonce + ciphertext
        if (wire_len < AES_GCM_NONCE_SIZE) {
            return REMOTE_ERR_PROTOCOL;
        }

        size_t ciphertext_len = wire_len - AES_GCM_NONCE_SIZE;
        const uint8_t *nonce = &ctx->rx_buffer[PACKET_HEADER_SIZE];
        const uint8_t *ciphertext = &ctx->rx_buffer[PACKET_HEADER_SIZE + AES_GCM_NONCE_SIZE];
        const uint8_t *tag = &ctx->rx_buffer[PACKET_HEADER_SIZE + wire_len];

        // Decrypt in place (overwrite nonce area with plaintext)
        uint8_t *plaintext = &ctx->rx_buffer[PACKET_HEADER_SIZE];
        remote_error_t err = crypto_decrypt(ctx->crypto,
                                            ciphertext, ciphertext_len,
                                            nonce, tag,
                                            plaintext);
        if (err != REMOTE_OK) {
            return err;
        }

        packet->payload = plaintext;
        packet->payload_len = ciphertext_len;
    } else {
        // Unencrypted packet
        packet->payload = &ctx->rx_buffer[PACKET_HEADER_SIZE];
        packet->payload_len = wire_len;
    }

    ctx->rx_sequence++;
    return REMOTE_OK;
}

void protocol_consume_packet(protocol_context_t *ctx) {
    if (ctx->rx_buffer_len < PACKET_HEADER_SIZE) {
        ctx->rx_buffer_len = 0;
        return;
    }

    packet_type_t type = (packet_type_t)ctx->rx_buffer[1];
    bool encrypted = (type >= PKT_SESSION_START);
    size_t total_size = get_packet_total_size(ctx->rx_buffer, encrypted);

    if (total_size >= ctx->rx_buffer_len) {
        ctx->rx_buffer_len = 0;
    } else {
        size_t remaining = ctx->rx_buffer_len - total_size;
        memmove(ctx->rx_buffer, &ctx->rx_buffer[total_size], remaining);
        ctx->rx_buffer_len = remaining;
    }
}

// =============================================================================
// Handshake Messages
// =============================================================================

// Store handshake state
static struct {
    uint8_t our_ephemeral_public[ECDH_PUBLIC_KEY_SIZE];
    uint8_t our_nonce[HANDSHAKE_NONCE_SIZE];
    uint8_t peer_ephemeral_public[ECDH_PUBLIC_KEY_SIZE];
    uint8_t peer_nonce[HANDSHAKE_NONCE_SIZE];
} handshake_state;

const uint8_t *protocol_build_handshake_init(protocol_context_t *ctx,
                                             size_t *out_len) {
    handshake_init_t init;

    // Generate ephemeral keypair
    remote_error_t err = crypto_generate_ephemeral_keypair(
        ctx->crypto, handshake_state.our_ephemeral_public);
    if (err != REMOTE_OK) {
        ESP_LOGE(TAG, "Failed to generate ephemeral keypair");
        return NULL;
    }
    memcpy(init.ephemeral_public_key, handshake_state.our_ephemeral_public,
           ECDH_PUBLIC_KEY_SIZE);

    // Generate nonce
    err = crypto_generate_nonce(ctx->crypto, handshake_state.our_nonce);
    if (err != REMOTE_OK) {
        ESP_LOGE(TAG, "Failed to generate nonce");
        return NULL;
    }
    memcpy(init.nonce, handshake_state.our_nonce, HANDSHAKE_NONCE_SIZE);

    // SECURITY: Sign ephemeral_public_key || nonce
    uint8_t sign_data[ECDH_PUBLIC_KEY_SIZE + HANDSHAKE_NONCE_SIZE];
    memcpy(sign_data, init.ephemeral_public_key, ECDH_PUBLIC_KEY_SIZE);
    memcpy(sign_data + ECDH_PUBLIC_KEY_SIZE, init.nonce, HANDSHAKE_NONCE_SIZE);

    err = crypto_sign(ctx->crypto, sign_data, sizeof(sign_data), init.signature);
    if (err != REMOTE_OK) {
        ESP_LOGE(TAG, "Failed to sign handshake init");
        return NULL;
    }

    return protocol_build_unencrypted(ctx, PKT_HANDSHAKE_INIT,
                                      (const uint8_t *)&init, sizeof(init),
                                      out_len);
}

remote_error_t protocol_handle_handshake_response(protocol_context_t *ctx,
                                                   const parsed_packet_t *packet) {
    if (packet->payload_len < sizeof(handshake_response_t)) {
        ESP_LOGE(TAG, "HandshakeResponse too short");
        return REMOTE_ERR_PROTOCOL;
    }

    const handshake_response_t *resp = (const handshake_response_t *)packet->payload;

    // Store peer's ephemeral public key and nonce
    memcpy(handshake_state.peer_ephemeral_public, resp->ephemeral_public_key,
           ECDH_PUBLIC_KEY_SIZE);
    memcpy(handshake_state.peer_nonce, resp->nonce, HANDSHAKE_NONCE_SIZE);

    // SECURITY: Verify signature covers ephemeral_pubkey || our_nonce || peer_nonce
    uint8_t sign_data[ECDH_PUBLIC_KEY_SIZE + HANDSHAKE_NONCE_SIZE * 2];
    memcpy(sign_data, resp->ephemeral_public_key, ECDH_PUBLIC_KEY_SIZE);
    memcpy(sign_data + ECDH_PUBLIC_KEY_SIZE, handshake_state.our_nonce,
           HANDSHAKE_NONCE_SIZE);
    memcpy(sign_data + ECDH_PUBLIC_KEY_SIZE + HANDSHAKE_NONCE_SIZE,
           resp->nonce, HANDSHAKE_NONCE_SIZE);

    remote_error_t err = crypto_verify_peer_signature(ctx->crypto,
                                                       sign_data, sizeof(sign_data),
                                                       resp->signature);
    if (err != REMOTE_OK) {
        ESP_LOGE(TAG, "Server signature verification failed");
        return REMOTE_ERR_SIGNATURE;
    }

    ESP_LOGI(TAG, "Server signature verified");

    // Derive session keys (we are client, not server)
    err = crypto_derive_session_keys(ctx->crypto,
                                     handshake_state.peer_ephemeral_public,
                                     handshake_state.our_nonce,
                                     handshake_state.peer_nonce,
                                     false);  // is_server = false
    if (err != REMOTE_OK) {
        ESP_LOGE(TAG, "Failed to derive session keys");
        return err;
    }

    ESP_LOGI(TAG, "Session keys derived");
    return REMOTE_OK;
}

const uint8_t *protocol_build_handshake_complete(protocol_context_t *ctx,
                                                  size_t *out_len) {
    handshake_complete_t complete;

    // Build transcript: our_ephemeral || our_nonce || peer_ephemeral || peer_nonce
    uint8_t transcript[(ECDH_PUBLIC_KEY_SIZE + HANDSHAKE_NONCE_SIZE) * 2];
    size_t offset = 0;

    memcpy(transcript + offset, handshake_state.our_ephemeral_public,
           ECDH_PUBLIC_KEY_SIZE);
    offset += ECDH_PUBLIC_KEY_SIZE;

    memcpy(transcript + offset, handshake_state.our_nonce, HANDSHAKE_NONCE_SIZE);
    offset += HANDSHAKE_NONCE_SIZE;

    memcpy(transcript + offset, handshake_state.peer_ephemeral_public,
           ECDH_PUBLIC_KEY_SIZE);
    offset += ECDH_PUBLIC_KEY_SIZE;

    memcpy(transcript + offset, handshake_state.peer_nonce, HANDSHAKE_NONCE_SIZE);

    // Compute transcript MAC
    remote_error_t err = crypto_compute_transcript_mac(ctx->crypto,
                                                        transcript, sizeof(transcript),
                                                        complete.transcript_mac);
    if (err != REMOTE_OK) {
        ESP_LOGE(TAG, "Failed to compute transcript MAC");
        return NULL;
    }

    // Clear handshake state
    memset(&handshake_state, 0, sizeof(handshake_state));

    return protocol_build_unencrypted(ctx, PKT_HANDSHAKE_COMPLETE,
                                      (const uint8_t *)&complete, sizeof(complete),
                                      out_len);
}

// =============================================================================
// Session Messages
// =============================================================================

const uint8_t *protocol_build_mouse_move(protocol_context_t *ctx,
                                         int8_t dx, int8_t dy,
                                         size_t *out_len) {
    mouse_move_t move = { .dx = dx, .dy = dy };
    return protocol_build_encrypted(ctx, PKT_MOUSE_MOVE,
                                    (const uint8_t *)&move, sizeof(move),
                                    out_len);
}

const uint8_t *protocol_build_mouse_click(protocol_context_t *ctx,
                                          mouse_button_t button,
                                          bool pressed,
                                          size_t *out_len) {
    mouse_click_t click = {
        .button = (uint8_t)button,
        .pressed = pressed ? 1 : 0
    };
    return protocol_build_encrypted(ctx, PKT_MOUSE_CLICK,
                                    (const uint8_t *)&click, sizeof(click),
                                    out_len);
}

const uint8_t *protocol_build_key_press(protocol_context_t *ctx,
                                        uint8_t keycode,
                                        uint8_t modifiers,
                                        size_t *out_len) {
    key_event_t event = {
        .keycode = keycode,
        .modifiers = modifiers
    };
    return protocol_build_encrypted(ctx, PKT_KEY_PRESS,
                                    (const uint8_t *)&event, sizeof(event),
                                    out_len);
}

const uint8_t *protocol_build_key_release(protocol_context_t *ctx,
                                          uint8_t keycode,
                                          uint8_t modifiers,
                                          size_t *out_len) {
    key_event_t event = {
        .keycode = keycode,
        .modifiers = modifiers
    };
    return protocol_build_encrypted(ctx, PKT_KEY_RELEASE,
                                    (const uint8_t *)&event, sizeof(event),
                                    out_len);
}

const uint8_t *protocol_build_heartbeat(protocol_context_t *ctx,
                                        size_t *out_len) {
    return protocol_build_encrypted(ctx, PKT_HEARTBEAT, NULL, 0, out_len);
}

const uint8_t *protocol_build_heartbeat_ack(protocol_context_t *ctx,
                                            size_t *out_len) {
    return protocol_build_encrypted(ctx, PKT_HEARTBEAT_ACK, NULL, 0, out_len);
}

const uint8_t *protocol_build_session_end(protocol_context_t *ctx,
                                          size_t *out_len) {
    return protocol_build_encrypted(ctx, PKT_SESSION_END, NULL, 0, out_len);
}

const uint8_t *protocol_build_mode_switch(protocol_context_t *ctx,
                                          input_mode_t mode,
                                          size_t *out_len) {
    uint8_t mode_byte = (uint8_t)mode;
    return protocol_build_encrypted(ctx, PKT_MODE_SWITCH,
                                    &mode_byte, 1, out_len);
}

// =============================================================================
// Screen Frame Parsing
// =============================================================================

remote_error_t protocol_parse_screen_frame(const parsed_packet_t *packet,
                                           screen_frame_t *frame) {
    if (packet->type != PKT_SCREEN_FRAME) {
        return REMOTE_ERR_PROTOCOL;
    }

    if (packet->payload_len < sizeof(screen_frame_header_t)) {
        return REMOTE_ERR_PROTOCOL;
    }

    const screen_frame_header_t *header =
        (const screen_frame_header_t *)packet->payload;

    frame->sequence = read_u32_be((const uint8_t *)&header->sequence);
    frame->timestamp = read_u32_be((const uint8_t *)&header->timestamp);
    frame->jpeg_data = packet->payload + sizeof(screen_frame_header_t);
    frame->jpeg_len = packet->payload_len - sizeof(screen_frame_header_t);

    return REMOTE_OK;
}

// =============================================================================
// Version Support
// =============================================================================

bool protocol_version_supported(uint8_t version) {
    return version >= MIN_PROTOCOL_VERSION && version <= MAX_PROTOCOL_VERSION;
}

uint8_t protocol_get_version(const protocol_context_t *ctx) {
    return ctx->protocol_version;
}

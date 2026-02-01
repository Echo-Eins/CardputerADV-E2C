/**
 * @file remote_protocol.h
 * @brief Protocol handling for Cardputer Remote
 *
 * Handles packet serialization/deserialization with versioning support
 */

#ifndef REMOTE_PROTOCOL_H
#define REMOTE_PROTOCOL_H

#include "remote_config.h"
#include "remote_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Packet Builder/Parser
// =============================================================================

/**
 * @brief Protocol context
 */
typedef struct {
    crypto_context_t *crypto;
    uint8_t protocol_version;
    uint32_t rx_sequence;
    uint32_t tx_sequence;

    // Receive buffer
    uint8_t *rx_buffer;
    size_t rx_buffer_size;
    size_t rx_buffer_len;

    // Transmit buffer
    uint8_t *tx_buffer;
    size_t tx_buffer_size;
} protocol_context_t;

/**
 * @brief Initialize protocol context
 * @param ctx Protocol context
 * @param crypto Initialized crypto context
 * @param rx_buffer Receive buffer
 * @param rx_buffer_size Receive buffer size
 * @param tx_buffer Transmit buffer
 * @param tx_buffer_size Transmit buffer size
 * @return REMOTE_OK on success
 */
remote_error_t protocol_init(protocol_context_t *ctx,
                             crypto_context_t *crypto,
                             uint8_t *rx_buffer, size_t rx_buffer_size,
                             uint8_t *tx_buffer, size_t tx_buffer_size);

/**
 * @brief Reset protocol state
 */
void protocol_reset(protocol_context_t *ctx);

// =============================================================================
// Packet Building (for transmission)
// =============================================================================

/**
 * @brief Build unencrypted packet (for handshake)
 * @param ctx Protocol context
 * @param type Packet type
 * @param payload Payload data
 * @param payload_len Payload length
 * @param[out] out_len Total packet length
 * @return Pointer to tx_buffer with built packet, or NULL on error
 */
const uint8_t *protocol_build_unencrypted(protocol_context_t *ctx,
                                          packet_type_t type,
                                          const uint8_t *payload,
                                          size_t payload_len,
                                          size_t *out_len);

/**
 * @brief Build encrypted packet
 * @param ctx Protocol context (session must be established)
 * @param type Packet type
 * @param payload Payload data
 * @param payload_len Payload length
 * @param[out] out_len Total packet length
 * @return Pointer to tx_buffer with built packet, or NULL on error
 */
const uint8_t *protocol_build_encrypted(protocol_context_t *ctx,
                                        packet_type_t type,
                                        const uint8_t *payload,
                                        size_t payload_len,
                                        size_t *out_len);

// =============================================================================
// Packet Parsing (for reception)
// =============================================================================

/**
 * @brief Parsed packet structure
 */
typedef struct {
    uint8_t version;
    packet_type_t type;
    uint16_t payload_len;
    const uint8_t *payload;     // Points into rx_buffer (decrypted in place)
    bool encrypted;
} parsed_packet_t;

/**
 * @brief Feed received data to parser
 * @param ctx Protocol context
 * @param data Received data
 * @param len Data length
 * @return Number of bytes consumed
 */
size_t protocol_feed_data(protocol_context_t *ctx,
                          const uint8_t *data, size_t len);

/**
 * @brief Check if a complete packet is available
 * @param ctx Protocol context
 * @return true if complete packet ready
 */
bool protocol_packet_ready(const protocol_context_t *ctx);

/**
 * @brief Parse next complete packet
 * @param ctx Protocol context
 * @param[out] packet Parsed packet info
 * @param encrypted true if packet should be decrypted
 * @return REMOTE_OK on success
 */
remote_error_t protocol_parse_packet(protocol_context_t *ctx,
                                     parsed_packet_t *packet,
                                     bool encrypted);

/**
 * @brief Discard current packet from buffer
 */
void protocol_consume_packet(protocol_context_t *ctx);

// =============================================================================
// Handshake Message Builders
// =============================================================================

/**
 * @brief Build HandshakeInit packet
 * @param ctx Protocol context
 * @param[out] out_len Packet length
 * @return Packet data or NULL
 */
const uint8_t *protocol_build_handshake_init(protocol_context_t *ctx,
                                             size_t *out_len);

/**
 * @brief Parse HandshakeResponse and derive session keys
 * @param ctx Protocol context
 * @param packet Parsed HandshakeResponse packet
 * @return REMOTE_OK on success
 */
remote_error_t protocol_handle_handshake_response(protocol_context_t *ctx,
                                                   const parsed_packet_t *packet);

/**
 * @brief Build HandshakeComplete packet
 * @param ctx Protocol context
 * @param[out] out_len Packet length
 * @return Packet data or NULL
 */
const uint8_t *protocol_build_handshake_complete(protocol_context_t *ctx,
                                                  size_t *out_len);

// =============================================================================
// Session Message Builders
// =============================================================================

/**
 * @brief Build mouse move packet
 */
const uint8_t *protocol_build_mouse_move(protocol_context_t *ctx,
                                         int8_t dx, int8_t dy,
                                         size_t *out_len);

/**
 * @brief Build mouse click packet
 */
const uint8_t *protocol_build_mouse_click(protocol_context_t *ctx,
                                          mouse_button_t button,
                                          bool pressed,
                                          size_t *out_len);

/**
 * @brief Build key press packet
 */
const uint8_t *protocol_build_key_press(protocol_context_t *ctx,
                                        uint8_t keycode,
                                        uint8_t modifiers,
                                        size_t *out_len);

/**
 * @brief Build key release packet
 */
const uint8_t *protocol_build_key_release(protocol_context_t *ctx,
                                          uint8_t keycode,
                                          uint8_t modifiers,
                                          size_t *out_len);

/**
 * @brief Build heartbeat packet
 */
const uint8_t *protocol_build_heartbeat(protocol_context_t *ctx,
                                        size_t *out_len);

/**
 * @brief Build heartbeat ack packet
 */
const uint8_t *protocol_build_heartbeat_ack(protocol_context_t *ctx,
                                            size_t *out_len);

/**
 * @brief Build session end packet
 */
const uint8_t *protocol_build_session_end(protocol_context_t *ctx,
                                          size_t *out_len);

/**
 * @brief Build mode switch packet
 */
const uint8_t *protocol_build_mode_switch(protocol_context_t *ctx,
                                          input_mode_t mode,
                                          size_t *out_len);

// =============================================================================
// Screen Frame Parsing
// =============================================================================

/**
 * @brief Parsed screen frame
 */
typedef struct {
    uint32_t sequence;
    uint32_t timestamp;
    const uint8_t *jpeg_data;
    size_t jpeg_len;
} screen_frame_t;

/**
 * @brief Parse screen frame from packet
 * @param packet Parsed packet (must be PKT_SCREEN_FRAME)
 * @param[out] frame Screen frame info
 * @return REMOTE_OK on success
 */
remote_error_t protocol_parse_screen_frame(const parsed_packet_t *packet,
                                           screen_frame_t *frame);

// =============================================================================
// Version Negotiation
// =============================================================================

/**
 * @brief Check if protocol version is supported
 */
bool protocol_version_supported(uint8_t version);

/**
 * @brief Get current protocol version
 */
uint8_t protocol_get_version(const protocol_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif // REMOTE_PROTOCOL_H

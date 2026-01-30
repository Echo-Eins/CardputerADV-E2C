/**
 * @file remote_network.h
 * @brief Network module with WiFi, mDNS discovery, and TCP connection
 */

#ifndef REMOTE_NETWORK_H
#define REMOTE_NETWORK_H

#include "remote_config.h"
#include "remote_crypto.h"
#include "remote_protocol.h"
#include <esp_wifi.h>
#include <mdns.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Discovered Server
// =============================================================================

typedef struct {
    char hostname[64];
    char ip_addr[16];
    uint16_t port;
    bool valid;
} discovered_server_t;

// =============================================================================
// Network Context
// =============================================================================

typedef struct {
    // WiFi state
    bool wifi_connected;
    uint8_t wifi_retry_count;
    esp_netif_t *netif;

    // mDNS state
    bool mdns_running;

    // TCP connection
    int sock;
    bool connected;
    connection_state_t state;

    // Configuration
    remote_config_t config;

    // Protocol & crypto
    crypto_context_t crypto;
    protocol_context_t protocol;

    // Buffers
    uint8_t *rx_buffer;
    uint8_t *tx_buffer;

    // Discovered servers
    discovered_server_t discovered_servers[8];
    int discovered_count;

    // Statistics
    uint32_t bytes_sent;
    uint32_t bytes_received;
    uint32_t packets_sent;
    uint32_t packets_received;
    int64_t last_heartbeat_time;
    int64_t last_receive_time;

    // Event callbacks
    void (*on_connected)(void *user_data);
    void (*on_disconnected)(void *user_data);
    void (*on_frame_received)(const uint8_t *jpeg_data, size_t len, void *user_data);
    void (*on_error)(remote_error_t error, void *user_data);
    void *user_data;
} network_context_t;

// =============================================================================
// Initialization
// =============================================================================

/**
 * @brief Initialize network module
 * @param ctx Network context
 * @param config Configuration
 * @return REMOTE_OK on success
 */
remote_error_t network_init(network_context_t *ctx, const remote_config_t *config);

/**
 * @brief Deinitialize network module
 */
void network_deinit(network_context_t *ctx);

// =============================================================================
// WiFi Connection
// =============================================================================

/**
 * @brief Connect to WiFi
 * @param ctx Network context
 * @return REMOTE_OK on success
 */
remote_error_t network_wifi_connect(network_context_t *ctx);

/**
 * @brief Disconnect from WiFi
 */
void network_wifi_disconnect(network_context_t *ctx);

/**
 * @brief Check if WiFi is connected
 */
bool network_wifi_is_connected(const network_context_t *ctx);

// =============================================================================
// Server Discovery
// =============================================================================

/**
 * @brief Start mDNS discovery for servers
 * @param ctx Network context
 * @param timeout_ms Discovery timeout
 * @return Number of servers found
 */
int network_discover_servers(network_context_t *ctx, uint32_t timeout_ms);

/**
 * @brief Get discovered server info
 * @param ctx Network context
 * @param index Server index (0-based)
 * @param[out] server Server info
 * @return REMOTE_OK if found
 */
remote_error_t network_get_discovered_server(const network_context_t *ctx,
                                             int index,
                                             discovered_server_t *server);

// =============================================================================
// Server Connection
// =============================================================================

/**
 * @brief Connect to a server
 * @param ctx Network context
 * @param host Server hostname or IP
 * @param port Server port
 * @return REMOTE_OK on success
 */
remote_error_t network_connect(network_context_t *ctx,
                               const char *host, uint16_t port);

/**
 * @brief Connect to discovered server by index
 * @param ctx Network context
 * @param server_index Index from discovery
 * @return REMOTE_OK on success
 */
remote_error_t network_connect_discovered(network_context_t *ctx, int server_index);

/**
 * @brief Disconnect from server
 */
void network_disconnect(network_context_t *ctx);

/**
 * @brief Check if connected to server
 */
bool network_is_connected(const network_context_t *ctx);

/**
 * @brief Get current connection state
 */
connection_state_t network_get_state(const network_context_t *ctx);

// =============================================================================
// Handshake
// =============================================================================

/**
 * @brief Perform cryptographic handshake with server
 * @param ctx Network context
 * @return REMOTE_OK on success
 */
remote_error_t network_handshake(network_context_t *ctx);

// =============================================================================
// Data Transfer
// =============================================================================

/**
 * @brief Send raw data
 * @param ctx Network context
 * @param data Data to send
 * @param len Data length
 * @return REMOTE_OK on success
 */
remote_error_t network_send(network_context_t *ctx,
                            const uint8_t *data, size_t len);

/**
 * @brief Receive data (non-blocking)
 * @param ctx Network context
 * @param timeout_ms Timeout (0 for non-blocking)
 * @return Number of bytes received, 0 if none, -1 on error
 */
int network_receive(network_context_t *ctx, uint32_t timeout_ms);

/**
 * @brief Process received data and dispatch events
 * @param ctx Network context
 * @return REMOTE_OK on success
 */
remote_error_t network_process(network_context_t *ctx);

// =============================================================================
// Input Sending
// =============================================================================

/**
 * @brief Send mouse move event
 */
remote_error_t network_send_mouse_move(network_context_t *ctx, int8_t dx, int8_t dy);

/**
 * @brief Send mouse click event
 */
remote_error_t network_send_mouse_click(network_context_t *ctx,
                                        mouse_button_t button, bool pressed);

/**
 * @brief Send key press event
 */
remote_error_t network_send_key_press(network_context_t *ctx,
                                      uint8_t keycode, uint8_t modifiers);

/**
 * @brief Send key release event
 */
remote_error_t network_send_key_release(network_context_t *ctx,
                                        uint8_t keycode, uint8_t modifiers);

/**
 * @brief Send mode switch event
 */
remote_error_t network_send_mode_switch(network_context_t *ctx, input_mode_t mode);

/**
 * @brief Send heartbeat
 */
remote_error_t network_send_heartbeat(network_context_t *ctx);

/**
 * @brief Send session end
 */
remote_error_t network_send_session_end(network_context_t *ctx);

// =============================================================================
// Event Callbacks
// =============================================================================

/**
 * @brief Set callback for connection established
 */
void network_set_on_connected(network_context_t *ctx,
                              void (*callback)(void *), void *user_data);

/**
 * @brief Set callback for disconnection
 */
void network_set_on_disconnected(network_context_t *ctx,
                                 void (*callback)(void *), void *user_data);

/**
 * @brief Set callback for frame received
 */
void network_set_on_frame(network_context_t *ctx,
                          void (*callback)(const uint8_t *, size_t, void *),
                          void *user_data);

/**
 * @brief Set callback for errors
 */
void network_set_on_error(network_context_t *ctx,
                          void (*callback)(remote_error_t, void *),
                          void *user_data);

// =============================================================================
// Statistics
// =============================================================================

typedef struct {
    uint32_t bytes_sent;
    uint32_t bytes_received;
    uint32_t packets_sent;
    uint32_t packets_received;
    int32_t latency_ms;
    bool connected;
} network_stats_t;

void network_get_stats(const network_context_t *ctx, network_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif // REMOTE_NETWORK_H

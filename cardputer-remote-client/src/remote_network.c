/**
 * @file remote_network.c
 * @brief Network module implementation
 */

#include "remote_network.h"
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <esp_log.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

static const char *TAG = "remote_network";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MAX_WIFI_RETRY      5

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_num = 0;

// =============================================================================
// WiFi Event Handlers
// =============================================================================

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    network_context_t *ctx = (network_context_t *)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ctx->wifi_connected = false;
        if (s_retry_num < MAX_WIFI_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry WiFi connection (%d/%d)", s_retry_num, MAX_WIFI_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ctx->wifi_connected = true;
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// =============================================================================
// Initialization
// =============================================================================

remote_error_t network_init(network_context_t *ctx, const remote_config_t *config) {
    esp_err_t err;

    memset(ctx, 0, sizeof(*ctx));
    memcpy(&ctx->config, config, sizeof(remote_config_t));
    ctx->sock = -1;
    ctx->state = CONN_STATE_DISCONNECTED;

    // Allocate buffers
    ctx->rx_buffer = heap_caps_malloc(TCP_RX_BUFFER_SIZE, MALLOC_CAP_INTERNAL);
    ctx->tx_buffer = heap_caps_malloc(TCP_TX_BUFFER_SIZE, MALLOC_CAP_INTERNAL);

    if (!ctx->rx_buffer || !ctx->tx_buffer) {
        ESP_LOGE(TAG, "Failed to allocate network buffers");
        network_deinit(ctx);
        return REMOTE_ERR_NO_MEMORY;
    }

    // Initialize crypto
    remote_error_t rerr = crypto_init(&ctx->crypto, config->private_key);
    if (rerr != REMOTE_OK) {
        ESP_LOGE(TAG, "Crypto init failed");
        network_deinit(ctx);
        return rerr;
    }

    // Set server's public key for authentication
    rerr = crypto_set_peer_public_key(&ctx->crypto,
                                       (const char *)config->server_public_key);
    if (rerr != REMOTE_OK) {
        ESP_LOGE(TAG, "Failed to set server public key");
        network_deinit(ctx);
        return rerr;
    }

    // Initialize protocol
    rerr = protocol_init(&ctx->protocol, &ctx->crypto,
                         ctx->rx_buffer, TCP_RX_BUFFER_SIZE,
                         ctx->tx_buffer, TCP_TX_BUFFER_SIZE);
    if (rerr != REMOTE_OK) {
        ESP_LOGE(TAG, "Protocol init failed");
        network_deinit(ctx);
        return rerr;
    }

    // Initialize WiFi
    s_wifi_event_group = xEventGroupCreate();

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "netif init failed: %s", esp_err_to_name(err));
        network_deinit(ctx);
        return REMOTE_ERR_NETWORK;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop create failed: %s", esp_err_to_name(err));
        network_deinit(ctx);
        return REMOTE_ERR_NETWORK;
    }

    ctx->netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        network_deinit(ctx);
        return REMOTE_ERR_NETWORK;
    }

    // Register event handlers
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, ctx, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, ctx, NULL);

    // Initialize mDNS
    err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        // Non-fatal - can still work without mDNS
    } else {
        ctx->mdns_running = true;
    }

    ESP_LOGI(TAG, "Network module initialized");
    return REMOTE_OK;
}

void network_deinit(network_context_t *ctx) {
    network_disconnect(ctx);
    network_wifi_disconnect(ctx);

    if (ctx->mdns_running) {
        mdns_free();
    }

    crypto_free(&ctx->crypto);

    if (ctx->rx_buffer) {
        heap_caps_free(ctx->rx_buffer);
    }
    if (ctx->tx_buffer) {
        heap_caps_free(ctx->tx_buffer);
    }

    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    memset(ctx, 0, sizeof(*ctx));
}

// =============================================================================
// WiFi Connection
// =============================================================================

remote_error_t network_wifi_connect(network_context_t *ctx) {
    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    strncpy((char *)wifi_cfg.sta.ssid, ctx->config.wifi_ssid,
            sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, ctx->config.wifi_password,
            sizeof(wifi_cfg.sta.password) - 1);

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi set mode failed: %s", esp_err_to_name(err));
        return REMOTE_ERR_NETWORK;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi set config failed: %s", esp_err_to_name(err));
        return REMOTE_ERR_NETWORK;
    }

    s_retry_num = 0;
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
        return REMOTE_ERR_NETWORK;
    }

    ESP_LOGI(TAG, "Connecting to WiFi: %s", ctx->config.wifi_ssid);

    // Wait for connection
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
        return REMOTE_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "WiFi connection failed");
        return REMOTE_ERR_NETWORK;
    }

    ESP_LOGE(TAG, "WiFi connection timeout");
    return REMOTE_ERR_TIMEOUT;
}

void network_wifi_disconnect(network_context_t *ctx) {
    esp_wifi_stop();
    ctx->wifi_connected = false;
}

bool network_wifi_is_connected(const network_context_t *ctx) {
    return ctx->wifi_connected;
}

// =============================================================================
// Server Discovery
// =============================================================================

int network_discover_servers(network_context_t *ctx, uint32_t timeout_ms) {
    if (!ctx->mdns_running) {
        ESP_LOGW(TAG, "mDNS not available");
        return 0;
    }

    ctx->discovered_count = 0;

    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(MDNS_SERVICE_TYPE, "_tcp", timeout_ms, 8, &results);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS query failed: %s", esp_err_to_name(err));
        return 0;
    }

    mdns_result_t *r = results;
    int count = 0;

    while (r && count < 8) {
        // Get service info
        mdns_result_t *a = NULL;
        err = mdns_query_srv(r->instance_name, MDNS_SERVICE_TYPE, "_tcp",
                            timeout_ms, &a);
        if (err == ESP_OK && a) {
            discovered_server_t *server = &ctx->discovered_servers[count];

            strncpy(server->hostname, r->instance_name, sizeof(server->hostname) - 1);
            server->port = a->port;

            // Get IP address
            if (a->addr && a->addr->addr.type == ESP_IPADDR_TYPE_V4) {
                snprintf(server->ip_addr, sizeof(server->ip_addr),
                        IPSTR, IP2STR(&a->addr->addr.u_addr.ip4));
                server->valid = true;
                count++;
                ESP_LOGI(TAG, "Found server: %s at %s:%d",
                        server->hostname, server->ip_addr, server->port);
            }

            mdns_query_results_free(a);
        }

        r = r->next;
    }

    mdns_query_results_free(results);
    ctx->discovered_count = count;

    return count;
}

remote_error_t network_get_discovered_server(const network_context_t *ctx,
                                             int index,
                                             discovered_server_t *server) {
    if (index < 0 || index >= ctx->discovered_count) {
        return REMOTE_ERR_INVALID_PARAM;
    }

    memcpy(server, &ctx->discovered_servers[index], sizeof(*server));
    return REMOTE_OK;
}

// =============================================================================
// TCP Connection
// =============================================================================

remote_error_t network_connect(network_context_t *ctx,
                               const char *host, uint16_t port) {
    if (ctx->connected) {
        network_disconnect(ctx);
    }

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;

    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", port);

    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed for %s", host);
        return REMOTE_ERR_NETWORK;
    }

    ctx->sock = socket(res->ai_family, res->ai_socktype, 0);
    if (ctx->sock < 0) {
        ESP_LOGE(TAG, "Socket create failed: %d", errno);
        freeaddrinfo(res);
        return REMOTE_ERR_NETWORK;
    }

    // Set socket options
    struct timeval timeout = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(ctx->sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(ctx->sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    int flag = 1;
    setsockopt(ctx->sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    ESP_LOGI(TAG, "Connecting to %s:%d...", host, port);
    ctx->state = CONN_STATE_CONNECTING;

    if (connect(ctx->sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "TCP connect failed: %d", errno);
        close(ctx->sock);
        ctx->sock = -1;
        ctx->state = CONN_STATE_ERROR;
        freeaddrinfo(res);
        return REMOTE_ERR_NETWORK;
    }

    freeaddrinfo(res);

    // Set non-blocking for receive
    int flags = fcntl(ctx->sock, F_GETFL, 0);
    fcntl(ctx->sock, F_SETFL, flags | O_NONBLOCK);

    ctx->connected = true;
    ctx->state = CONN_STATE_HANDSHAKE_INIT;
    ctx->last_receive_time = esp_timer_get_time() / 1000;

    ESP_LOGI(TAG, "TCP connected to %s:%d", host, port);

    // Reset protocol state
    protocol_reset(&ctx->protocol);
    crypto_reset_session(&ctx->crypto);

    return REMOTE_OK;
}

remote_error_t network_connect_discovered(network_context_t *ctx, int server_index) {
    if (server_index < 0 || server_index >= ctx->discovered_count) {
        return REMOTE_ERR_INVALID_PARAM;
    }

    const discovered_server_t *server = &ctx->discovered_servers[server_index];
    return network_connect(ctx, server->ip_addr, server->port);
}

void network_disconnect(network_context_t *ctx) {
    if (ctx->sock >= 0) {
        // Try to send session end
        if (ctx->state == CONN_STATE_ESTABLISHED) {
            size_t len;
            const uint8_t *pkt = protocol_build_session_end(&ctx->protocol, &len);
            if (pkt) {
                send(ctx->sock, pkt, len, 0);
            }
        }

        close(ctx->sock);
        ctx->sock = -1;
    }

    ctx->connected = false;
    ctx->state = CONN_STATE_DISCONNECTED;

    if (ctx->on_disconnected) {
        ctx->on_disconnected(ctx->user_data);
    }
}

bool network_is_connected(const network_context_t *ctx) {
    return ctx->connected && ctx->state == CONN_STATE_ESTABLISHED;
}

connection_state_t network_get_state(const network_context_t *ctx) {
    return ctx->state;
}

// =============================================================================
// Handshake
// =============================================================================

remote_error_t network_handshake(network_context_t *ctx) {
    if (!ctx->connected) {
        return REMOTE_ERR_NETWORK;
    }

    ESP_LOGI(TAG, "Starting handshake...");

    // Step 1: Send HandshakeInit
    size_t pkt_len;
    const uint8_t *pkt = protocol_build_handshake_init(&ctx->protocol, &pkt_len);
    if (!pkt) {
        ESP_LOGE(TAG, "Failed to build HandshakeInit");
        return REMOTE_ERR_CRYPTO;
    }

    if (send(ctx->sock, pkt, pkt_len, 0) != pkt_len) {
        ESP_LOGE(TAG, "Failed to send HandshakeInit");
        return REMOTE_ERR_NETWORK;
    }

    ctx->state = CONN_STATE_HANDSHAKE_RESPONSE;
    ESP_LOGI(TAG, "HandshakeInit sent, waiting for response...");

    // Step 2: Receive HandshakeResponse
    // Set blocking timeout for handshake
    struct timeval timeout = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(ctx->sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    uint8_t recv_buf[512];
    ssize_t recv_len = recv(ctx->sock, recv_buf, sizeof(recv_buf), 0);
    if (recv_len <= 0) {
        ESP_LOGE(TAG, "Failed to receive HandshakeResponse: %d", errno);
        return REMOTE_ERR_TIMEOUT;
    }

    protocol_feed_data(&ctx->protocol, recv_buf, recv_len);

    if (!protocol_packet_ready(&ctx->protocol)) {
        ESP_LOGE(TAG, "Incomplete HandshakeResponse");
        return REMOTE_ERR_PROTOCOL;
    }

    parsed_packet_t packet;
    remote_error_t err = protocol_parse_packet(&ctx->protocol, &packet, false);
    if (err != REMOTE_OK) {
        ESP_LOGE(TAG, "Failed to parse HandshakeResponse");
        return err;
    }

    if (packet.type != PKT_HANDSHAKE_RESPONSE) {
        ESP_LOGE(TAG, "Expected HandshakeResponse, got %d", packet.type);
        return REMOTE_ERR_PROTOCOL;
    }

    // Process response and derive session keys
    err = protocol_handle_handshake_response(&ctx->protocol, &packet);
    if (err != REMOTE_OK) {
        ESP_LOGE(TAG, "HandshakeResponse processing failed");
        return err;
    }

    protocol_consume_packet(&ctx->protocol);

    // Step 3: Send HandshakeComplete
    ctx->state = CONN_STATE_HANDSHAKE_COMPLETE;

    pkt = protocol_build_handshake_complete(&ctx->protocol, &pkt_len);
    if (!pkt) {
        ESP_LOGE(TAG, "Failed to build HandshakeComplete");
        return REMOTE_ERR_CRYPTO;
    }

    if (send(ctx->sock, pkt, pkt_len, 0) != pkt_len) {
        ESP_LOGE(TAG, "Failed to send HandshakeComplete");
        return REMOTE_ERR_NETWORK;
    }

    // Wait for SessionStart
    recv_len = recv(ctx->sock, recv_buf, sizeof(recv_buf), 0);
    if (recv_len <= 0) {
        ESP_LOGE(TAG, "Failed to receive SessionStart: %d", errno);
        return REMOTE_ERR_TIMEOUT;
    }

    protocol_feed_data(&ctx->protocol, recv_buf, recv_len);

    if (protocol_packet_ready(&ctx->protocol)) {
        err = protocol_parse_packet(&ctx->protocol, &packet, true);
        if (err == REMOTE_OK && packet.type == PKT_SESSION_START) {
            protocol_consume_packet(&ctx->protocol);
            ctx->state = CONN_STATE_ESTABLISHED;
            ctx->last_heartbeat_time = esp_timer_get_time() / 1000;

            ESP_LOGI(TAG, "Handshake complete - session established!");

            // Restore non-blocking
            timeout.tv_sec = 0;
            timeout.tv_usec = 0;
            setsockopt(ctx->sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

            if (ctx->on_connected) {
                ctx->on_connected(ctx->user_data);
            }

            return REMOTE_OK;
        }
    }

    ESP_LOGE(TAG, "Expected SessionStart, handshake failed");
    return REMOTE_ERR_HANDSHAKE_FAILED;
}

// =============================================================================
// Data Transfer
// =============================================================================

remote_error_t network_send(network_context_t *ctx,
                            const uint8_t *data, size_t len) {
    if (!ctx->connected) {
        return REMOTE_ERR_NETWORK;
    }

    ssize_t sent = send(ctx->sock, data, len, 0);
    if (sent != len) {
        ESP_LOGE(TAG, "Send failed: %d (errno %d)", (int)sent, errno);
        return REMOTE_ERR_NETWORK;
    }

    ctx->bytes_sent += len;
    ctx->packets_sent++;
    return REMOTE_OK;
}

int network_receive(network_context_t *ctx, uint32_t timeout_ms) {
    if (!ctx->connected) {
        return -1;
    }

    if (timeout_ms > 0) {
        fd_set rfds;
        struct timeval tv = {
            .tv_sec = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };

        FD_ZERO(&rfds);
        FD_SET(ctx->sock, &rfds);

        int ret = select(ctx->sock + 1, &rfds, NULL, NULL, &tv);
        if (ret <= 0) {
            return 0;  // No data or timeout
        }
    }

    uint8_t temp[4096];
    ssize_t len = recv(ctx->sock, temp, sizeof(temp), 0);

    if (len > 0) {
        protocol_feed_data(&ctx->protocol, temp, len);
        ctx->bytes_received += len;
        ctx->last_receive_time = esp_timer_get_time() / 1000;
        return len;
    } else if (len == 0) {
        // Connection closed
        ESP_LOGI(TAG, "Connection closed by server");
        network_disconnect(ctx);
        return -1;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGE(TAG, "Receive error: %d", errno);
        network_disconnect(ctx);
        return -1;
    }

    return 0;
}

remote_error_t network_process(network_context_t *ctx) {
    while (protocol_packet_ready(&ctx->protocol)) {
        parsed_packet_t packet;
        remote_error_t err = protocol_parse_packet(&ctx->protocol, &packet, true);

        if (err != REMOTE_OK) {
            ESP_LOGE(TAG, "Packet parse error: %d", err);
            protocol_consume_packet(&ctx->protocol);
            if (ctx->on_error) {
                ctx->on_error(err, ctx->user_data);
            }
            continue;
        }

        ctx->packets_received++;

        switch (packet.type) {
            case PKT_SCREEN_FRAME: {
                screen_frame_t frame;
                err = protocol_parse_screen_frame(&packet, &frame);
                if (err == REMOTE_OK && ctx->on_frame_received) {
                    ctx->on_frame_received(frame.jpeg_data, frame.jpeg_len,
                                          ctx->user_data);
                }
                break;
            }

            case PKT_HEARTBEAT:
                network_send_heartbeat(ctx);
                break;

            case PKT_HEARTBEAT_ACK:
                // Latency measurement could go here
                break;

            case PKT_SESSION_END:
            case PKT_SESSION_TIMEOUT:
                ESP_LOGI(TAG, "Session ended by server");
                network_disconnect(ctx);
                break;

            case PKT_MODE_ACK:
                ESP_LOGI(TAG, "Mode switch acknowledged");
                break;

            default:
                ESP_LOGD(TAG, "Unhandled packet type: %d", packet.type);
                break;
        }

        protocol_consume_packet(&ctx->protocol);
    }

    // Check for heartbeat timeout
    int64_t now = esp_timer_get_time() / 1000;
    if (now - ctx->last_heartbeat_time > HEARTBEAT_INTERVAL_MS) {
        network_send_heartbeat(ctx);
        ctx->last_heartbeat_time = now;
    }

    // Check for session timeout
    if (now - ctx->last_receive_time > SESSION_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Session timeout");
        network_disconnect(ctx);
        return REMOTE_ERR_TIMEOUT;
    }

    return REMOTE_OK;
}

// =============================================================================
// Input Sending
// =============================================================================

remote_error_t network_send_mouse_move(network_context_t *ctx, int8_t dx, int8_t dy) {
    size_t len;
    const uint8_t *pkt = protocol_build_mouse_move(&ctx->protocol, dx, dy, &len);
    if (!pkt) return REMOTE_ERR_CRYPTO;
    return network_send(ctx, pkt, len);
}

remote_error_t network_send_mouse_click(network_context_t *ctx,
                                        mouse_button_t button, bool pressed) {
    size_t len;
    const uint8_t *pkt = protocol_build_mouse_click(&ctx->protocol, button, pressed, &len);
    if (!pkt) return REMOTE_ERR_CRYPTO;
    return network_send(ctx, pkt, len);
}

remote_error_t network_send_key_press(network_context_t *ctx,
                                      uint8_t keycode, uint8_t modifiers) {
    size_t len;
    const uint8_t *pkt = protocol_build_key_press(&ctx->protocol, keycode, modifiers, &len);
    if (!pkt) return REMOTE_ERR_CRYPTO;
    return network_send(ctx, pkt, len);
}

remote_error_t network_send_key_release(network_context_t *ctx,
                                        uint8_t keycode, uint8_t modifiers) {
    size_t len;
    const uint8_t *pkt = protocol_build_key_release(&ctx->protocol, keycode, modifiers, &len);
    if (!pkt) return REMOTE_ERR_CRYPTO;
    return network_send(ctx, pkt, len);
}

remote_error_t network_send_mode_switch(network_context_t *ctx, input_mode_t mode) {
    size_t len;
    const uint8_t *pkt = protocol_build_mode_switch(&ctx->protocol, mode, &len);
    if (!pkt) return REMOTE_ERR_CRYPTO;
    return network_send(ctx, pkt, len);
}

remote_error_t network_send_heartbeat(network_context_t *ctx) {
    size_t len;
    const uint8_t *pkt = protocol_build_heartbeat(&ctx->protocol, &len);
    if (!pkt) return REMOTE_ERR_CRYPTO;
    return network_send(ctx, pkt, len);
}

remote_error_t network_send_session_end(network_context_t *ctx) {
    size_t len;
    const uint8_t *pkt = protocol_build_session_end(&ctx->protocol, &len);
    if (!pkt) return REMOTE_ERR_CRYPTO;
    return network_send(ctx, pkt, len);
}

// =============================================================================
// Callbacks & Stats
// =============================================================================

void network_set_on_connected(network_context_t *ctx,
                              void (*callback)(void *), void *user_data) {
    ctx->on_connected = callback;
    ctx->user_data = user_data;
}

void network_set_on_disconnected(network_context_t *ctx,
                                 void (*callback)(void *), void *user_data) {
    ctx->on_disconnected = callback;
    ctx->user_data = user_data;
}

void network_set_on_frame(network_context_t *ctx,
                          void (*callback)(const uint8_t *, size_t, void *),
                          void *user_data) {
    ctx->on_frame_received = callback;
    ctx->user_data = user_data;
}

void network_set_on_error(network_context_t *ctx,
                          void (*callback)(remote_error_t, void *),
                          void *user_data) {
    ctx->on_error = callback;
    ctx->user_data = user_data;
}

void network_get_stats(const network_context_t *ctx, network_stats_t *stats) {
    stats->bytes_sent = ctx->bytes_sent;
    stats->bytes_received = ctx->bytes_received;
    stats->packets_sent = ctx->packets_sent;
    stats->packets_received = ctx->packets_received;
    stats->connected = ctx->connected;
    stats->latency_ms = 0;  // TODO: Calculate from heartbeat round-trip
}

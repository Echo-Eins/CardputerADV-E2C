/**
 * @file main.c
 * @brief Cardputer Remote Client - Main Application
 *
 * Event-driven remote desktop client for M5Stack Cardputer
 *
 * Architecture:
 * - Main task: Event loop processing network/display/input
 * - Input task: Keyboard matrix scanning
 * - Network: Non-blocking receive with callbacks
 * - Display: DMA-based rendering with double buffering
 */

#include <stdio.h>
#include <string.h>
#include <nvs_flash.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include "remote_config.h"
#include "remote_crypto.h"
#include "remote_protocol.h"
#include "remote_network.h"
#include "remote_display.h"
#include "remote_input.h"

static const char *TAG = "remote_main";

// =============================================================================
// Application Context
// =============================================================================

typedef struct {
    // Modules
    network_context_t network;
    display_context_t display;
    input_context_t input;

    // Configuration
    remote_config_t config;

    // State
    bool running;
    connection_state_t conn_state;

    // Statistics
    remote_stats_t stats;
    uint32_t frame_count;
    int64_t last_fps_time;
    float current_fps;

    // Event group for synchronization
    EventGroupHandle_t events;
} app_context_t;

static app_context_t app;

// Event bits
#define EVT_FRAME_RECEIVED  BIT0
#define EVT_CONNECTED       BIT1
#define EVT_DISCONNECTED    BIT2
#define EVT_ERROR           BIT3

// =============================================================================
// Default Configuration
// =============================================================================

static void load_default_config(remote_config_t *cfg) {
    // WiFi - should be configured before use
    strncpy(cfg->wifi_ssid, "CardputerRemote", sizeof(cfg->wifi_ssid));
    strncpy(cfg->wifi_password, "password123", sizeof(cfg->wifi_password));

    // Server
    strncpy(cfg->server_host, "", sizeof(cfg->server_host));
    cfg->server_port = REMOTE_DEFAULT_PORT;

    // Discovery cookie (16 bytes) - must match server config
    memset(cfg->discovery_cookie, 0, DISCOVERY_COOKIE_SIZE);

    // Keys - MUST be replaced with real keys
    // These are placeholder zeros - generate real keys before use!
    memset(cfg->private_key, 0, ECDH_PRIVATE_KEY_SIZE);
    memset(cfg->server_public_key, 0, ECDH_PUBLIC_KEY_SIZE);

    // Display
    cfg->brightness = 128;
    cfg->auto_sleep = true;
    cfg->sleep_timeout_ms = 60000;  // 1 minute

    ESP_LOGW(TAG, "Using default config - please configure keys!");
}

// =============================================================================
// Network Callbacks
// =============================================================================

static void on_frame_received(const uint8_t *jpeg_data, size_t len, void *user_data) {
    app_context_t *ctx = (app_context_t *)user_data;

    // Render frame to display
    remote_error_t err = display_render_jpeg(&ctx->display, jpeg_data, len);
    if (err == REMOTE_OK) {
        ctx->frame_count++;
        ctx->stats.frames_received++;
    } else {
        ctx->stats.decode_errors++;
    }

    xEventGroupSetBits(ctx->events, EVT_FRAME_RECEIVED);
}

static void on_connected(void *user_data) {
    app_context_t *ctx = (app_context_t *)user_data;
    ctx->conn_state = CONN_STATE_ESTABLISHED;
    ESP_LOGI(TAG, "Connected to server!");
    display_draw_status(&ctx->display, "Connected", 0);
    xEventGroupSetBits(ctx->events, EVT_CONNECTED);
}

static void on_disconnected(void *user_data) {
    app_context_t *ctx = (app_context_t *)user_data;
    ctx->conn_state = CONN_STATE_DISCONNECTED;
    ESP_LOGI(TAG, "Disconnected from server");
    display_draw_status(&ctx->display, "Disconnected", 0);
    xEventGroupSetBits(ctx->events, EVT_DISCONNECTED);
}

static void on_error(remote_error_t error, void *user_data) {
    app_context_t *ctx = (app_context_t *)user_data;
    ESP_LOGE(TAG, "Network error: %d", error);
    ctx->stats.crypto_errors++;
    xEventGroupSetBits(ctx->events, EVT_ERROR);
}

// =============================================================================
// Input Processing
// =============================================================================

static void process_input(app_context_t *ctx) {
    input_event_t event;

    while (input_get_event(&ctx->input, &event)) {
        // Handle special keys first
        if (event.is_special) {
            switch (event.keycode) {
                case KEY_MODE_SWITCH:
                    if (event.pressed) {
                        input_mode_t new_mode = input_toggle_mode(&ctx->input);
                        network_send_mode_switch(&ctx->network, new_mode);

                        const char *mode_str = (new_mode == INPUT_MODE_MOUSE) ?
                                              "Mode: Mouse" : "Mode: Keyboard";
                        display_draw_status(&ctx->display, mode_str, 1);
                    }
                    continue;

                case KEY_BRIGHTNESS_UP:
                    if (event.pressed) {
                        uint8_t br = ctx->config.brightness;
                        br = (br > 230) ? 255 : br + 25;
                        ctx->config.brightness = br;
                        display_set_brightness(&ctx->display, br);
                    }
                    continue;

                case KEY_BRIGHTNESS_DN:
                    if (event.pressed) {
                        uint8_t br = ctx->config.brightness;
                        br = (br < 25) ? 0 : br - 25;
                        ctx->config.brightness = br;
                        display_set_brightness(&ctx->display, br);
                    }
                    continue;
            }
        }

        // Skip if not connected
        if (!network_is_connected(&ctx->network)) {
            continue;
        }

        // Process based on current mode
        input_mode_t mode = input_get_mode(&ctx->input);

        if (mode == INPUT_MODE_MOUSE) {
            // In mouse mode, arrow keys move cursor
            if (input_is_arrow_key(event.keycode)) {
                if (event.pressed) {
                    int8_t dx, dy;
                    input_arrow_to_mouse(event.keycode, &dx, &dy);
                    network_send_mouse_move(&ctx->network, dx, dy);
                }
            }
            // Enter = left click, Backspace = right click
            else if (event.keycode == HID_KEY_ENTER) {
                network_send_mouse_click(&ctx->network, MOUSE_BUTTON_LEFT, event.pressed);
            }
            else if (event.keycode == HID_KEY_BACKSPACE) {
                network_send_mouse_click(&ctx->network, MOUSE_BUTTON_RIGHT, event.pressed);
            }
            // Space = middle click
            else if (event.keycode == HID_KEY_SPACE) {
                network_send_mouse_click(&ctx->network, MOUSE_BUTTON_MIDDLE, event.pressed);
            }
        } else {
            // Keyboard mode - send key events
            if (event.pressed) {
                network_send_key_press(&ctx->network, event.keycode, event.modifiers);
            } else {
                network_send_key_release(&ctx->network, event.keycode, event.modifiers);
            }
        }

        ctx->stats.packets_sent++;
    }
}

// =============================================================================
// FPS Calculation
// =============================================================================

static void update_fps(app_context_t *ctx) {
    int64_t now = esp_timer_get_time() / 1000;
    int64_t elapsed = now - ctx->last_fps_time;

    if (elapsed >= 1000) {
        ctx->current_fps = (float)ctx->frame_count * 1000.0f / (float)elapsed;
        ctx->frame_count = 0;
        ctx->last_fps_time = now;
        ctx->stats.avg_fps = ctx->current_fps;
    }
}

// =============================================================================
// Connection Management
// =============================================================================

static remote_error_t connect_to_server(app_context_t *ctx) {
    display_draw_status(&ctx->display, "Connecting WiFi...", 0);

    // Connect to WiFi
    remote_error_t err = network_wifi_connect(&ctx->network);
    if (err != REMOTE_OK) {
        display_draw_status(&ctx->display, "WiFi Failed!", 0);
        return err;
    }

    display_draw_status(&ctx->display, "WiFi Connected", 0);
    vTaskDelay(pdMS_TO_TICKS(500));

    // Try mDNS discovery first
    display_draw_status(&ctx->display, "Discovering...", 0);
    int found = network_discover_servers(&ctx->network, 5000);

    if (found > 0) {
        display_draw_status(&ctx->display, "Server found!", 0);
        err = network_connect_discovered(&ctx->network, 0);
    } else if (strlen(ctx->config.server_host) > 0) {
        // Fallback to configured host
        char status[32];
        snprintf(status, sizeof(status), "Connecting %s", ctx->config.server_host);
        display_draw_status(&ctx->display, status, 0);
        err = network_connect(&ctx->network, ctx->config.server_host,
                              ctx->config.server_port);
    } else {
        display_draw_status(&ctx->display, "No server found", 0);
        return REMOTE_ERR_NETWORK;
    }

    if (err != REMOTE_OK) {
        display_draw_status(&ctx->display, "Connect failed!", 0);
        return err;
    }

    // Perform handshake
    display_draw_status(&ctx->display, "Handshake...", 0);
    err = network_handshake(&ctx->network);
    if (err != REMOTE_OK) {
        display_draw_status(&ctx->display, "Handshake failed!", 0);
        network_disconnect(&ctx->network);
        return err;
    }

    display_clear(&ctx->display);
    return REMOTE_OK;
}

// =============================================================================
// Main Event Loop
// =============================================================================

static void event_loop(app_context_t *ctx) {
    ESP_LOGI(TAG, "Starting event loop");

    while (ctx->running) {
        // Process network data
        if (network_is_connected(&ctx->network)) {
            // Receive any pending data
            network_receive(&ctx->network, 10);

            // Process received packets
            network_process(&ctx->network);
        }

        // Process keyboard input
        process_input(ctx);

        // Update FPS counter
        update_fps(ctx);

        // Handle disconnection
        if (ctx->conn_state == CONN_STATE_DISCONNECTED) {
            // Auto-reconnect after delay
            vTaskDelay(pdMS_TO_TICKS(5000));

            if (ctx->running) {
                ESP_LOGI(TAG, "Attempting reconnection...");
                connect_to_server(ctx);
            }
        }

        // Small yield to prevent watchdog
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// =============================================================================
// Application Entry Point
// =============================================================================

void app_main(void) {
    ESP_LOGI(TAG, "Cardputer Remote Client v1.0");
    ESP_LOGI(TAG, "Protocol version: %d", PROTOCOL_VERSION);

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize app context
    memset(&app, 0, sizeof(app));
    app.running = true;
    app.last_fps_time = esp_timer_get_time() / 1000;

    // Create event group
    app.events = xEventGroupCreate();
    if (!app.events) {
        ESP_LOGE(TAG, "Failed to create event group");
        return;
    }

    // Load configuration
    load_default_config(&app.config);

    // TODO: Load config from NVS or SPIFFS

    // Initialize display
    ESP_LOGI(TAG, "Initializing display...");
    remote_error_t err = display_init(&app.display);
    if (err != REMOTE_OK) {
        ESP_LOGE(TAG, "Display init failed: %d", err);
        return;
    }

    display_clear(&app.display);
    display_draw_status(&app.display, "Cardputer Remote", 0);
    display_draw_status(&app.display, "Initializing...", 1);

    // Initialize input
    ESP_LOGI(TAG, "Initializing input...");
    err = input_init(&app.input);
    if (err != REMOTE_OK) {
        ESP_LOGE(TAG, "Input init failed: %d", err);
        display_deinit(&app.display);
        return;
    }

    err = input_start(&app.input);
    if (err != REMOTE_OK) {
        ESP_LOGE(TAG, "Input start failed: %d", err);
        input_deinit(&app.input);
        display_deinit(&app.display);
        return;
    }

    // Initialize network
    ESP_LOGI(TAG, "Initializing network...");
    err = network_init(&app.network, &app.config);
    if (err != REMOTE_OK) {
        ESP_LOGE(TAG, "Network init failed: %d", err);
        input_deinit(&app.input);
        display_deinit(&app.display);
        return;
    }

    // Set network callbacks
    network_set_on_frame(&app.network, on_frame_received, &app);
    network_set_on_connected(&app.network, on_connected, &app);
    network_set_on_disconnected(&app.network, on_disconnected, &app);
    network_set_on_error(&app.network, on_error, &app);

    // Connect to server
    ESP_LOGI(TAG, "Connecting to server...");
    err = connect_to_server(&app);
    if (err != REMOTE_OK) {
        ESP_LOGW(TAG, "Initial connection failed, will retry in event loop");
    }

    // Run main event loop
    event_loop(&app);

    // Cleanup
    ESP_LOGI(TAG, "Shutting down...");
    network_deinit(&app.network);
    input_deinit(&app.input);
    display_deinit(&app.display);

    if (app.events) {
        vEventGroupDelete(app.events);
    }

    ESP_LOGI(TAG, "Goodbye!");
}

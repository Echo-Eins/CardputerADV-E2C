/**
 * @file remote_input.c
 * @brief Keyboard input implementation for Cardputer
 */

#include "remote_input.h"
#include <string.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>

static const char *TAG = "remote_input";

// =============================================================================
// Cardputer Keyboard Layout
// =============================================================================

// Normal keymap [row][col] - 8 rows x 7 columns via 74HC138 decoder
// Layout based on M5Stack Cardputer physical keyboard
static const uint8_t KEYMAP_NORMAL[KB_ROW_COUNT][KB_COL_COUNT] = {
    // Row 0 (decoder Y0)
    {HID_KEY_GRAVE, HID_KEY_1, HID_KEY_2, HID_KEY_3, HID_KEY_4, HID_KEY_5, HID_KEY_6},
    // Row 1 (decoder Y1)
    {HID_KEY_7, HID_KEY_8, HID_KEY_9, HID_KEY_0, HID_KEY_MINUS, HID_KEY_EQUAL, HID_KEY_BACKSPACE},
    // Row 2 (decoder Y2)
    {HID_KEY_TAB, HID_KEY_Q, HID_KEY_W, HID_KEY_E, HID_KEY_R, HID_KEY_T, HID_KEY_Y},
    // Row 3 (decoder Y3)
    {HID_KEY_U, HID_KEY_I, HID_KEY_O, HID_KEY_P, HID_KEY_LBRACKET, HID_KEY_RBRACKET, HID_KEY_BACKSLASH},
    // Row 4 (decoder Y4)
    {KEY_FN, HID_KEY_A, HID_KEY_S, HID_KEY_D, HID_KEY_F, HID_KEY_G, HID_KEY_H},
    // Row 5 (decoder Y5)
    {HID_KEY_J, HID_KEY_K, HID_KEY_L, HID_KEY_SEMICOLON, HID_KEY_APOSTROPHE, HID_KEY_ENTER, HID_KEY_NONE},
    // Row 6 (decoder Y6)
    {HID_KEY_NONE, HID_KEY_Z, HID_KEY_X, HID_KEY_C, HID_KEY_V, HID_KEY_B, HID_KEY_N},
    // Row 7 (decoder Y7)
    {HID_KEY_M, HID_KEY_COMMA, HID_KEY_PERIOD, HID_KEY_SLASH, HID_KEY_NONE, HID_KEY_SPACE, HID_KEY_NONE},
};

// FN keymap [row][col] - keys that change with FN held (8x7 matrix)
static const uint8_t KEYMAP_FN[KB_ROW_COUNT][KB_COL_COUNT] = {
    // Row 0 - ESC and F1-F6
    {HID_KEY_ESCAPE, HID_KEY_F1, HID_KEY_F2, HID_KEY_F3, HID_KEY_F4, HID_KEY_F5, HID_KEY_F6},
    // Row 1 - F7-F12 and Delete
    {HID_KEY_F7, HID_KEY_F8, HID_KEY_F9, HID_KEY_F10, HID_KEY_F11, HID_KEY_F12, HID_KEY_DELETE},
    // Row 2 - Navigation
    {HID_KEY_NONE, HID_KEY_NONE, HID_KEY_UP, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_HOME},
    // Row 3
    {HID_KEY_END, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_PAGEUP, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_NONE},
    // Row 4 - Arrow keys (FN + A/S/D for left/down/right)
    {KEY_FN, HID_KEY_LEFT, HID_KEY_DOWN, HID_KEY_RIGHT, HID_KEY_PAGEDOWN, HID_KEY_NONE, HID_KEY_NONE},
    // Row 5
    {HID_KEY_NONE, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_NONE},
    // Row 6 - Special functions
    {HID_KEY_NONE, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_NONE},
    // Row 7 - Mode switch and brightness
    {KEY_MODE_SWITCH, KEY_BRIGHTNESS_DN, KEY_BRIGHTNESS_UP, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_NONE, HID_KEY_NONE},
};

// Modifier keys locations in 8x7 matrix
// Note: The Cardputer has Shift, Ctrl, Alt in specific positions
// These need to be verified against actual hardware layout
#define MOD_SHIFT_ROW   6
#define MOD_SHIFT_COL   0
#define MOD_CTRL_ROW    7
#define MOD_CTRL_COL    4
#define MOD_ALT_ROW     7
#define MOD_ALT_COL     6

// =============================================================================
// GPIO Initialization for 74HC138 Decoder-based Keyboard
// =============================================================================

static void init_gpio_pins(void) {
    // Configure 74HC138 address pins as outputs
    // These 3 pins select which of the 8 rows is active
    gpio_config_t addr_conf = {
        .pin_bit_mask = (1ULL << KB_ADDR_A0) | (1ULL << KB_ADDR_A1) | (1ULL << KB_ADDR_A2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&addr_conf);

    // Initialize address pins to 0 (select row 0)
    gpio_set_level(KB_ADDR_A0, 0);
    gpio_set_level(KB_ADDR_A1, 0);
    gpio_set_level(KB_ADDR_A2, 0);

    // Configure column pins as inputs with pull-down
    // The 74HC138 outputs are active-low, so when a key is pressed
    // and the corresponding row is selected, the column is pulled LOW
    for (int i = 0; i < KB_COL_COUNT; i++) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << KB_COL_PINS[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,  // Pull-up, key press pulls LOW
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
    }
}

/**
 * @brief Select a row using the 74HC138 decoder
 * @param row Row number (0-7)
 *
 * The 74HC138 uses 3 address inputs (A0, A1, A2) to select one of 8 outputs.
 * Address encoding: row 0 = 000, row 1 = 001, row 2 = 010, etc.
 */
static void select_row(int row) {
    gpio_set_level(KB_ADDR_A0, (row >> 0) & 1);
    gpio_set_level(KB_ADDR_A1, (row >> 1) & 1);
    gpio_set_level(KB_ADDR_A2, (row >> 2) & 1);
}

// =============================================================================
// Matrix Scanning (using 74HC138 decoder for row selection)
// =============================================================================

static void scan_matrix(input_context_t *ctx) {
    // Save previous state
    memcpy(ctx->prev_state, ctx->key_state, sizeof(ctx->key_state));

    // Scan each row using the 74HC138 decoder
    for (int row = 0; row < KB_ROW_COUNT; row++) {
        // Select current row via decoder address pins
        select_row(row);

        // Small delay for decoder and signal to settle
        esp_rom_delay_us(10);

        // Read all column pins
        // When row is selected (decoder output LOW) and key is pressed,
        // the column pin is pulled LOW through the switch
        for (int col = 0; col < KB_COL_COUNT; col++) {
            ctx->key_state[row][col] = (gpio_get_level(KB_COL_PINS[col]) == 0) ? 1 : 0;
        }
    }
}

// =============================================================================
// Key Mapping
// =============================================================================

uint8_t input_matrix_to_hid(int row, int col, bool fn_pressed, uint8_t *modifiers) {
    *modifiers = 0;

    // Check for modifier keys
    if (row == MOD_SHIFT_ROW && col == MOD_SHIFT_COL) {
        *modifiers = MOD_LSHIFT;
        return HID_KEY_NONE;
    }
    if (row == MOD_CTRL_ROW && col == MOD_CTRL_COL) {
        *modifiers = MOD_LCTRL;
        return HID_KEY_NONE;
    }
    if (row == MOD_ALT_ROW && col == MOD_ALT_COL) {
        *modifiers = MOD_LALT;
        return HID_KEY_NONE;
    }

    // Get keycode from appropriate map
    if (fn_pressed) {
        uint8_t fn_key = KEYMAP_FN[row][col];
        if (fn_key != HID_KEY_NONE) {
            return fn_key;
        }
    }

    return KEYMAP_NORMAL[row][col];
}

bool input_is_arrow_key(uint8_t keycode) {
    return keycode == HID_KEY_UP ||
           keycode == HID_KEY_DOWN ||
           keycode == HID_KEY_LEFT ||
           keycode == HID_KEY_RIGHT;
}

void input_arrow_to_mouse(uint8_t keycode, int8_t *dx, int8_t *dy) {
    *dx = 0;
    *dy = 0;

    switch (keycode) {
        case HID_KEY_UP:
            *dy = -MOUSE_MOVE_STEP;
            break;
        case HID_KEY_DOWN:
            *dy = MOUSE_MOVE_STEP;
            break;
        case HID_KEY_LEFT:
            *dx = -MOUSE_MOVE_STEP;
            break;
        case HID_KEY_RIGHT:
            *dx = MOUSE_MOVE_STEP;
            break;
    }
}

// =============================================================================
// Event Processing
// =============================================================================

static void process_key_changes(input_context_t *ctx) {
    uint8_t new_modifiers = 0;

    // First pass: update modifiers and FN state
    for (int row = 0; row < KB_ROW_COUNT; row++) {
        for (int col = 0; col < KB_COL_COUNT; col++) {
            if (ctx->key_state[row][col]) {
                uint8_t mod;
                uint8_t key = input_matrix_to_hid(row, col, false, &mod);

                if (mod != 0) {
                    new_modifiers |= mod;
                }

                if (key == KEY_FN) {
                    ctx->fn_pressed = true;
                }
            }
        }
    }

    // Update FN state from current scan
    ctx->fn_pressed = false;
    for (int row = 0; row < KB_ROW_COUNT; row++) {
        for (int col = 0; col < KB_COL_COUNT; col++) {
            if (ctx->key_state[row][col]) {
                uint8_t mod;
                uint8_t key = input_matrix_to_hid(row, col, false, &mod);
                if (key == KEY_FN) {
                    ctx->fn_pressed = true;
                    break;
                }
            }
        }
        if (ctx->fn_pressed) break;
    }

    ctx->current_modifiers = new_modifiers;

    // Second pass: detect key changes and generate events
    for (int row = 0; row < KB_ROW_COUNT; row++) {
        for (int col = 0; col < KB_COL_COUNT; col++) {
            uint8_t current = ctx->key_state[row][col];
            uint8_t previous = ctx->prev_state[row][col];

            if (current != previous) {
                uint8_t mod;
                uint8_t keycode = input_matrix_to_hid(row, col, ctx->fn_pressed, &mod);

                // Skip modifier-only keys and FN key
                if (keycode == HID_KEY_NONE || keycode == KEY_FN) {
                    continue;
                }

                input_event_t event = {
                    .keycode = keycode,
                    .modifiers = ctx->current_modifiers,
                    .pressed = (current == 1),
                    .is_special = (keycode >= 0xF0),
                };

                // Queue event
                if (ctx->event_queue) {
                    xQueueSend(ctx->event_queue, &event, 0);
                }

                // Handle auto-repeat
                if (event.pressed && ctx->auto_repeat_enabled) {
                    ctx->repeat_keycode = keycode;
                    ctx->repeat_start_time = esp_timer_get_time() / 1000;
                    ctx->last_repeat_time = 0;
                } else if (!event.pressed && keycode == ctx->repeat_keycode) {
                    ctx->repeat_keycode = HID_KEY_NONE;
                }
            }
        }
    }
}

static void process_auto_repeat(input_context_t *ctx) {
    if (!ctx->auto_repeat_enabled || ctx->repeat_keycode == HID_KEY_NONE) {
        return;
    }

    uint32_t now = esp_timer_get_time() / 1000;
    uint32_t elapsed = now - ctx->repeat_start_time;

    if (elapsed < ctx->repeat_delay_ms) {
        return;
    }

    uint32_t repeat_elapsed = now - ctx->last_repeat_time;
    if (ctx->last_repeat_time == 0 || repeat_elapsed >= ctx->repeat_interval_ms) {
        input_event_t event = {
            .keycode = ctx->repeat_keycode,
            .modifiers = ctx->current_modifiers,
            .pressed = true,
            .is_special = (ctx->repeat_keycode >= 0xF0),
        };

        if (ctx->event_queue) {
            xQueueSend(ctx->event_queue, &event, 0);
        }

        ctx->last_repeat_time = now;
    }
}

// =============================================================================
// Scanning Task
// =============================================================================

static void input_scan_task(void *arg) {
    input_context_t *ctx = (input_context_t *)arg;

    while (ctx->running) {
        uint32_t now = esp_timer_get_time() / 1000;

        // Debounce check
        if (now - ctx->last_scan_time >= ctx->debounce_ms) {
            scan_matrix(ctx);
            process_key_changes(ctx);
            ctx->last_scan_time = now;
        }

        // Auto-repeat
        process_auto_repeat(ctx);

        // Small delay to prevent CPU hogging
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    vTaskDelete(NULL);
}

// =============================================================================
// Public API
// =============================================================================

remote_error_t input_init(input_context_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));

    ctx->mode = INPUT_MODE_KEYBOARD;
    ctx->debounce_ms = 20;
    ctx->auto_repeat_enabled = true;
    ctx->repeat_delay_ms = 500;
    ctx->repeat_interval_ms = 50;
    ctx->repeat_keycode = HID_KEY_NONE;

    // Create event queue
    ctx->event_queue = xQueueCreate(INPUT_QUEUE_SIZE, sizeof(input_event_t));
    if (!ctx->event_queue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return REMOTE_ERR_NO_MEMORY;
    }

    // Initialize GPIO
    init_gpio_pins();

    ESP_LOGI(TAG, "Input module initialized");
    return REMOTE_OK;
}

void input_deinit(input_context_t *ctx) {
    input_stop(ctx);

    if (ctx->event_queue) {
        vQueueDelete(ctx->event_queue);
    }

    memset(ctx, 0, sizeof(*ctx));
}

remote_error_t input_start(input_context_t *ctx) {
    if (ctx->running) {
        return REMOTE_OK;
    }

    ctx->running = true;

    BaseType_t ret = xTaskCreate(input_scan_task, "kb_scan",
                                 4096, ctx, 5, &ctx->scan_task);
    if (ret != pdPASS) {
        ctx->running = false;
        ESP_LOGE(TAG, "Failed to create scan task");
        return REMOTE_ERR_NO_MEMORY;
    }

    ESP_LOGI(TAG, "Keyboard scanning started");
    return REMOTE_OK;
}

void input_stop(input_context_t *ctx) {
    ctx->running = false;

    // Wait for task to exit
    if (ctx->scan_task) {
        vTaskDelay(pdMS_TO_TICKS(50));
        ctx->scan_task = NULL;
    }
}

bool input_get_event(input_context_t *ctx, input_event_t *event) {
    if (!ctx->event_queue) {
        return false;
    }

    return xQueueReceive(ctx->event_queue, event, 0) == pdTRUE;
}

bool input_wait_event(input_context_t *ctx, input_event_t *event, uint32_t timeout_ms) {
    if (!ctx->event_queue) {
        return false;
    }

    TickType_t ticks = (timeout_ms == portMAX_DELAY) ?
                       portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    return xQueueReceive(ctx->event_queue, event, ticks) == pdTRUE;
}

input_mode_t input_get_mode(const input_context_t *ctx) {
    return ctx->mode;
}

void input_set_mode(input_context_t *ctx, input_mode_t mode) {
    ctx->mode = mode;
    ESP_LOGI(TAG, "Input mode: %s", mode == INPUT_MODE_MOUSE ? "Mouse" : "Keyboard");
}

input_mode_t input_toggle_mode(input_context_t *ctx) {
    ctx->mode = (ctx->mode == INPUT_MODE_MOUSE) ?
                INPUT_MODE_KEYBOARD : INPUT_MODE_MOUSE;
    ESP_LOGI(TAG, "Input mode toggled: %s",
             ctx->mode == INPUT_MODE_MOUSE ? "Mouse" : "Keyboard");
    return ctx->mode;
}

void input_set_debounce(input_context_t *ctx, uint32_t ms) {
    ctx->debounce_ms = ms;
}

void input_set_auto_repeat(input_context_t *ctx, bool enable,
                           uint32_t delay_ms, uint32_t interval_ms) {
    ctx->auto_repeat_enabled = enable;
    ctx->repeat_delay_ms = delay_ms;
    ctx->repeat_interval_ms = interval_ms;
}

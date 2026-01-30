/**
 * @file remote_input.h
 * @brief Keyboard input handling for Cardputer
 *
 * Maps Cardputer's matrix keyboard to USB HID keycodes
 */

#ifndef REMOTE_INPUT_H
#define REMOTE_INPUT_H

#include "remote_config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Cardputer Keyboard Matrix Pins
// =============================================================================

// Row pins (directly matched from Cardputer schematic)
#define KB_ROW_COUNT    7
#define KB_COL_COUNT    8

// GPIO pins for keyboard matrix (M5Stack Cardputer)
static const int KB_ROW_PINS[KB_ROW_COUNT] = {8, 9, 11, 13, 15, 3, 4};
static const int KB_COL_PINS[KB_COL_COUNT] = {18, 17, 16, 14, 12, 10, 7, 5};

// =============================================================================
// USB HID Keycodes
// =============================================================================

// Modifier bits
#define MOD_LCTRL   0x01
#define MOD_LSHIFT  0x02
#define MOD_LALT    0x04
#define MOD_LGUI    0x08
#define MOD_RCTRL   0x10
#define MOD_RSHIFT  0x20
#define MOD_RALT    0x40
#define MOD_RGUI    0x80

// Common HID keycodes
#define HID_KEY_NONE        0x00
#define HID_KEY_A           0x04
#define HID_KEY_B           0x05
#define HID_KEY_C           0x06
#define HID_KEY_D           0x07
#define HID_KEY_E           0x08
#define HID_KEY_F           0x09
#define HID_KEY_G           0x0A
#define HID_KEY_H           0x0B
#define HID_KEY_I           0x0C
#define HID_KEY_J           0x0D
#define HID_KEY_K           0x0E
#define HID_KEY_L           0x0F
#define HID_KEY_M           0x10
#define HID_KEY_N           0x11
#define HID_KEY_O           0x12
#define HID_KEY_P           0x13
#define HID_KEY_Q           0x14
#define HID_KEY_R           0x15
#define HID_KEY_S           0x16
#define HID_KEY_T           0x17
#define HID_KEY_U           0x18
#define HID_KEY_V           0x19
#define HID_KEY_W           0x1A
#define HID_KEY_X           0x1B
#define HID_KEY_Y           0x1C
#define HID_KEY_Z           0x1D
#define HID_KEY_1           0x1E
#define HID_KEY_2           0x1F
#define HID_KEY_3           0x20
#define HID_KEY_4           0x21
#define HID_KEY_5           0x22
#define HID_KEY_6           0x23
#define HID_KEY_7           0x24
#define HID_KEY_8           0x25
#define HID_KEY_9           0x26
#define HID_KEY_0           0x27
#define HID_KEY_ENTER       0x28
#define HID_KEY_ESCAPE      0x29
#define HID_KEY_BACKSPACE   0x2A
#define HID_KEY_TAB         0x2B
#define HID_KEY_SPACE       0x2C
#define HID_KEY_MINUS       0x2D
#define HID_KEY_EQUAL       0x2E
#define HID_KEY_LBRACKET    0x2F
#define HID_KEY_RBRACKET    0x30
#define HID_KEY_BACKSLASH   0x31
#define HID_KEY_SEMICOLON   0x33
#define HID_KEY_APOSTROPHE  0x34
#define HID_KEY_GRAVE       0x35
#define HID_KEY_COMMA       0x36
#define HID_KEY_PERIOD      0x37
#define HID_KEY_SLASH       0x38
#define HID_KEY_CAPSLOCK    0x39
#define HID_KEY_F1          0x3A
#define HID_KEY_F2          0x3B
#define HID_KEY_F3          0x3C
#define HID_KEY_F4          0x3D
#define HID_KEY_F5          0x3E
#define HID_KEY_F6          0x3F
#define HID_KEY_F7          0x40
#define HID_KEY_F8          0x41
#define HID_KEY_F9          0x42
#define HID_KEY_F10         0x43
#define HID_KEY_F11         0x44
#define HID_KEY_F12         0x45
#define HID_KEY_PRINTSCREEN 0x46
#define HID_KEY_SCROLLLOCK  0x47
#define HID_KEY_PAUSE       0x48
#define HID_KEY_INSERT      0x49
#define HID_KEY_HOME        0x4A
#define HID_KEY_PAGEUP      0x4B
#define HID_KEY_DELETE      0x4C
#define HID_KEY_END         0x4D
#define HID_KEY_PAGEDOWN    0x4E
#define HID_KEY_RIGHT       0x4F
#define HID_KEY_LEFT        0x50
#define HID_KEY_DOWN        0x51
#define HID_KEY_UP          0x52

// Special Cardputer keys
#define KEY_FN              0xF0    // Function key (internal use)
#define KEY_MODE_SWITCH     0xF1    // Toggle mouse/keyboard mode
#define KEY_BRIGHTNESS_UP   0xF2    // Increase brightness
#define KEY_BRIGHTNESS_DN   0xF3    // Decrease brightness

// =============================================================================
// Input Event
// =============================================================================

typedef struct {
    uint8_t keycode;        // USB HID keycode or special key
    uint8_t modifiers;      // Modifier bits
    bool pressed;           // true = press, false = release
    bool is_special;        // true if special function key
} input_event_t;

// =============================================================================
// Input Context
// =============================================================================

typedef struct {
    // Current state
    input_mode_t mode;
    uint8_t current_modifiers;
    bool fn_pressed;

    // Key state tracking (for release detection)
    uint8_t key_state[KB_ROW_COUNT][KB_COL_COUNT];
    uint8_t prev_state[KB_ROW_COUNT][KB_COL_COUNT];

    // Event queue
    QueueHandle_t event_queue;

    // Debounce
    uint32_t last_scan_time;
    uint32_t debounce_ms;

    // Auto-repeat
    bool auto_repeat_enabled;
    uint32_t repeat_delay_ms;
    uint32_t repeat_interval_ms;
    uint8_t repeat_keycode;
    uint32_t repeat_start_time;
    uint32_t last_repeat_time;

    // Task handle
    TaskHandle_t scan_task;
    bool running;
} input_context_t;

// =============================================================================
// Initialization
// =============================================================================

/**
 * @brief Initialize input module
 * @param ctx Input context
 * @return REMOTE_OK on success
 */
remote_error_t input_init(input_context_t *ctx);

/**
 * @brief Deinitialize input module
 */
void input_deinit(input_context_t *ctx);

// =============================================================================
// Input Processing
// =============================================================================

/**
 * @brief Start keyboard scanning task
 * @param ctx Input context
 * @return REMOTE_OK on success
 */
remote_error_t input_start(input_context_t *ctx);

/**
 * @brief Stop keyboard scanning task
 */
void input_stop(input_context_t *ctx);

/**
 * @brief Get next input event (non-blocking)
 * @param ctx Input context
 * @param[out] event Event data
 * @return true if event available
 */
bool input_get_event(input_context_t *ctx, input_event_t *event);

/**
 * @brief Get next input event (blocking)
 * @param ctx Input context
 * @param[out] event Event data
 * @param timeout_ms Timeout in ms (portMAX_DELAY for infinite)
 * @return true if event available
 */
bool input_wait_event(input_context_t *ctx, input_event_t *event, uint32_t timeout_ms);

// =============================================================================
// Mode Control
// =============================================================================

/**
 * @brief Get current input mode
 */
input_mode_t input_get_mode(const input_context_t *ctx);

/**
 * @brief Set input mode
 */
void input_set_mode(input_context_t *ctx, input_mode_t mode);

/**
 * @brief Toggle between mouse and keyboard mode
 * @return New mode
 */
input_mode_t input_toggle_mode(input_context_t *ctx);

// =============================================================================
// Configuration
// =============================================================================

/**
 * @brief Set debounce time
 * @param ctx Input context
 * @param ms Debounce time in milliseconds
 */
void input_set_debounce(input_context_t *ctx, uint32_t ms);

/**
 * @brief Enable/disable key auto-repeat
 * @param ctx Input context
 * @param enable true to enable
 * @param delay_ms Initial delay before repeat starts
 * @param interval_ms Interval between repeats
 */
void input_set_auto_repeat(input_context_t *ctx, bool enable,
                           uint32_t delay_ms, uint32_t interval_ms);

// =============================================================================
// Utility Functions
// =============================================================================

/**
 * @brief Convert Cardputer key to USB HID keycode
 * @param row Matrix row
 * @param col Matrix column
 * @param fn_pressed FN key state
 * @param[out] modifiers Modifier bits (if key is a modifier)
 * @return HID keycode, or KEY_NONE if not mappable
 */
uint8_t input_matrix_to_hid(int row, int col, bool fn_pressed, uint8_t *modifiers);

/**
 * @brief Check if keycode is an arrow key
 */
bool input_is_arrow_key(uint8_t keycode);

/**
 * @brief Convert arrow key to mouse movement
 * @param keycode Arrow keycode
 * @param[out] dx X movement (-MOUSE_MOVE_STEP to +MOUSE_MOVE_STEP)
 * @param[out] dy Y movement (-MOUSE_MOVE_STEP to +MOUSE_MOVE_STEP)
 */
void input_arrow_to_mouse(uint8_t keycode, int8_t *dx, int8_t *dy);

#ifdef __cplusplus
}
#endif

#endif // REMOTE_INPUT_H

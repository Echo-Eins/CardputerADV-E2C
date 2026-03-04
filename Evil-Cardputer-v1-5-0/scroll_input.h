/**
 * @file scroll_input.h
 * @brief M5Stack Scroll Unit driver - universal input device
 *
 * Reads rotary encoder delta and button state from Scroll Unit (STM32F030)
 * via I2C. Provides two input intents:
 *
 *   Intent 1 - Menu Navigation:
 *     Rotation = up/down selection, Button press = Enter/confirm
 *
 *   Intent 2 - Remote Desktop Mouse Wheel:
 *     Rotation = vertical scroll, Button press = middle mouse click
 *
 * Register map (I2C address 0x40):
 *   0x10  Encoder value (int32, R/W)
 *   0x20  Button status (1 byte, 0=pressed, 1=released)
 *   0x30  RGB LED (4 bytes: index, R, G, B)
 *   0x40  Reset encoder (write 1)
 *   0x50  Incremental encoder (int32, auto-resets on read)
 *   0xFE  Firmware version (1 byte)
 *   0xFF  I2C address (1 byte, R/W)
 */

#ifndef SCROLL_INPUT_H
#define SCROLL_INPUT_H

#include <Arduino.h>

// ============================================================================
// Scroll Unit Registers
// ============================================================================

#define SCROLL_REG_ENCODER      0x10
#define SCROLL_REG_BUTTON       0x20
#define SCROLL_REG_RGB_LED      0x30
#define SCROLL_REG_RESET        0x40
#define SCROLL_REG_INC_ENCODER  0x50
#define SCROLL_REG_BOOTLOADER   0xFC
#define SCROLL_REG_FW_VERSION   0xFE
#define SCROLL_REG_I2C_ADDR     0xFF

// ============================================================================
// Input Events
// ============================================================================

enum class ScrollEvent : uint8_t {
    None = 0,
    ScrollUp,       // Encoder rotated counter-clockwise (negative delta)
    ScrollDown,     // Encoder rotated clockwise (positive delta)
    ButtonPress,    // Button pressed (transition to pressed)
    ButtonRelease,  // Button released (transition to released)
    ButtonClick,    // Full press+release cycle detected
};

struct ScrollInputState {
    int32_t encoderDelta;       // Incremental delta since last poll
    int32_t encoderAbsolute;    // Absolute encoder position
    bool buttonPressed;         // Current button state
    bool buttonChanged;         // Button state changed since last poll
    uint32_t lastPollMs;        // Timestamp of last successful poll
    uint8_t firmwareVersion;    // Cached firmware version
    bool connected;             // Device is responding
};

// ============================================================================
// Scroll Input Driver
// ============================================================================

class ScrollInput {
public:
    // Initialize driver (does NOT start I2C - caller must ensure bus is active)
    static bool init(uint8_t address = 0x40);
    static void shutdown();
    static bool isInitialized();
    static bool isConnected();

    // Polling (call from main loop or before input check)
    static bool poll();

    // State access
    static const ScrollInputState& getState();
    static int32_t getDelta();          // Get and consume encoder delta
    static bool isButtonPressed();
    static bool wasButtonClicked();     // True once per click, auto-resets

    // Menu navigation helper: returns ScrollEvent based on delta + button
    static ScrollEvent getMenuEvent();

    // Remote Desktop helper: returns raw delta for scroll packets
    static int8_t getScrollDelta();     // Clamped to int8_t range

    // Encoder control
    static bool resetEncoder();
    static bool setEncoderValue(int32_t value);

    // LED control
    static bool setLED(uint8_t r, uint8_t g, uint8_t b);
    static bool setLEDOff();

    // Configuration
    static void setAddress(uint8_t addr);
    static uint8_t getAddress();
    static void setPaHubRoute(uint8_t hubAddr, uint8_t channel);
    static void clearPaHubRoute();

private:
    static uint8_t _address;
    static ScrollInputState _state;
    static bool _initialized;
    static bool _buttonClickPending;

    // PaHub routing (if Scroll is behind a PaHub)
    static uint8_t _paHubAddr;
    static uint8_t _paHubChannel;
    static bool _hasPaHubRoute;

    // Internal I2C helpers
    static bool activatePaHubRoute();
    static void deactivatePaHubRoute();
    static int32_t readInt32(uint8_t reg);
    static uint8_t readByte(uint8_t reg);
    static bool writeByte(uint8_t reg, uint8_t value);
    static bool writeBytes(uint8_t reg, const uint8_t* data, uint8_t len);
};

#endif // SCROLL_INPUT_H

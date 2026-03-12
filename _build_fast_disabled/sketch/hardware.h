#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\hardware.h"
/*
 * Hardware Abstraction Layer for Evil-Cardputer
 *
 * This module provides hardware abstraction for:
 * - Display (with video driver abstraction layer)
 * - LED (NeoPixel)
 * - Audio (MP3 playback via M5Speaker)
 * - Battery monitoring
 * - Board detection (Cardputer vs Cardputer-ADV)
 * - GPS UART configuration
 *
 * The display layer is designed to support future video driver backends.
 */

#ifndef HARDWARE_H
#define HARDWARE_H

#include <M5Unified.h>
#include "M5Cardputer.h"
#include <Adafruit_NeoPixel.h>
#include <SD.h>
#include <AudioGeneratorMP3.h>

// Forward declarations for audio (defined in hardware.cpp)
class AudioOutputM5Speaker;

// ============================================================================
// Board Detection
// ============================================================================

enum class BoardType : uint8_t {
    UNKNOWN = 0,
    CARDPUTER = 1,
    CARDPUTER_ADV = 2
};

// Get detected board type
BoardType hwGetBoardType();

// Get board name as string
const char* hwGetBoardName();

// Check if running on ADV variant
bool hwIsCardputerADV();

// ============================================================================
// Display Abstraction Layer (Video Driver Interface)
// ============================================================================

// Display backend types for future extensibility
enum class DisplayBackend : uint8_t {
    M5_UNIFIED = 0,         // Built-in M5 display
    TFT_ESPI_ILI9488 = 1,   // External ILI9488 (TFT_eSPI profile)
    LGFX_ILI9488 = 2,       // External ILI9488 (LovyanGFX profile)
};

// Display driver configuration
struct DisplayConfig {
    DisplayBackend backend;
    uint16_t width;
    uint16_t height;
    uint8_t rotation;
    bool doubleBuffer;
};

// Display driver abstraction class
// This provides a layer that can be swapped for different video backends
class HardwareDisplay {
public:
    // Initialize display subsystem
    static void init();

    // Get current configuration
    static DisplayConfig getConfig();

    // Core drawing primitives (wrappers around current backend)
    static void clear();
    static void fillScreen(uint16_t color);
    static void display();  // Flush to screen

    // Text operations
    static void setCursor(int16_t x, int16_t y);
    static void setTextColor(uint16_t color);
    static void setTextColor(uint16_t fg, uint16_t bg);
    static void setTextSize(float size);
    static void setTextFont(uint8_t font);
    static void print(const char* text);
    static void print(const String& text);
    static void println(const char* text);
    static void println(const String& text);
    static void printf(const char* format, ...);
    static int textWidth(const char* text);
    static int textWidth(const String& text);

    // Graphics primitives
    static void drawPixel(int16_t x, int16_t y, uint16_t color);
    static void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
    static void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    static void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    static void drawCircle(int16_t x, int16_t y, int16_t r, uint16_t color);
    static void fillCircle(int16_t x, int16_t y, int16_t r, uint16_t color);

    // Image operations
    static void drawJpgFile(fs::FS& fs, const char* path, int16_t x = 0, int16_t y = 0);
    static void drawImage(const char* filepath);

    // Brightness control
    static uint8_t getBrightness();
    static void setBrightness(uint8_t brightness);

    // Screen dimensions
    static int16_t width();
    static int16_t height();

    // Rotation
    static void setRotation(uint8_t rotation);
    static uint8_t getRotation();

private:
    static DisplayConfig _config;
    static bool _initialized;
};

// ============================================================================
// LED Control (NeoPixel)
// ============================================================================

// LED pin configuration
#define HW_LED_PIN       21
#define HW_LED_COUNT     1

// LED control class
class HardwareLED {
public:
    // Initialize LED subsystem
    static void init();

    // Set LED color (RGB)
    static void setColor(uint8_t r, uint8_t g, uint8_t b);

    // Set LED color (32-bit packed)
    static void setColor(uint32_t color);

    // Set LED color for a range (for multi-LED setups)
    static void setColorRange(int startPixel, int endPixel, uint32_t color);

    // Turn LED off
    static void off();

    // Blink LED (blocking)
    static void blink(uint8_t r, uint8_t g, uint8_t b, int delayMs = 100);

    // Get/set LED enabled state
    static bool isEnabled();
    static void setEnabled(bool enabled);

    // Update LED (call after color changes)
    static void show();

    // Access to raw NeoPixel object (for advanced usage)
    static Adafruit_NeoPixel& getPixels();

private:
    // Note: pixels is a global variable defined in hardware.cpp for legacy compatibility
    static bool _enabled;
    static bool _initialized;
};

// ============================================================================
// Audio Control (MP3 via M5Speaker)
// ============================================================================

class HardwareAudio {
public:
    // Initialize audio subsystem
    static void init();

    // Play MP3 file from SD card
    static void play(const char* filepath);

    // Stop current playback
    static void stop();

    // Check if audio is playing
    static bool isPlaying();

    // Process audio (call in loop when playing)
    static bool loop();

    // Volume control (0-255)
    static uint8_t getVolume();
    static void setVolume(uint8_t volume);

    // Get/set sound enabled state
    static bool isEnabled();
    static void setEnabled(bool enabled);

private:
    static bool _enabled;
    static bool _initialized;
};

// ============================================================================
// Power/Battery Management
// ============================================================================

class HardwarePower {
public:
    // Get battery level as percentage (0-100), or -1 on error
    static int getBatteryLevel();

    // Get battery level as string (for display)
    static String getBatteryLevelString();

    // Get battery current in mA
    static int getBatteryCurrent();

    // Get temperature from IMU
    static float getTemperature();
    static String getTemperatureString();

    // Get free heap in KB
    static int getFreeHeapKB();
    static String getFreeHeapString();

    // Get stack watermark in KB
    static float getStackWatermarkKB();
    static String getStackWatermarkString();
};

// ============================================================================
// GPS Configuration
// ============================================================================

class HardwareGPS {
public:
    // Initialize GPS UART with detected or configured pins
    static void init(int baudrate = 115200);

    // Get configured pins
    static int getRxPin();
    static int getTxPin();

    // Set pins mode: -1=auto, 0=pins 1/2, 1=pins 15/13
    static void setPinsMode(int mode);
    static int getPinsMode();

    // Access to HardwareSerial
    static HardwareSerial& serial();

private:
    static int _rxPin;
    static int _txPin;
    static int _pinsMode;
    static bool _initialized;
};

// ============================================================================
// Hardware Module Initialization
// ============================================================================

// Initialize all hardware subsystems
// Call this in setup() after M5.begin()
void hwInit();

// Initialize board detection (called early in setup)
void hwDetectBoard();

// ============================================================================
// Legacy Compatibility Wrappers
// ============================================================================

// These functions maintain backward compatibility with existing code
// They simply delegate to the Hardware classes above

extern bool ledOn;      // LED enabled state (for config compatibility)
extern bool soundOn;    // Sound enabled state (for config compatibility)

// Legacy global pixels object (for direct access compatibility)
extern Adafruit_NeoPixel pixels;

// Legacy global mp3 object (for direct access compatibility)
extern AudioGeneratorMP3 mp3;

// Legacy LED functions
void setColorRange(int startPixel, int endPixel, uint32_t color);

// Legacy audio functions
void play(const char* fname);
void stop(void);

// Legacy battery/system functions
String getBatteryLevel();
String getTemperature();
String getStack();
String getRamUsage();

// Legacy display functions
void drawImage(const char* filepath);

#endif // HARDWARE_H

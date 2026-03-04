/**
 * @file display_config.h
 * @brief Display configuration system with JSON persistence
 *
 * Manages display profiles stored in /evil/config/displays.json.
 * Supports internal (built-in ST7789V) and external (ILI9488 SPI) displays.
 * Allows selecting which display receives the rendered UI output.
 *
 * JSON format:
 * {
 *   "active": "internal",
 *   "displays": [
 *     {
 *       "name": "Internal",
 *       "port": "ST7789V",
 *       "interface": "SPI",
 *       "width": 240,
 *       "height": 135,
 *       "rotation": 1,
 *       "builtin": true
 *     },
 *     {
 *       "name": "External TFT",
 *       "port": "ILI9488",
 *       "interface": "SPI",
 *       "width": 480,
 *       "height": 320,
 *       "rotation": 0,
 *       "builtin": false,
 *       "pins": {
 *         "cs": 14,
 *         "dc": 27,
 *         "rst": 33,
 *         "mosi": 35,
 *         "sclk": 36,
 *         "miso": 37
 *       }
 *     }
 *   ]
 * }
 */

#ifndef DISPLAY_CONFIG_H
#define DISPLAY_CONFIG_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD.h>

// ============================================================================
// Constants
// ============================================================================

#define DISPLAY_CONFIG_PATH     "/evil/config/displays.json"
#define DISPLAY_MAX_PROFILES    4
#define DISPLAY_NAME_MAX_LEN    24
#define DISPLAY_PORT_MAX_LEN    16

// ============================================================================
// Display Profile
// ============================================================================

struct DisplayPins {
    int8_t cs;
    int8_t dc;
    int8_t rst;
    int8_t mosi;
    int8_t sclk;
    int8_t miso;
    int8_t bl;      // Backlight (-1 if not used)

    DisplayPins() : cs(-1), dc(-1), rst(-1), mosi(-1), sclk(-1), miso(-1), bl(-1) {}
};

struct DisplayProfile {
    char name[DISPLAY_NAME_MAX_LEN];
    char port[DISPLAY_PORT_MAX_LEN];    // Controller name: "ST7789V", "ILI9488", "SSD1306"
    char interface_type[8];              // "SPI", "I2C", "PAR"
    uint16_t width;
    uint16_t height;
    uint8_t rotation;
    bool builtin;                       // true = internal M5 display
    DisplayPins pins;

    bool isValid() const {
        return (name[0] != '\0' && width > 0 && height > 0);
    }

    String toString() const {
        String s = name;
        s += " (";
        s += port;
        s += " ";
        s += String(width) + "x" + String(height);
        s += ")";
        return s;
    }
};

// ============================================================================
// Display Config Manager
// ============================================================================

class DisplayProfileManager {
public:
    // Initialize and load config from SD
    static bool init();
    static bool isInitialized();

    // Profile management
    static uint8_t getProfileCount();
    static const DisplayProfile* getProfile(uint8_t index);
    static const DisplayProfile* getProfileByName(const char* name);
    static const DisplayProfile* getActiveProfile();
    static int8_t getActiveIndex();

    // Selection
    static bool setActive(uint8_t index);
    static bool setActiveByName(const char* name);
    static const char* getActiveName();

    // Persistence
    static bool save();
    static bool load();
    static bool createDefault();

    // Utility
    static String getDisplayListFormatted();

private:
    static bool _initialized;
    static DisplayProfile _profiles[DISPLAY_MAX_PROFILES];
    static uint8_t _profileCount;
    static int8_t _activeIndex;
    static char _activeName[DISPLAY_NAME_MAX_LEN];

    static void populateInternalProfile(DisplayProfile& p);
    static void populateExternalDefault(DisplayProfile& p);
};

#endif // DISPLAY_CONFIG_H

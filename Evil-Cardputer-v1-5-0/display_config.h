/**
 * @file display_config.h
 * @brief Display profile configuration (JSON + validation)
 *
 * Profiles are stored in /evil/config/displays.json and define runtime
 * display backend selection, SPI bus parameters, initialization order and
 * SD arbitration policy.
 */

#ifndef DISPLAY_CONFIG_H
#define DISPLAY_CONFIG_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD.h>

// ============================================================================
// Constants
// ============================================================================

#define DISPLAY_CONFIG_PATH          "/evil/config/displays.json"
#define DISPLAY_MAX_PROFILES         6
#define DISPLAY_NAME_MAX_LEN         32
#define DISPLAY_ERROR_MAX_LEN        160

// ============================================================================
// Enums / Helpers
// ============================================================================

enum class DisplayDriver : uint8_t {
    M5_BUILTIN = 0,
    TFT_ESPI_ILI9488 = 1,
    LGFX_ILI9488 = 2,
};

enum class DisplaySpiHost : uint8_t {
    AUTO = 0,
    SPI2 = 1,
    SPI3 = 2,
};

enum class DisplayInitOrder : uint8_t {
    ACTIVE_FIRST = 0,
    INTERNAL_FIRST = 1,
    EXTERNAL_FIRST = 2,
};

enum class DisplayCompositorMode : uint8_t {
    DIRECT = 0,
    SCALED_FULL = 1,
    SCALED_TILES = 2,
    AUTO = 3,
};

const char* displayDriverToString(DisplayDriver driver);
DisplayDriver displayDriverFromString(const char* value);

const char* displaySpiHostToString(DisplaySpiHost host);
DisplaySpiHost displaySpiHostFromString(const char* value);

const char* displayInitOrderToString(DisplayInitOrder order);
DisplayInitOrder displayInitOrderFromString(const char* value);
const char* displayCompositorModeToString(DisplayCompositorMode mode);
DisplayCompositorMode displayCompositorModeFromString(const char* value);

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
    int8_t bl;  // Backlight pin (-1 = unused)

    DisplayPins()
        : cs(-1)
        , dc(-1)
        , rst(-1)
        , mosi(-1)
        , sclk(-1)
        , miso(-1)
        , bl(-1) {}
};

struct DisplayProfile {
    char name[DISPLAY_NAME_MAX_LEN];

    DisplayDriver driver;
    uint16_t width;
    uint16_t height;
    uint8_t rotation;
    uint8_t colorDepth;  // 16 or 24
    bool builtin;

    // Low-memory renderer. External defaults use a 240x160 4-bpp logical
    // canvas and scale exactly 2x to the 480x320 ILI9488.
    DisplayCompositorMode compositorMode;
    uint16_t logicalWidth;
    uint16_t logicalHeight;
    uint8_t tileSize;
    uint8_t fullFlushThreshold;

    // SPI bus profile
    DisplaySpiHost spiHost;
    uint8_t spiMode;      // 0..3
    uint32_t freqWrite;
    uint32_t freqRead;
    bool spi3Wire;
    int8_t dmaChannel;    // -1 = auto
    bool busShared;
    bool useLock;

    // Initialization policy
    DisplayInitOrder initOrder;
    uint16_t initDelayMs;

    // SD arbitration policy
    bool sharesBusWithSd;
    bool releaseBeforeSd;

    DisplayPins pins;

    DisplayProfile();

    bool isValidBasic() const;
    String toString() const;
};

// ============================================================================
// Display Config Manager
// ============================================================================

class DisplayProfileManager {
public:
    static bool init();
    static bool isInitialized();

    static uint8_t getProfileCount();
    static const DisplayProfile* getProfile(uint8_t index);
    static const DisplayProfile* getProfileByName(const char* name);
    static const DisplayProfile* getActiveProfile();
    static int8_t getActiveIndex();
    static const char* getActiveName();
    static const char* getLastError();
    static bool isRememberActiveEnabled();

    static bool setActive(uint8_t index, bool persist = true);
    static bool setActiveByName(const char* name, bool persist = true);
    static bool setRememberActiveEnabled(bool enabled, bool persist = true);
    static bool setProfileWriteFrequency(uint8_t index, uint32_t frequencyHz,
                                         bool persist = true);
    static bool setProfileCompositorMode(uint8_t index,
                                         DisplayCompositorMode mode,
                                         bool persist = true);
    static bool setProfileFullFlushThreshold(uint8_t index,
                                             uint8_t thresholdPercent,
                                             bool persist = true);

    static bool save();
    static bool load();
    static bool reload();
    static bool createDefault();
    static bool validate(String* reason = nullptr);

    static String getDisplayListFormatted();

private:
    static bool _initialized;
    static DisplayProfile _profiles[DISPLAY_MAX_PROFILES];
    static uint8_t _profileCount;
    static int8_t _activeIndex;
    static char _activeName[DISPLAY_NAME_MAX_LEN];
    static bool _rememberActive;
    static char _lastError[DISPLAY_ERROR_MAX_LEN];

    static void setError(const String& reason);
    static int8_t findProfileIndexByName(const char* name);
    static bool loadProfileFromJson(DisplayProfile& p, JsonObjectConst d, bool validateOnly, String* reason);
    static void saveProfileToJson(const DisplayProfile& p, JsonObject d);
    static bool validateProfiles(String* reason);
    static bool validateProfile(const DisplayProfile& p, String* reason);
    static bool validatePairConflicts(const DisplayProfile& a, const DisplayProfile& b, String* reason);

    static void populateInternalProfile(DisplayProfile& p);
    static void populateExternalDefault(DisplayProfile& p);
};

#endif  // DISPLAY_CONFIG_H

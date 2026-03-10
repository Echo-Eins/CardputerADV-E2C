#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\display_config.cpp"
/**
 * @file display_config.cpp
 * @brief Display configuration system implementation
 */

#include "display_config.h"

// ============================================================================
// Static member initialization
// ============================================================================

bool DisplayProfileManager::_initialized = false;
DisplayProfile DisplayProfileManager::_profiles[DISPLAY_MAX_PROFILES];
uint8_t DisplayProfileManager::_profileCount = 0;
int8_t DisplayProfileManager::_activeIndex = 0;
char DisplayProfileManager::_activeName[DISPLAY_NAME_MAX_LEN] = "Internal";

// ============================================================================
// Lifecycle
// ============================================================================

bool DisplayProfileManager::init() {
    if (_initialized) return true;

    if (!load()) {
        Serial.println(F("[DisplayProfileManager] No config found, creating default"));
        createDefault();
        save();
    }

    _initialized = true;
    Serial.printf("[DisplayProfileManager] Loaded %d profile(s), active: %s\n",
                  _profileCount, _activeName);
    return true;
}

bool DisplayProfileManager::isInitialized() { return _initialized; }

// ============================================================================
// Profile access
// ============================================================================

uint8_t DisplayProfileManager::getProfileCount() { return _profileCount; }

const DisplayProfile* DisplayProfileManager::getProfile(uint8_t index) {
    if (index >= _profileCount) return nullptr;
    return &_profiles[index];
}

const DisplayProfile* DisplayProfileManager::getProfileByName(const char* name) {
    for (uint8_t i = 0; i < _profileCount; i++) {
        if (strcmp(_profiles[i].name, name) == 0) return &_profiles[i];
    }
    return nullptr;
}

const DisplayProfile* DisplayProfileManager::getActiveProfile() {
    if (_activeIndex >= 0 && _activeIndex < _profileCount) {
        return &_profiles[_activeIndex];
    }
    return (_profileCount > 0) ? &_profiles[0] : nullptr;
}

int8_t DisplayProfileManager::getActiveIndex() { return _activeIndex; }

const char* DisplayProfileManager::getActiveName() { return _activeName; }

// ============================================================================
// Selection
// ============================================================================

bool DisplayProfileManager::setActive(uint8_t index) {
    if (index >= _profileCount) return false;
    _activeIndex = index;
    strncpy(_activeName, _profiles[index].name, DISPLAY_NAME_MAX_LEN - 1);
    _activeName[DISPLAY_NAME_MAX_LEN - 1] = '\0';
    save();
    Serial.printf("[DisplayProfileManager] Active display: %s\n", _activeName);
    return true;
}

bool DisplayProfileManager::setActiveByName(const char* name) {
    for (uint8_t i = 0; i < _profileCount; i++) {
        if (strcmp(_profiles[i].name, name) == 0) {
            return setActive(i);
        }
    }
    return false;
}

// ============================================================================
// Persistence
// ============================================================================

bool DisplayProfileManager::load() {
    if (!SD.exists(DISPLAY_CONFIG_PATH)) return false;

    File f = SD.open(DISPLAY_CONFIG_PATH, FILE_READ);
    if (!f) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("[DisplayProfileManager] JSON parse error: %s\n", err.c_str());
        return false;
    }

    // Read active display name
    const char* activeName = doc["active"] | "Internal";
    strncpy(_activeName, activeName, DISPLAY_NAME_MAX_LEN - 1);
    _activeName[DISPLAY_NAME_MAX_LEN - 1] = '\0';

    // Read display profiles
    JsonArray displays = doc["displays"].as<JsonArray>();
    _profileCount = 0;

    for (JsonObject d : displays) {
        if (_profileCount >= DISPLAY_MAX_PROFILES) break;

        DisplayProfile& p = _profiles[_profileCount];
        memset(&p, 0, sizeof(DisplayProfile));
        p.pins = DisplayPins();

        strncpy(p.name, d["name"] | "Unknown", DISPLAY_NAME_MAX_LEN - 1);
        strncpy(p.port, d["port"] | "Unknown", DISPLAY_PORT_MAX_LEN - 1);
        strncpy(p.interface_type, d["interface"] | "SPI", 7);
        p.width = d["width"] | 0;
        p.height = d["height"] | 0;
        p.rotation = d["rotation"] | 0;
        p.builtin = d["builtin"] | false;

        if (d.containsKey("pins")) {
            JsonObject pins = d["pins"];
            p.pins.cs = pins["cs"] | -1;
            p.pins.dc = pins["dc"] | -1;
            p.pins.rst = pins["rst"] | -1;
            p.pins.mosi = pins["mosi"] | -1;
            p.pins.sclk = pins["sclk"] | -1;
            p.pins.miso = pins["miso"] | -1;
            p.pins.bl = pins["bl"] | -1;
        }

        // Resolve active index
        if (strcmp(p.name, _activeName) == 0) {
            _activeIndex = _profileCount;
        }

        _profileCount++;
    }

    return (_profileCount > 0);
}

bool DisplayProfileManager::save() {
    // Ensure config folder exists
    if (!SD.exists("/evil")) SD.mkdir("/evil");
    if (!SD.exists("/evil/config")) SD.mkdir("/evil/config");

    JsonDocument doc;
    doc["active"] = _activeName;

    JsonArray displays = doc["displays"].to<JsonArray>();

    for (uint8_t i = 0; i < _profileCount; i++) {
        const DisplayProfile& p = _profiles[i];
        JsonObject d = displays.add<JsonObject>();

        d["name"] = p.name;
        d["port"] = p.port;
        d["interface"] = p.interface_type;
        d["width"] = p.width;
        d["height"] = p.height;
        d["rotation"] = p.rotation;
        d["builtin"] = p.builtin;

        if (!p.builtin) {
            JsonObject pins = d["pins"].to<JsonObject>();
            if (p.pins.cs >= 0) pins["cs"] = p.pins.cs;
            if (p.pins.dc >= 0) pins["dc"] = p.pins.dc;
            if (p.pins.rst >= 0) pins["rst"] = p.pins.rst;
            if (p.pins.mosi >= 0) pins["mosi"] = p.pins.mosi;
            if (p.pins.sclk >= 0) pins["sclk"] = p.pins.sclk;
            if (p.pins.miso >= 0) pins["miso"] = p.pins.miso;
            if (p.pins.bl >= 0) pins["bl"] = p.pins.bl;
        }
    }

    File f = SD.open(DISPLAY_CONFIG_PATH, FILE_WRITE);
    if (!f) {
        Serial.println(F("[DisplayProfileManager] Failed to write config"));
        return false;
    }

    serializeJsonPretty(doc, f);
    f.close();
    return true;
}

bool DisplayProfileManager::createDefault() {
    _profileCount = 0;

    // Profile 0: Internal display (Cardputer ADV built-in ST7789V)
    populateInternalProfile(_profiles[0]);
    _profileCount++;

    // Profile 1: External ILI9488 (user's display)
    populateExternalDefault(_profiles[1]);
    _profileCount++;

    _activeIndex = 0;
    strncpy(_activeName, "Internal", DISPLAY_NAME_MAX_LEN - 1);

    return true;
}

// ============================================================================
// Default profiles
// ============================================================================

void DisplayProfileManager::populateInternalProfile(DisplayProfile& p) {
    memset(&p, 0, sizeof(DisplayProfile));
    p.pins = DisplayPins();
    strncpy(p.name, "Internal", DISPLAY_NAME_MAX_LEN - 1);
    strncpy(p.port, "ST7789V", DISPLAY_PORT_MAX_LEN - 1);
    strncpy(p.interface_type, "SPI", 7);
    p.width = 240;
    p.height = 135;
    p.rotation = 1;
    p.builtin = true;
}

void DisplayProfileManager::populateExternalDefault(DisplayProfile& p) {
    memset(&p, 0, sizeof(DisplayProfile));
    p.pins = DisplayPins();
    strncpy(p.name, "External TFT", DISPLAY_NAME_MAX_LEN - 1);
    strncpy(p.port, "ILI9488", DISPLAY_PORT_MAX_LEN - 1);
    strncpy(p.interface_type, "SPI", 7);
    p.width = 480;
    p.height = 320;
    p.rotation = 0;
    p.builtin = false;
    // Pins left as -1 (unassigned) - user configures in JSON
}

// ============================================================================
// Utility
// ============================================================================

String DisplayProfileManager::getDisplayListFormatted() {
    String result;
    if (_profileCount == 0) {
        result = "No displays configured";
        return result;
    }

    for (uint8_t i = 0; i < _profileCount; i++) {
        if (i == _activeIndex) {
            result += "> ";
        } else {
            result += "  ";
        }
        result += _profiles[i].toString() + "\n";
    }
    return result;
}

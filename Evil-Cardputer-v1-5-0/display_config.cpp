/**
 * @file display_config.cpp
 * @brief Display profile configuration with validation.
 */

#include "display_config.h"
#include "display_runtime.h"
#include <driver/spi_master.h>
#include <cstring>
#include <algorithm>

namespace {

constexpr spi_host_device_t kDefaultSdHost = SPI2_HOST;
constexpr int8_t kDefaultSdCs = 12;
constexpr int8_t kDefaultSdSclk = static_cast<int8_t>(SCK);
constexpr int8_t kDefaultSdMosi = static_cast<int8_t>(MOSI);
constexpr int8_t kDefaultSdMiso = static_cast<int8_t>(MISO);

bool strEq(const char* a, const char* b) {
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

bool validPin(int8_t pin) {
    return pin >= 0;
}

bool pinConflict(int8_t a, int8_t b) {
    return validPin(a) && validPin(b) && a == b;
}

bool pinSetMismatch(int8_t a, int8_t b) {
    return validPin(a) && validPin(b) && a != b;
}

spi_host_device_t resolveSpiHost(DisplaySpiHost host) {
    switch (host) {
        case DisplaySpiHost::SPI2: return SPI2_HOST;
        case DisplaySpiHost::SPI3: return SPI3_HOST;
        case DisplaySpiHost::AUTO:
        default:
            return SPI2_HOST;
    }
}

bool hasJsonKey(JsonObjectConst obj, const char* key) {
    JsonVariantConst v = obj[key];
    return !v.isNull();
}

bool profileTouchesSdPins(const DisplayProfile& p) {
    return pinConflict(p.pins.cs, kDefaultSdCs) ||
           pinConflict(p.pins.sclk, kDefaultSdSclk) ||
           pinConflict(p.pins.mosi, kDefaultSdMosi) ||
           pinConflict(p.pins.miso, kDefaultSdMiso);
}

bool inferSharesBusWithSd(const DisplayProfile& p) {
    if (p.builtin) {
        return false;
    }
    const spi_host_device_t host = resolveSpiHost(p.spiHost);
    return host == kDefaultSdHost || profileTouchesSdPins(p);
}

}  // namespace

// ============================================================================
// Enum helpers
// ============================================================================

const char* displayDriverToString(DisplayDriver driver) {
    switch (driver) {
        case DisplayDriver::M5_BUILTIN: return "m5_builtin";
        case DisplayDriver::TFT_ESPI_ILI9488: return "tft_espi_ili9488";
        case DisplayDriver::LGFX_ILI9488: return "lgfx_ili9488";
        default: return "m5_builtin";
    }
}

DisplayDriver displayDriverFromString(const char* value) {
    if (!value) return DisplayDriver::M5_BUILTIN;
    if (strEq(value, "m5_builtin")) return DisplayDriver::M5_BUILTIN;
    if (strEq(value, "tft_espi_ili9488")) return DisplayDriver::TFT_ESPI_ILI9488;
    if (strEq(value, "lgfx_ili9488")) return DisplayDriver::LGFX_ILI9488;
    return DisplayDriver::M5_BUILTIN;
}

const char* displaySpiHostToString(DisplaySpiHost host) {
    switch (host) {
        case DisplaySpiHost::SPI2: return "SPI2_HOST";
        case DisplaySpiHost::SPI3: return "SPI3_HOST";
        case DisplaySpiHost::AUTO:
        default:
            return "AUTO";
    }
}

DisplaySpiHost displaySpiHostFromString(const char* value) {
    if (!value) return DisplaySpiHost::AUTO;
    if (strEq(value, "SPI2_HOST") || strEq(value, "FSPI_HOST")) return DisplaySpiHost::SPI2;
    if (strEq(value, "SPI3_HOST") || strEq(value, "HSPI_HOST")) return DisplaySpiHost::SPI3;
    return DisplaySpiHost::AUTO;
}

const char* displayInitOrderToString(DisplayInitOrder order) {
    switch (order) {
        case DisplayInitOrder::INTERNAL_FIRST: return "internal_first";
        case DisplayInitOrder::EXTERNAL_FIRST: return "external_first";
        case DisplayInitOrder::ACTIVE_FIRST:
        default:
            return "active_first";
    }
}

DisplayInitOrder displayInitOrderFromString(const char* value) {
    if (!value) return DisplayInitOrder::ACTIVE_FIRST;
    if (strEq(value, "internal_first")) return DisplayInitOrder::INTERNAL_FIRST;
    if (strEq(value, "external_first")) return DisplayInitOrder::EXTERNAL_FIRST;
    return DisplayInitOrder::ACTIVE_FIRST;
}

const char* displayCompositorModeToString(DisplayCompositorMode mode) {
    switch (mode) {
        case DisplayCompositorMode::DIRECT: return "direct";
        case DisplayCompositorMode::SCALED_FULL: return "scaled_full";
        case DisplayCompositorMode::SCALED_TILES: return "scaled_tiles";
        case DisplayCompositorMode::AUTO: return "auto";
        default: return "direct";
    }
}

DisplayCompositorMode displayCompositorModeFromString(const char* value) {
    if (!value) return DisplayCompositorMode::DIRECT;
    if (strEq(value, "scaled_full"))
        return DisplayCompositorMode::SCALED_FULL;
    if (strEq(value, "scaled_tiles"))
        return DisplayCompositorMode::SCALED_TILES;
    if (strEq(value, "auto")) return DisplayCompositorMode::AUTO;
    return DisplayCompositorMode::DIRECT;
}

// ============================================================================
// DisplayProfile
// ============================================================================

DisplayProfile::DisplayProfile()
    : driver(DisplayDriver::M5_BUILTIN)
    , width(240)
    , height(135)
    , rotation(1)
    , colorDepth(16)
    , builtin(true)
    , compositorMode(DisplayCompositorMode::DIRECT)
    , logicalWidth(240)
    , logicalHeight(135)
    , tileSize(16)
    , fullFlushThreshold(38)
    , spiHost(DisplaySpiHost::SPI2)
    , spiMode(0)
    , freqWrite(40000000UL)
    , freqRead(16000000UL)
    , spi3Wire(false)
    , dmaChannel(0)
    , busShared(false)
    , useLock(true)
    , initOrder(DisplayInitOrder::ACTIVE_FIRST)
    , initDelayMs(120)
    , sharesBusWithSd(false)
    , releaseBeforeSd(false) {
    name[0] = '\0';
}

bool DisplayProfile::isValidBasic() const {
    return name[0] != '\0' && width > 0 && height > 0;
}

String DisplayProfile::toString() const {
    String s(name);
    s += " [";
    s += displayDriverToString(driver);
    s += ", ";
    s += String(width) + "x" + String(height);
    s += ", ";
    s += displaySpiHostToString(spiHost);
    s += ", renderer=";
    s += displayCompositorModeToString(compositorMode);
    s += "]";
    return s;
}

// ============================================================================
// Static members
// ============================================================================

bool DisplayProfileManager::_initialized = false;
DisplayProfile DisplayProfileManager::_profiles[DISPLAY_MAX_PROFILES];
uint8_t DisplayProfileManager::_profileCount = 0;
int8_t DisplayProfileManager::_activeIndex = 0;
char DisplayProfileManager::_activeName[DISPLAY_NAME_MAX_LEN] = "internal_st7789";
bool DisplayProfileManager::_rememberActive = false;
char DisplayProfileManager::_lastError[DISPLAY_ERROR_MAX_LEN] = "";

// ============================================================================
// Helpers
// ============================================================================

void DisplayProfileManager::setError(const String& reason) {
    strncpy(_lastError, reason.c_str(), DISPLAY_ERROR_MAX_LEN - 1);
    _lastError[DISPLAY_ERROR_MAX_LEN - 1] = '\0';
    Serial.printf("[DisplayProfileManager] %s\n", _lastError);
}

int8_t DisplayProfileManager::findProfileIndexByName(const char* name) {
    if (!name) return -1;
    for (uint8_t i = 0; i < _profileCount; i++) {
        if (strcmp(_profiles[i].name, name) == 0) {
            return static_cast<int8_t>(i);
        }
    }
    return -1;
}

bool DisplayProfileManager::loadProfileFromJson(DisplayProfile& p,
                                                JsonObjectConst d,
                                                bool validateOnly,
                                                String* reason) {
    (void)validateOnly;
    p = DisplayProfile();

    const char* name = d["name"] | "";
    strncpy(p.name, name, DISPLAY_NAME_MAX_LEN - 1);
    p.name[DISPLAY_NAME_MAX_LEN - 1] = '\0';

    p.driver = displayDriverFromString(d["driver"] | (d["builtin"] | false ? "m5_builtin" : "lgfx_ili9488"));
    p.width = d["width"] | 0;
    p.height = d["height"] | 0;
    p.rotation = d["rotation"] | 0;
    p.colorDepth = d["color_depth"] | 16;
    p.builtin = d["builtin"] | (p.driver == DisplayDriver::M5_BUILTIN);

    p.compositorMode = displayCompositorModeFromString(
        d["renderer_mode"] | (p.builtin ? "direct" : "auto"));
    p.logicalWidth = d["logical_width"] | 240;
    p.logicalHeight = d["logical_height"] | (p.builtin ? p.height : 160);
    p.tileSize = d["tile_size"] | 16;
    p.fullFlushThreshold = d["full_flush_threshold"] | 38;

    p.spiHost = displaySpiHostFromString(d["spi_host"] | (p.builtin ? "SPI2_HOST" : "SPI3_HOST"));
    p.spiMode = d["spi_mode"] | 0;
    p.freqWrite = d["freq_write"] | 40000000UL;
    p.freqRead = d["freq_read"] | 16000000UL;
    p.spi3Wire = d["spi_3wire"] | (!p.builtin);
    p.dmaChannel = d["dma_channel"] | 0;
    p.busShared = d["bus_shared"] | false;
    p.useLock = d["use_lock"] | true;

    p.initOrder = displayInitOrderFromString(d["init_order"] | "active_first");
    p.initDelayMs = d["init_delay_ms"] | 120;

    const bool hasSharesBusKey = hasJsonKey(d, "shares_bus_with_sd");
    const bool hasReleaseBeforeSdKey = hasJsonKey(d, "release_before_sd");

    JsonObjectConst pins = d["pins"].as<JsonObjectConst>();
    if (!pins.isNull()) {
        p.pins.cs = pins["cs"] | -1;
        p.pins.dc = pins["dc"] | -1;
        p.pins.rst = pins["rst"] | -1;
        p.pins.mosi = pins["mosi"] | -1;
        p.pins.sclk = pins["sclk"] | -1;
        p.pins.miso = pins["miso"] | -1;
        p.pins.bl = pins["bl"] | -1;
    }

    const bool inferredSharesBus = inferSharesBusWithSd(p);
    p.sharesBusWithSd = hasSharesBusKey ? static_cast<bool>(d["shares_bus_with_sd"] | false)
                                        : inferredSharesBus;
    p.releaseBeforeSd = hasReleaseBeforeSdKey
                            ? static_cast<bool>(d["release_before_sd"] | false)
                            : p.sharesBusWithSd;

    if (!validateProfile(p, reason)) {
        return false;
    }
    return true;
}

void DisplayProfileManager::saveProfileToJson(const DisplayProfile& p, JsonObject d) {
    d["name"] = p.name;
    d["driver"] = displayDriverToString(p.driver);
    d["width"] = p.width;
    d["height"] = p.height;
    d["rotation"] = p.rotation;
    d["color_depth"] = p.colorDepth;
    d["builtin"] = p.builtin;
    d["renderer_mode"] = displayCompositorModeToString(p.compositorMode);
    d["logical_width"] = p.logicalWidth;
    d["logical_height"] = p.logicalHeight;
    d["tile_size"] = p.tileSize;
    d["full_flush_threshold"] = p.fullFlushThreshold;

    d["spi_host"] = displaySpiHostToString(p.spiHost);
    d["spi_mode"] = p.spiMode;
    d["freq_write"] = p.freqWrite;
    d["freq_read"] = p.freqRead;
    d["spi_3wire"] = p.spi3Wire;
    d["dma_channel"] = p.dmaChannel;
    d["bus_shared"] = p.busShared;
    d["use_lock"] = p.useLock;

    d["init_order"] = displayInitOrderToString(p.initOrder);
    d["init_delay_ms"] = p.initDelayMs;

    d["shares_bus_with_sd"] = p.sharesBusWithSd;
    d["release_before_sd"] = p.releaseBeforeSd;

    JsonObject pins = d["pins"].to<JsonObject>();
    pins["cs"] = p.pins.cs;
    pins["dc"] = p.pins.dc;
    pins["rst"] = p.pins.rst;
    pins["mosi"] = p.pins.mosi;
    pins["sclk"] = p.pins.sclk;
    pins["miso"] = p.pins.miso;
    pins["bl"] = p.pins.bl;
}

bool DisplayProfileManager::validateProfile(const DisplayProfile& p, String* reason) {
    if (!p.isValidBasic()) {
        if (reason) *reason = "invalid name/size";
        return false;
    }

    if (p.colorDepth != 16 && p.colorDepth != 24) {
        if (reason) *reason = "color_depth must be 16 or 24";
        return false;
    }

    if (p.spiMode > 3) {
        if (reason) *reason = "spi_mode must be 0..3";
        return false;
    }

    if (p.rotation > 7) {
        if (reason) *reason = "rotation must be 0..7";
        return false;
    }

    if (p.logicalWidth < 64 || p.logicalWidth > 480 ||
        p.logicalHeight < 64 || p.logicalHeight > 320) {
        if (reason) *reason = "logical display size is out of range";
        return false;
    }
    if (p.tileSize != 8 && p.tileSize != 16 && p.tileSize != 32) {
        if (reason) *reason = "tile_size must be 8, 16 or 32";
        return false;
    }
    if (p.fullFlushThreshold < 1 || p.fullFlushThreshold > 100) {
        if (reason) *reason = "full_flush_threshold must be 1..100";
        return false;
    }

    if (p.builtin && p.driver != DisplayDriver::M5_BUILTIN) {
        if (reason) *reason = "builtin profile must use driver=m5_builtin";
        return false;
    }

    if (!p.builtin && p.driver == DisplayDriver::M5_BUILTIN) {
        if (reason) *reason = "external profile cannot use driver=m5_builtin";
        return false;
    }

    if (!p.builtin) {
        if (p.freqWrite == 0) {
            if (reason) *reason = "freq_write must be > 0";
            return false;
        }
        if (!validPin(p.pins.cs) || !validPin(p.pins.dc) ||
            !validPin(p.pins.mosi) || !validPin(p.pins.sclk)) {
            if (reason) *reason = "external profile requires pins: cs/dc/mosi/sclk";
            return false;
        }
        if (p.sharesBusWithSd && !p.busShared) {
            if (reason) *reason = "shares_bus_with_sd requires bus_shared=true";
            return false;
        }
        if (p.releaseBeforeSd && !p.sharesBusWithSd) {
            if (reason) *reason = "release_before_sd requires shares_bus_with_sd=true";
            return false;
        }
    }

    return true;
}

bool DisplayProfileManager::validatePairConflicts(const DisplayProfile& a,
                                                  const DisplayProfile& b,
                                                  String* reason) {
    const spi_host_device_t hostA = resolveSpiHost(a.spiHost);
    const spi_host_device_t hostB = resolveSpiHost(b.spiHost);
    const bool sameHost = hostA == hostB;
    const bool hasBuiltin = a.builtin || b.builtin;

    // CS collision on same SPI host is always invalid.
    if (sameHost && pinConflict(a.pins.cs, b.pins.cs)) {
        if (reason) *reason = String("CS conflict on same host between '") + a.name + "' and '" + b.name + "'";
        return false;
    }

    // Same host with different wire routing is undefined for deterministic runtime switching.
    if (sameHost &&
        (pinSetMismatch(a.pins.sclk, b.pins.sclk) ||
         pinSetMismatch(a.pins.mosi, b.pins.mosi) ||
         pinSetMismatch(a.pins.miso, b.pins.miso))) {
        if (reason) *reason = String("pin mismatch on shared host between '") + a.name + "' and '" + b.name + "'";
        return false;
    }

    // If external profile shares host with built-in, it must be declared as shared bus.
    if (sameHost && hasBuiltin) {
        const DisplayProfile& external = a.builtin ? b : a;
        if (!external.builtin && !external.busShared) {
            if (reason) *reason = String("profile '") + external.name + "': same host as built-in requires bus_shared=true";
            return false;
        }
    }

    // Two external profiles on the same SPI host must opt into shared-bus mode.
    if (sameHost && !a.builtin && !b.builtin && (!a.busShared || !b.busShared)) {
        if (reason) *reason = String("shared host between '") + a.name + "' and '" + b.name + "' requires bus_shared=true for both";
        return false;
    }

    // Physical pin reuse for control lines is not allowed.
    if (pinConflict(a.pins.dc, b.pins.dc) ||
        pinConflict(a.pins.rst, b.pins.rst) ||
        pinConflict(a.pins.bl, b.pins.bl)) {
        if (reason) *reason = String("control pin conflict between '") + a.name + "' and '" + b.name + "'";
        return false;
    }

    return true;
}

bool DisplayProfileManager::validateProfiles(String* reason) {
    if (_profileCount == 0) {
        if (reason) *reason = "no profiles configured";
        return false;
    }

    bool hasBuiltin = false;
    for (uint8_t i = 0; i < _profileCount; i++) {
        const DisplayProfile& p = _profiles[i];
        String profileReason;
        if (!validateProfile(p, &profileReason)) {
            if (reason) *reason = String("profile '") + p.name + "': " + profileReason;
            return false;
        }
        hasBuiltin = hasBuiltin || p.builtin;

        // Unique names
        for (uint8_t j = static_cast<uint8_t>(i + 1); j < _profileCount; j++) {
            if (strcmp(p.name, _profiles[j].name) == 0) {
                if (reason) *reason = String("duplicate profile name: ") + p.name;
                return false;
            }
        }
    }

    if (!hasBuiltin) {
        if (reason) *reason = "at least one builtin profile is required";
        return false;
    }

    for (uint8_t i = 0; i < _profileCount; i++) {
        for (uint8_t j = static_cast<uint8_t>(i + 1); j < _profileCount; j++) {
            String pairReason;
            if (!validatePairConflicts(_profiles[i], _profiles[j], &pairReason)) {
                if (reason) *reason = pairReason;
                return false;
            }
        }
    }

    // Active profile must exist.
    int8_t resolvedActive = findProfileIndexByName(_activeName);
    if (resolvedActive < 0) {
        resolvedActive = 0;
        strncpy(_activeName, _profiles[0].name, DISPLAY_NAME_MAX_LEN - 1);
        _activeName[DISPLAY_NAME_MAX_LEN - 1] = '\0';
    }
    _activeIndex = resolvedActive;

    // Validate SD arbitration rules.
    for (uint8_t i = 0; i < _profileCount; i++) {
        const DisplayProfile& p = _profiles[i];
        if (p.builtin) continue;

        const spi_host_device_t host = resolveSpiHost(p.spiHost);
        const bool touchesSdPins =
            pinConflict(p.pins.cs, kDefaultSdCs) ||
            pinConflict(p.pins.sclk, kDefaultSdSclk) ||
            pinConflict(p.pins.mosi, kDefaultSdMosi) ||
            pinConflict(p.pins.miso, kDefaultSdMiso);
        if (p.sharesBusWithSd) {
            // Shared with SD is valid either by host or by physical pin overlap.
            // Cardputer-Adv external setups often use SPI3_HOST while reusing SD lines.
            if (!touchesSdPins && host != kDefaultSdHost) {
                if (reason) *reason = String("profile '") + p.name + "': shares_bus_with_sd=true but no shared host/pins with SD";
                return false;
            }
            if (pinConflict(p.pins.cs, kDefaultSdCs)) {
                if (reason) *reason = String("profile '") + p.name + "': CS conflicts with SD CS";
                return false;
            }
            if (touchesSdPins) {
                if (pinSetMismatch(p.pins.sclk, kDefaultSdSclk) ||
                    pinSetMismatch(p.pins.mosi, kDefaultSdMosi) ||
                    (!p.spi3Wire && pinSetMismatch(p.pins.miso, kDefaultSdMiso))) {
                    if (reason) *reason = String("profile '") + p.name + "': SD shared-bus pins mismatch";
                    return false;
                }
            }
        } else {
            if (touchesSdPins) {
                if (reason) *reason = String("profile '") + p.name + "': SD pins reused without shares_bus_with_sd=true";
                return false;
            }
            if (host == kDefaultSdHost) {
                if (reason) *reason = String("profile '") + p.name + "': host conflicts with SD, set shares_bus_with_sd=true or move to SPI3_HOST";
                return false;
            }
        }
    }

    return true;
}

// ============================================================================
// Lifecycle
// ============================================================================

bool DisplayProfileManager::init() {
    if (_initialized) return true;

    if (!load()) {
        Serial.println(F("[DisplayProfileManager] displays.json missing/invalid, creating default"));
        createDefault();
        save();
    }

    // Safe default remains the built-in display. A user must explicitly
    // enable remember_active before an external profile may become a boot
    // target.
    if (!_rememberActive) {
        for (uint8_t i = 0; i < _profileCount; ++i) {
            if (_profiles[i].builtin) {
                _activeIndex = static_cast<int8_t>(i);
                strncpy(_activeName, _profiles[i].name, DISPLAY_NAME_MAX_LEN - 1);
                _activeName[DISPLAY_NAME_MAX_LEN - 1] = '\0';
                break;
            }
        }
    }

    _initialized = true;
    Serial.printf("[DisplayProfileManager] Loaded %u profile(s), active=%s\n",
                  static_cast<unsigned>(_profileCount), _activeName);
    return true;
}

bool DisplayProfileManager::isInitialized() {
    return _initialized;
}

// ============================================================================
// Access
// ============================================================================

uint8_t DisplayProfileManager::getProfileCount() {
    return _profileCount;
}

const DisplayProfile* DisplayProfileManager::getProfile(uint8_t index) {
    if (index >= _profileCount) return nullptr;
    return &_profiles[index];
}

const DisplayProfile* DisplayProfileManager::getProfileByName(const char* name) {
    int8_t idx = findProfileIndexByName(name);
    if (idx < 0) return nullptr;
    return &_profiles[idx];
}

const DisplayProfile* DisplayProfileManager::getActiveProfile() {
    if (_activeIndex >= 0 && _activeIndex < _profileCount) {
        return &_profiles[_activeIndex];
    }
    return (_profileCount > 0) ? &_profiles[0] : nullptr;
}

int8_t DisplayProfileManager::getActiveIndex() {
    return _activeIndex;
}

const char* DisplayProfileManager::getActiveName() {
    return _activeName;
}

const char* DisplayProfileManager::getLastError() {
    return _lastError;
}

bool DisplayProfileManager::isRememberActiveEnabled() {
    return _rememberActive;
}

// ============================================================================
// Selection
// ============================================================================

bool DisplayProfileManager::setActive(uint8_t index, bool persist) {
    if (index >= _profileCount) {
        setError(String("setActive: invalid index ") + index);
        return false;
    }

    _activeIndex = static_cast<int8_t>(index);
    strncpy(_activeName, _profiles[index].name, DISPLAY_NAME_MAX_LEN - 1);
    _activeName[DISPLAY_NAME_MAX_LEN - 1] = '\0';

    if (persist && !save()) {
        return false;
    }
    return true;
}

bool DisplayProfileManager::setActiveByName(const char* name, bool persist) {
    int8_t idx = findProfileIndexByName(name);
    if (idx < 0) {
        setError(String("setActiveByName: profile not found: ") + (name ? name : "<null>"));
        return false;
    }
    return setActive(static_cast<uint8_t>(idx), persist);
}

bool DisplayProfileManager::setRememberActiveEnabled(bool enabled, bool persist) {
    const bool previous = _rememberActive;
    _rememberActive = enabled;
    if (persist && !save()) {
        _rememberActive = previous;
        return false;
    }
    return true;
}

bool DisplayProfileManager::setProfileWriteFrequency(uint8_t index,
                                                     uint32_t frequencyHz,
                                                     bool persist) {
    if (index >= _profileCount) {
        setError(String("set frequency: invalid profile ") + index);
        return false;
    }
    if (frequencyHz < 1000000UL || frequencyHz > 80000000UL) {
        setError("display frequency must be between 1 and 80 MHz");
        return false;
    }

    const uint32_t previous = _profiles[index].freqWrite;
    _profiles[index].freqWrite = frequencyHz;
    String reason;
    if (!validateProfiles(&reason)) {
        _profiles[index].freqWrite = previous;
        setError(String("frequency rejected: ") + reason);
        return false;
    }
    if (persist && !save()) {
        _profiles[index].freqWrite = previous;
        return false;
    }
    return true;
}

bool DisplayProfileManager::setProfileCompositorMode(
    uint8_t index, DisplayCompositorMode mode, bool persist) {
    if (index >= _profileCount) {
        setError(String("set renderer: invalid profile ") + index);
        return false;
    }
    const DisplayCompositorMode previous = _profiles[index].compositorMode;
    _profiles[index].compositorMode = mode;
    String reason;
    if (!validateProfiles(&reason)) {
        _profiles[index].compositorMode = previous;
        setError(String("renderer rejected: ") + reason);
        return false;
    }
    if (persist && !save()) {
        _profiles[index].compositorMode = previous;
        return false;
    }
    return true;
}

bool DisplayProfileManager::setProfileFullFlushThreshold(
    uint8_t index, uint8_t thresholdPercent, bool persist) {
    if (index >= _profileCount || thresholdPercent < 1 ||
        thresholdPercent > 100) {
        setError("invalid full-flush threshold");
        return false;
    }
    const uint8_t previous = _profiles[index].fullFlushThreshold;
    _profiles[index].fullFlushThreshold = thresholdPercent;
    if (persist && !save()) {
        _profiles[index].fullFlushThreshold = previous;
        return false;
    }
    return true;
}

// ============================================================================
// Persistence
// ============================================================================

bool DisplayProfileManager::load() {
    DisplayRuntime::ScopedSdDisplayRelease sdGuard;
    _lastError[0] = '\0';
    if (!SD.exists(DISPLAY_CONFIG_PATH)) {
        setError("displays.json not found");
        return false;
    }

    File f = SD.open(DISPLAY_CONFIG_PATH, FILE_READ);
    if (!f) {
        setError("failed to open displays.json");
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        setError(String("json parse error: ") + err.c_str());
        return false;
    }

    _rememberActive = doc["remember_active"] | false;
    const char* active = doc["active"] | "";
    strncpy(_activeName, active, DISPLAY_NAME_MAX_LEN - 1);
    _activeName[DISPLAY_NAME_MAX_LEN - 1] = '\0';

    JsonArrayConst displays = doc["displays"].as<JsonArrayConst>();
    if (displays.isNull()) {
        setError("missing displays[]");
        return false;
    }

    _profileCount = 0;
    for (JsonObjectConst d : displays) {
        if (_profileCount >= DISPLAY_MAX_PROFILES) break;
        String reason;
        if (!loadProfileFromJson(_profiles[_profileCount], d, false, &reason)) {
            setError(String("profile parse failed: ") + reason);
            return false;
        }
        _profileCount++;
    }

    String reason;
    if (!validateProfiles(&reason)) {
        setError(String("validation failed: ") + reason);
        return false;
    }

    return true;
}

bool DisplayProfileManager::reload() {
    DisplayProfile previousProfiles[DISPLAY_MAX_PROFILES];
    for (uint8_t i = 0; i < DISPLAY_MAX_PROFILES; ++i) {
        previousProfiles[i] = _profiles[i];
    }
    const uint8_t previousCount = _profileCount;
    const int8_t previousActiveIndex = _activeIndex;
    char previousActiveName[DISPLAY_NAME_MAX_LEN];
    strncpy(previousActiveName, _activeName, DISPLAY_NAME_MAX_LEN);
    previousActiveName[DISPLAY_NAME_MAX_LEN - 1] = '\0';
    const bool previousRemember = _rememberActive;

    if (load()) {
        _initialized = true;
        Serial.printf("[DisplayProfileManager] Reloaded %u profile(s), active=%s\n",
                      static_cast<unsigned>(_profileCount), _activeName);
        return true;
    }

    const String reloadError = _lastError;
    for (uint8_t i = 0; i < DISPLAY_MAX_PROFILES; ++i) {
        _profiles[i] = previousProfiles[i];
    }
    _profileCount = previousCount;
    _activeIndex = previousActiveIndex;
    strncpy(_activeName, previousActiveName, DISPLAY_NAME_MAX_LEN);
    _activeName[DISPLAY_NAME_MAX_LEN - 1] = '\0';
    _rememberActive = previousRemember;
    setError(reloadError);
    return false;
}

bool DisplayProfileManager::save() {
    DisplayRuntime::ScopedSdDisplayRelease sdGuard;
    _lastError[0] = '\0';

    String reason;
    if (!validateProfiles(&reason)) {
        setError(String("save blocked: ") + reason);
        return false;
    }

    if (!SD.exists("/evil")) SD.mkdir("/evil");
    if (!SD.exists("/evil/config")) SD.mkdir("/evil/config");

    JsonDocument doc;
    doc["remember_active"] = _rememberActive;

    // With remembering disabled, persist the built-in recovery profile. With
    // it enabled, persist the profile currently active in this session.
    const char* persistedActive = _activeName;
    if (!_rememberActive) {
        for (uint8_t i = 0; i < _profileCount; ++i) {
            if (_profiles[i].builtin) {
                persistedActive = _profiles[i].name;
                break;
            }
        }
    }
    doc["active"] = persistedActive;

    JsonArray displays = doc["displays"].to<JsonArray>();
    for (uint8_t i = 0; i < _profileCount; i++) {
        JsonObject d = displays.add<JsonObject>();
        saveProfileToJson(_profiles[i], d);
    }

    const String tempPath = String(DISPLAY_CONFIG_PATH) + ".tmp";
    const String backupPath = String(DISPLAY_CONFIG_PATH) + ".bak";
    if (SD.exists(tempPath.c_str()) && !SD.remove(tempPath.c_str())) {
        setError("failed to remove stale displays.json.tmp");
        return false;
    }

    File f = SD.open(tempPath.c_str(), FILE_WRITE);
    if (!f) {
        setError("failed to open displays.json.tmp for write");
        return false;
    }

    if (serializeJsonPretty(doc, f) == 0) {
        f.close();
        SD.remove(tempPath.c_str());
        setError("failed to write displays.json.tmp");
        return false;
    }
    f.flush();
    f.close();

    const bool hadOriginal = SD.exists(DISPLAY_CONFIG_PATH);
    if (hadOriginal) {
        if (SD.exists(backupPath.c_str()) &&
            !SD.remove(backupPath.c_str())) {
            SD.remove(tempPath.c_str());
            setError("failed to rotate displays.json backup");
            return false;
        }
        if (!SD.rename(DISPLAY_CONFIG_PATH, backupPath.c_str())) {
            SD.remove(tempPath.c_str());
            setError("failed to back up displays.json");
            return false;
        }
    }

    if (!SD.rename(tempPath.c_str(), DISPLAY_CONFIG_PATH)) {
        if (hadOriginal) {
            SD.rename(backupPath.c_str(), DISPLAY_CONFIG_PATH);
        }
        SD.remove(tempPath.c_str());
        setError("failed to install displays.json");
        return false;
    }
    return true;
}

bool DisplayProfileManager::createDefault() {
    _profileCount = 0;
    populateInternalProfile(_profiles[_profileCount++]);
    populateExternalDefault(_profiles[_profileCount++]);

    _rememberActive = false;
    _activeIndex = 0;
    strncpy(_activeName, _profiles[0].name, DISPLAY_NAME_MAX_LEN - 1);
    _activeName[DISPLAY_NAME_MAX_LEN - 1] = '\0';

    String reason;
    if (!validateProfiles(&reason)) {
        setError(String("default profile invalid: ") + reason);
        return false;
    }
    return true;
}

bool DisplayProfileManager::validate(String* reason) {
    return validateProfiles(reason);
}

// ============================================================================
// Defaults
// ============================================================================

void DisplayProfileManager::populateInternalProfile(DisplayProfile& p) {
    p = DisplayProfile();
    strncpy(p.name, "internal_st7789", DISPLAY_NAME_MAX_LEN - 1);
    p.driver = DisplayDriver::M5_BUILTIN;
    p.width = 240;
    p.height = 135;
    p.rotation = 1;
    p.colorDepth = 16;
    p.builtin = true;
    p.compositorMode = DisplayCompositorMode::DIRECT;
    p.logicalWidth = 240;
    p.logicalHeight = 135;
    p.tileSize = 16;
    p.fullFlushThreshold = 38;

    p.spiHost = DisplaySpiHost::SPI3;
    p.spiMode = 0;
    p.freqWrite = 40000000UL;
    p.freqRead = 16000000UL;
    p.spi3Wire = false;
    p.dmaChannel = 0;
    p.busShared = false;
    p.useLock = true;

    p.initOrder = DisplayInitOrder::ACTIVE_FIRST;
    p.initDelayMs = 60;
    p.sharesBusWithSd = false;
    p.releaseBeforeSd = false;
}

void DisplayProfileManager::populateExternalDefault(DisplayProfile& p) {
    p = DisplayProfile();
    strncpy(p.name, "external_ili9488", DISPLAY_NAME_MAX_LEN - 1);
    p.driver = DisplayDriver::TFT_ESPI_ILI9488;
    p.width = 480;
    p.height = 320;
    p.rotation = 3;
    p.colorDepth = 16;
    p.builtin = false;
    p.compositorMode = DisplayCompositorMode::AUTO;
    p.logicalWidth = 240;
    p.logicalHeight = 160;
    p.tileSize = 16;
    p.fullFlushThreshold = 38;

    // Cardputer ADV exposes the SD bus on the EXT connector. The external
    // panel must therefore be a device on SPI2, while the built-in ST7789
    // remains on SPI3.
    p.spiHost = DisplaySpiHost::SPI2;
    p.spiMode = 0;
    p.freqWrite = 40000000UL;
    p.freqRead = 0;
    p.spi3Wire = false;
    p.dmaChannel = 0;
    p.busShared = true;
    p.useLock = true;

    p.initOrder = DisplayInitOrder::EXTERNAL_FIRST;
    p.initDelayMs = 120;

    p.sharesBusWithSd = true;
    p.releaseBeforeSd = true;

    p.pins.cs = 5;
    p.pins.dc = 6;
    p.pins.rst = 3;
    p.pins.mosi = 14;
    p.pins.sclk = 40;
    // This is the bus MISO pin used by SD. ILI9488 SDO stays physically
    // disconnected and the panel remains write-only.
    p.pins.miso = 39;
    p.pins.bl = -1;
}

// ============================================================================
// Utility
// ============================================================================

String DisplayProfileManager::getDisplayListFormatted() {
    String out;
    if (_profileCount == 0) {
        return "No display profiles";
    }

    for (uint8_t i = 0; i < _profileCount; i++) {
        out += (i == _activeIndex) ? "> " : "  ";
        out += _profiles[i].toString();
        out += '\n';
    }
    return out;
}

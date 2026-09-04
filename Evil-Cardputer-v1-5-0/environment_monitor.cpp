#include "environment_monitor.h"

#include <ArduinoJson.h>
#include <M5Cardputer.h>
#include <M5Unified.h>
#include <SD.h>
#include <Wire.h>
#include <cmath>
#include <cstring>

#include "display_runtime.h"
#include "gui/gui.h"
#include "gui/core/gui_display_lock.h"
#include "gui/core/gui_low_memory_compositor.h"
#include "i2c_manager.h"
#include "input_compat.h"
#include "scroll_input.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern bool inMenu;

namespace EnvironmentMonitor {
namespace {

using Screen = GUI::LegacyBridge;

constexpr char kConfigPath[] = "/evil/config/environment.json";
constexpr char kConfigTmpPath[] = "/evil/config/environment.tmp";
constexpr char kConfigBakPath[] = "/evil/config/environment.bak";
constexpr uint8_t kBh1750Address = 0x23;
constexpr uint8_t kScd40Address = 0x62;
constexpr uint16_t kScdStartPeriodic = 0x21B1;
constexpr uint16_t kScdReadMeasurement = 0xEC05;
constexpr uint16_t kScdStopPeriodic = 0x3F86;
constexpr uint16_t kScdDataReady = 0xE4B8;
constexpr int64_t kTaskPeriodUs = 50000;
constexpr int64_t kLightSampleUs = 500000;
constexpr int64_t kAlertFlashLimitUs = 10000000;

enum class LidState : uint8_t {
    Warming,
    Open,
    Closing,
    Closed,
    Opening,
    DarkGuard,
};

enum class UiEvent : uint8_t { None, Up, Down, Enter, Back };

struct Config {
    bool lidEnabled = true;
    bool co2AlertEnabled = true;
    bool alertFlash = true;
    uint16_t co2RefreshSeconds = 5;
    uint16_t co2AlertPpm = 1000;
    uint16_t displayTimeoutSeconds = 0;
    uint16_t baselineUpdateSeconds = 15;
    uint16_t closeHoldMs = 1500;
    float closeRatio = 0.18f;
    float openRatio = 0.45f;
    float minimumBaselineLux = 3.0f;
    DisplayTarget timeoutTarget = DisplayTarget::Both;
    DisplayTarget lidTarget = DisplayTarget::External;
    DisplayTarget alertTarget = DisplayTarget::Both;
};

struct SensorRoute {
    bool valid = false;
    uint8_t address = 0;
    uint8_t hubAddress = 0;
    uint8_t channel = 0xFF;
};

struct SensorReading {
    bool valid = false;
    uint16_t co2 = 0;
    float temperature = 0.0f;
    float humidity = 0.0f;
    int64_t timestampUs = 0;
};

struct Snapshot {
    Config config;
    SensorRoute lightRoute;
    SensorRoute scdRoute;
    SensorReading air;
    bool lightValid = false;
    float lux = 0.0f;
    float baselineLux = 0.0f;
    LidState lidState = LidState::Warming;
    bool lidClosed = false;
    bool co2Alert = false;
    bool manualPending = false;
    bool scanning = false;
    char lightStatus[48] = {};
    char scdStatus[48] = {};
};

SemaphoreHandle_t s_stateMutex = nullptr;
TaskHandle_t s_task = nullptr;
portMUX_TYPE s_activityMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE s_flagMux = portMUX_INITIALIZER_UNLOCKED;
volatile int64_t s_lastActivityUs = 0;
volatile bool s_running = false;
volatile bool s_rescanRequested = false;
volatile bool s_calibrateRequested = false;
volatile bool s_uiRefreshRequested = false;

Config s_config;
SensorRoute s_lightRoute;
SensorRoute s_scdRoute;
SensorReading s_air;
bool s_lightValid = false;
float s_lux = 0.0f;
float s_baselineLux = 0.0f;
bool s_baselineValid = false;
LidState s_lidState = LidState::Warming;
bool s_lidClosed = false;
bool s_co2Alert = false;
bool s_manualPending = false;
bool s_scanning = false;
bool s_uiOpen = false;
char s_lightStatus[48] = "Not scanned";
char s_scdStatus[48] = "Not scanned";

bool s_lightStarted = false;
bool s_scdStarted = false;
int64_t s_lightReadyUs = 0;
int64_t s_nextLightReadUs = 0;
int64_t s_nextScdReadUs = 0;
int64_t s_lastBaselineUpdateUs = 0;
int64_t s_lidCandidateUs = 0;
int64_t s_lidWarmupUntilUs = 0;
int64_t s_alertStartedUs = 0;
uint8_t s_alertHighCount = 0;
uint8_t s_alertClearCount = 0;

bool s_internalAwake = true;
bool s_externalAwake = true;
uint8_t s_internalBrightness = 90;
uint8_t s_externalBrightness = 128;
lgfx::LGFX_Device* s_lastExternalDevice = nullptr;
bool s_externalPowerSuspended = false;
bool s_externalAlertSuspended = false;
bool s_alertOverlayActive = false;
bool s_lastAlertPhase = false;
bool s_steadyAlertDrawn = false;

template <typename T>
T clampValue(T value, T low, T high) {
    return value < low ? low : (value > high ? high : value);
}

void copyText(char* dst, size_t size, const char* src) {
    if (size == 0) return;
    std::strncpy(dst, src ? src : "", size - 1);
    dst[size - 1] = '\0';
}

bool lockState(TickType_t ticks = pdMS_TO_TICKS(100)) {
    return s_stateMutex && xSemaphoreTake(s_stateMutex, ticks) == pdTRUE;
}

void unlockState() {
    if (s_stateMutex) xSemaphoreGive(s_stateMutex);
}

Config configSnapshot() {
    Config result;
    if (lockState()) {
        result = s_config;
        unlockState();
    }
    return result;
}

Snapshot snapshot() {
    Snapshot out;
    if (!lockState()) return out;
    out.config = s_config;
    out.lightRoute = s_lightRoute;
    out.scdRoute = s_scdRoute;
    out.air = s_air;
    out.lightValid = s_lightValid;
    out.lux = s_lux;
    out.baselineLux = s_baselineLux;
    out.lidState = s_lidState;
    out.lidClosed = s_lidClosed;
    out.co2Alert = s_co2Alert;
    out.manualPending = s_manualPending;
    out.scanning = s_scanning;
    copyText(out.lightStatus, sizeof(out.lightStatus), s_lightStatus);
    copyText(out.scdStatus, sizeof(out.scdStatus), s_scdStatus);
    unlockState();
    return out;
}

const char* targetName(DisplayTarget target) {
    switch (target) {
        case DisplayTarget::Internal: return "Internal";
        case DisplayTarget::External: return "External";
        default: return "Both";
    }
}

DisplayTarget parseTarget(const char* value, DisplayTarget fallback) {
    if (!value) return fallback;
    if (strcmp(value, "internal") == 0) return DisplayTarget::Internal;
    if (strcmp(value, "external") == 0) return DisplayTarget::External;
    if (strcmp(value, "both") == 0) return DisplayTarget::Both;
    return fallback;
}

const char* targetJson(DisplayTarget target) {
    switch (target) {
        case DisplayTarget::Internal: return "internal";
        case DisplayTarget::External: return "external";
        default: return "both";
    }
}

bool includesTarget(DisplayTarget target, bool external) {
    return target == DisplayTarget::Both ||
           (external ? target == DisplayTarget::External
                     : target == DisplayTarget::Internal);
}

const char* lidStateName(LidState state) {
    switch (state) {
        case LidState::Open: return "OPEN";
        case LidState::Closing: return "CLOSING";
        case LidState::Closed: return "CLOSED";
        case LidState::Opening: return "OPENING";
        case LidState::DarkGuard: return "NIGHT GUARD";
        default: return "WARMING";
    }
}

String routeName(const SensorRoute& route) {
    if (!route.valid) return "not found";
    if (route.channel == 0xFF) return "direct 0x" + String(route.address, HEX);
    String text = "hub 0x" + String(route.hubAddress, HEX);
    text += " / ch" + String(route.channel);
    return text;
}

bool loadConfig() {
    DisplayRuntime::ScopedSdDisplayRelease sdGuard;
    if (!SD.exists(kConfigPath)) return false;
    File file = SD.open(kConfigPath, FILE_READ);
    if (!file) return false;
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument doc;
#else
    StaticJsonDocument<1536> doc;
#endif
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    if (error) return false;

    Config cfg;
    cfg.lidEnabled = doc["lid_enabled"] | cfg.lidEnabled;
    cfg.co2AlertEnabled = doc["co2_alert_enabled"] | cfg.co2AlertEnabled;
    cfg.alertFlash = doc["alert_flash"] | cfg.alertFlash;
    cfg.co2RefreshSeconds = clampValue<uint16_t>(doc["co2_refresh_s"] | cfg.co2RefreshSeconds, 5, 3600);
    cfg.co2AlertPpm = clampValue<uint16_t>(doc["co2_alert_ppm"] | cfg.co2AlertPpm, 400, 2000);
    cfg.displayTimeoutSeconds = clampValue<uint16_t>(doc["display_timeout_s"] | cfg.displayTimeoutSeconds, 0, 3600);
    cfg.baselineUpdateSeconds = clampValue<uint16_t>(doc["baseline_update_s"] | cfg.baselineUpdateSeconds, 5, 300);
    cfg.closeHoldMs = clampValue<uint16_t>(doc["close_hold_ms"] | cfg.closeHoldMs, 500, 5000);
    cfg.closeRatio = clampValue<float>(doc["close_ratio"] | cfg.closeRatio, 0.05f, 0.50f);
    cfg.openRatio = clampValue<float>(doc["open_ratio"] | cfg.openRatio, cfg.closeRatio + 0.10f, 0.90f);
    cfg.minimumBaselineLux = clampValue<float>(doc["minimum_baseline_lux"] | cfg.minimumBaselineLux, 1.0f, 50.0f);
    cfg.timeoutTarget = parseTarget(doc["timeout_target"] | nullptr, cfg.timeoutTarget);
    cfg.lidTarget = parseTarget(doc["lid_target"] | nullptr, cfg.lidTarget);
    cfg.alertTarget = parseTarget(doc["alert_target"] | nullptr, cfg.alertTarget);
    s_config = cfg;
    return true;
}

bool saveConfig() {
    Config cfg = configSnapshot();
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument doc;
#else
    StaticJsonDocument<1536> doc;
#endif
    doc["version"] = 1;
    doc["lid_enabled"] = cfg.lidEnabled;
    doc["co2_alert_enabled"] = cfg.co2AlertEnabled;
    doc["alert_flash"] = cfg.alertFlash;
    doc["co2_refresh_s"] = cfg.co2RefreshSeconds;
    doc["co2_alert_ppm"] = cfg.co2AlertPpm;
    doc["display_timeout_s"] = cfg.displayTimeoutSeconds;
    doc["baseline_update_s"] = cfg.baselineUpdateSeconds;
    doc["close_hold_ms"] = cfg.closeHoldMs;
    doc["close_ratio"] = cfg.closeRatio;
    doc["open_ratio"] = cfg.openRatio;
    doc["minimum_baseline_lux"] = cfg.minimumBaselineLux;
    doc["timeout_target"] = targetJson(cfg.timeoutTarget);
    doc["lid_target"] = targetJson(cfg.lidTarget);
    doc["alert_target"] = targetJson(cfg.alertTarget);

    DisplayRuntime::ScopedSdDisplayRelease sdGuard;
    SD.mkdir("/evil");
    SD.mkdir("/evil/config");
    SD.remove(kConfigTmpPath);
    File file = SD.open(kConfigTmpPath, FILE_WRITE);
    if (!file) return false;
    const bool written = serializeJsonPretty(doc, file) > 0;
    file.flush();
    file.close();
    if (!written) {
        SD.remove(kConfigTmpPath);
        return false;
    }

    SD.remove(kConfigBakPath);
    const bool hadOriginal = SD.exists(kConfigPath);
    if (hadOriginal && !SD.rename(kConfigPath, kConfigBakPath)) {
        SD.remove(kConfigTmpPath);
        return false;
    }
    if (!SD.rename(kConfigTmpPath, kConfigPath)) {
        if (hadOriginal) SD.rename(kConfigBakPath, kConfigPath);
        return false;
    }
    SD.remove(kConfigBakPath);
    return true;
}

void detectRoutesFromRegistry() {
    SensorRoute light;
    SensorRoute scd;
    for (uint8_t i = 0; i < I2CManager::getDeviceCount(); ++i) {
        const I2CDeviceInfo* device = I2CManager::getDevice(i);
        if (!device) continue;
        SensorRoute route;
        route.valid = true;
        route.address = device->address;
        route.hubAddress = device->paHubAddr;
        route.channel = device->paHubIndex;
        if (!light.valid && device->type == I2CDeviceType::DLight) light = route;
        if (!scd.valid && device->type == I2CDeviceType::SCD4x) scd = route;
    }
    if (lockState()) {
        s_lightRoute = light;
        s_scdRoute = scd;
        copyText(s_lightStatus, sizeof(s_lightStatus),
                 light.valid ? "BH1750 detected" : "No I2C DLight (0x23)");
        copyText(s_scdStatus, sizeof(s_scdStatus),
                 scd.valid ? "SCD40 detected" : "No SCD40 (0x62)");
        unlockState();
    }
}

uint8_t sensirionCrc(const uint8_t* data, size_t count) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < count; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31)
                               : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

bool writeCommand(uint8_t address, uint16_t command) {
    Wire.beginTransmission(address);
    Wire.write(static_cast<uint8_t>(command >> 8));
    Wire.write(static_cast<uint8_t>(command & 0xFF));
    return Wire.endTransmission() == 0;
}

bool initBh1750(const SensorRoute& route) {
    if (!route.valid) return false;
    I2CPaHubRouteGuard guard(route.hubAddress, route.channel, 150);
    if (!guard.ready()) return false;
    Wire.beginTransmission(route.address);
    Wire.write(0x01); // power on
    if (Wire.endTransmission() != 0) return false;
    Wire.beginTransmission(route.address);
    Wire.write(0x10); // continuous high-resolution mode, 1 lx
    return Wire.endTransmission() == 0;
}

bool readBh1750(const SensorRoute& route, float& lux) {
    I2CPaHubRouteGuard guard(route.hubAddress, route.channel, 100);
    if (!guard.ready()) return false;
    if (Wire.requestFrom(route.address, static_cast<uint8_t>(2)) != 2) return false;
    const uint16_t raw = (static_cast<uint16_t>(Wire.read()) << 8) | Wire.read();
    lux = static_cast<float>(raw) / 1.2f;
    return std::isfinite(lux);
}

bool scdGetDataReady(const SensorRoute& route, bool& ready) {
    ready = false;
    I2CPaHubRouteGuard guard(route.hubAddress, route.channel, 100);
    if (!guard.ready() || !writeCommand(route.address, kScdDataReady)) return false;
    delay(2);
    uint8_t bytes[3];
    if (Wire.requestFrom(route.address, static_cast<uint8_t>(3)) != 3) return false;
    for (uint8_t& byte : bytes) byte = Wire.read();
    if (sensirionCrc(bytes, 2) != bytes[2]) return false;
    const uint16_t status = (static_cast<uint16_t>(bytes[0]) << 8) | bytes[1];
    ready = (status & 0x07FF) != 0;
    return true;
}

bool readScd40(const SensorRoute& route, SensorReading& reading) {
    I2CPaHubRouteGuard guard(route.hubAddress, route.channel, 120);
    if (!guard.ready() || !writeCommand(route.address, kScdReadMeasurement)) return false;
    delay(2);
    uint8_t bytes[9];
    if (Wire.requestFrom(route.address, static_cast<uint8_t>(9)) != 9) return false;
    for (uint8_t& byte : bytes) byte = Wire.read();
    for (uint8_t word = 0; word < 3; ++word) {
        if (sensirionCrc(&bytes[word * 3], 2) != bytes[word * 3 + 2]) return false;
    }
    const uint16_t rawCo2 = (static_cast<uint16_t>(bytes[0]) << 8) | bytes[1];
    const uint16_t rawTemp = (static_cast<uint16_t>(bytes[3]) << 8) | bytes[4];
    const uint16_t rawHumidity = (static_cast<uint16_t>(bytes[6]) << 8) | bytes[7];
    reading.valid = rawCo2 != 0;
    reading.co2 = rawCo2;
    reading.temperature = -45.0f + 175.0f * rawTemp / 65535.0f;
    reading.humidity = 100.0f * rawHumidity / 65535.0f;
    reading.timestampUs = esp_timer_get_time();
    return reading.valid;
}

bool startScd40(const SensorRoute& route) {
    if (!route.valid) return false;
    {
        I2CPaHubRouteGuard guard(route.hubAddress, route.channel, 150);
        if (!guard.ready()) return false;
        writeCommand(route.address, kScdStopPeriodic);
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    {
        I2CPaHubRouteGuard guard(route.hubAddress, route.channel, 150);
        if (!guard.ready()) return false;
        if (writeCommand(route.address, kScdStartPeriodic)) return true;
    }
    bool ready = false;
    return scdGetDataReady(route, ready);
}

void initializeSensors() {
    Snapshot snap = snapshot();
    s_lightStarted = initBh1750(snap.lightRoute);
    s_lightReadyUs = esp_timer_get_time() + 180000;
    s_nextLightReadUs = s_lightReadyUs;
    if (lockState()) {
        if (snap.lightRoute.valid) {
            copyText(s_lightStatus, sizeof(s_lightStatus),
                     s_lightStarted ? "BH1750 measuring" : "BH1750 init failed");
        }
        unlockState();
    }

    s_scdStarted = startScd40(snap.scdRoute);
    s_nextScdReadUs = esp_timer_get_time() + 5000000;
    if (lockState()) {
        if (snap.scdRoute.valid) {
            copyText(s_scdStatus, sizeof(s_scdStatus),
                     s_scdStarted ? "SCD40 warming (5 s)" : "SCD40 init failed");
        }
        unlockState();
    }
}

void processLight(float lux, int64_t nowUs, const Config& cfg, bool calibrate) {
    if (!lockState()) return;
    s_lightValid = true;
    s_lux = lux;

    if (!s_baselineValid || calibrate) {
        s_baselineLux = lux;
        s_baselineValid = true;
        s_lastBaselineUpdateUs = nowUs;
        s_lidWarmupUntilUs = nowUs + 3000000;
        s_lidCandidateUs = 0;
        s_lidClosed = false;
        s_lidState = LidState::Warming;
        unlockState();
        return;
    }

    if (!cfg.lidEnabled) {
        s_lidClosed = false;
        s_lidState = LidState::Open;
        s_lidCandidateUs = 0;
    } else if (nowUs < s_lidWarmupUntilUs) {
        s_lidState = LidState::Warming;
    } else if (s_lidClosed) {
        const float openThreshold = s_baselineLux * cfg.openRatio;
        if (lux >= openThreshold) {
            if (s_lidState != LidState::Opening) s_lidCandidateUs = nowUs;
            s_lidState = LidState::Opening;
            if (nowUs - s_lidCandidateUs >= 600000) {
                s_lidClosed = false;
                s_lidState = LidState::Open;
                s_lidCandidateUs = 0;
                s_baselineLux = lux;
                s_lastBaselineUpdateUs = nowUs;
            }
        } else {
            s_lidState = LidState::Closed;
            s_lidCandidateUs = 0;
        }
    } else if (s_baselineLux < cfg.minimumBaselineLux) {
        s_lidState = LidState::DarkGuard;
        s_lidCandidateUs = 0;
    } else {
        const float closeThreshold = s_baselineLux * cfg.closeRatio;
        const bool enoughDrop = (s_baselineLux - lux) >= 1.0f;
        if (lux <= closeThreshold && enoughDrop) {
            if (s_lidState != LidState::Closing) s_lidCandidateUs = nowUs;
            s_lidState = LidState::Closing;
            if (nowUs - s_lidCandidateUs >= static_cast<int64_t>(cfg.closeHoldMs) * 1000) {
                s_lidClosed = true;
                s_lidState = LidState::Closed;
                s_lidCandidateUs = 0;
            }
        } else {
            s_lidState = LidState::Open;
            s_lidCandidateUs = 0;
        }
    }

    const bool canAdapt = !s_lidClosed && s_lidState != LidState::Closing;
    if (canAdapt && nowUs - s_lastBaselineUpdateUs >=
                        static_cast<int64_t>(cfg.baselineUpdateSeconds) * 1000000) {
        s_baselineLux = s_baselineLux * 0.80f + lux * 0.20f;
        s_lastBaselineUpdateUs = nowUs;
    }
    unlockState();
}

void processAir(const SensorReading& reading, const Config& cfg) {
    bool changed = false;
    if (!lockState()) return;
    s_air = reading;
    s_manualPending = false;
    copyText(s_scdStatus, sizeof(s_scdStatus), "SCD40 OK");
    if (!cfg.co2AlertEnabled) {
        changed = s_co2Alert;
        s_co2Alert = false;
        s_alertHighCount = s_alertClearCount = 0;
    } else if (!s_co2Alert) {
        s_alertHighCount = reading.co2 >= cfg.co2AlertPpm
            ? static_cast<uint8_t>(s_alertHighCount + 1) : 0;
        if (s_alertHighCount >= 2) {
            s_co2Alert = true;
            s_alertStartedUs = reading.timestampUs;
            s_alertClearCount = 0;
            changed = true;
        }
    } else {
        const uint16_t clearAt = cfg.co2AlertPpm > 100 ? cfg.co2AlertPpm - 100 : 0;
        s_alertClearCount = reading.co2 <= clearAt
            ? static_cast<uint8_t>(s_alertClearCount + 1) : 0;
        if (s_alertClearCount >= 2) {
            s_co2Alert = false;
            s_alertHighCount = 0;
            changed = true;
        }
    }
    unlockState();
    if (changed) {
        portENTER_CRITICAL(&s_flagMux);
        s_uiRefreshRequested = true;
        portEXIT_CRITICAL(&s_flagMux);
    }
}

lgfx::LGFX_Device* externalDevice() {
    return DisplayRuntime::usingExternalDisplay()
        ? DisplayRuntime::getActiveDevice() : nullptr;
}

void suspendExternalForPower() {
    if (!s_externalPowerSuspended && GUI::lowMemoryCompositor().active()) {
        GUI::lowMemoryCompositor().suspend();
        s_externalPowerSuspended = true;
    }
}

void resumeExternalForPower() {
    if (s_externalPowerSuspended) {
        s_externalPowerSuspended = false;
        GUI::lowMemoryCompositor().resume();
    }
}

void suspendExternalForAlert() {
    if (!s_externalAlertSuspended && GUI::lowMemoryCompositor().active()) {
        GUI::lowMemoryCompositor().suspend();
        s_externalAlertSuspended = true;
    }
}

void resumeExternalForAlert() {
    if (s_externalAlertSuspended) {
        s_externalAlertSuspended = false;
        GUI::lowMemoryCompositor().resume();
    }
}

void setDevicePower(bool external, bool awake) {
    lgfx::LGFX_Device* device = external ? externalDevice() : &M5.Display;
    if (!device) return;
    bool& current = external ? s_externalAwake : s_internalAwake;
    uint8_t& brightness = external ? s_externalBrightness : s_internalBrightness;
    if (current == awake) return;

    if (external && !awake) suspendExternalForPower();
    GUI::DisplayLockGuard lock(200);
    if (!lock.locked()) return;
    if (awake) {
        device->wakeup();
        device->setBrightness(brightness ? brightness : 96);
        current = true;
        if (external) {
            resumeExternalForPower();
            GUI::lowMemoryCompositor().forceFullRefresh();
        }
        portENTER_CRITICAL(&s_flagMux);
        s_uiRefreshRequested = true;
        portEXIT_CRITICAL(&s_flagMux);
    } else {
        const uint8_t measured = device->getBrightness();
        if (measured > 0) brightness = measured;
        if (external) device->fillScreen(TFT_BLACK);
        device->setBrightness(0);
        device->sleep();
        current = false;
    }
}

void drawAlert(lgfx::LGFX_Device* device, uint16_t co2, bool bright) {
    if (!device) return;
    const uint16_t background = bright ? 0xF800 : 0x6000;
    device->fillScreen(background);
    device->setTextColor(TFT_WHITE, background);
    device->setTextFont(1);
    device->setTextSize(device->width() >= 400 ? 3.0f : 2.0f);
    device->setCursor(10, device->height() / 4);
    device->println("CO2 ALERT");
    device->setTextSize(device->width() >= 400 ? 4.0f : 3.0f);
    device->setCursor(10, device->height() / 2);
    device->printf("%u ppm", co2);
    device->setTextSize(device->width() >= 400 ? 2.0f : 1.5f);
    device->setCursor(10, device->height() - 28);
    device->print("OPEN A WINDOW");
}

void clearAlertOverlay(const Config& cfg) {
    if (!s_alertOverlayActive) return;
    GUI::DisplayLockGuard lock(250);
    if (lock.locked()) {
        if (includesTarget(cfg.alertTarget, false)) M5.Display.fillScreen(TFT_BLACK);
    }
    resumeExternalForAlert();
    if (GUI::lowMemoryCompositor().active()) GUI::lowMemoryCompositor().forceFullRefresh();
    s_alertOverlayActive = false;
    s_steadyAlertDrawn = false;
    portENTER_CRITICAL(&s_flagMux);
    s_uiRefreshRequested = true;
    portEXIT_CRITICAL(&s_flagMux);
}

void updateAlertOverlay(int64_t nowUs, const Config& cfg, const Snapshot& snap) {
    if (!snap.co2Alert || snap.air.co2 == 0 || s_uiOpen) {
        clearAlertOverlay(cfg);
        return;
    }

    const bool flashing = cfg.alertFlash &&
                          nowUs - s_alertStartedUs < kAlertFlashLimitUs;
    const bool phase = flashing ? ((nowUs / 500000) & 1) != 0 : true;
    if (s_alertOverlayActive && phase == s_lastAlertPhase &&
        (flashing || s_steadyAlertDrawn)) return;

    if (includesTarget(cfg.alertTarget, true)) suspendExternalForAlert();
    GUI::DisplayLockGuard lock(250);
    if (!lock.locked()) return;
    if (includesTarget(cfg.alertTarget, false) && s_internalAwake) {
        drawAlert(&M5.Display, snap.air.co2, phase);
    }
    if (includesTarget(cfg.alertTarget, true) && s_externalAwake) {
        drawAlert(externalDevice(), snap.air.co2, phase);
    }
    s_alertOverlayActive = true;
    s_lastAlertPhase = phase;
    if (!flashing) s_steadyAlertDrawn = true;
}

void applyDisplayPolicy(int64_t nowUs, const Config& cfg) {
    Snapshot snap = snapshot();
    int64_t lastActivity;
    portENTER_CRITICAL(&s_activityMux);
    lastActivity = s_lastActivityUs;
    portEXIT_CRITICAL(&s_activityMux);
    const bool timedOut = cfg.displayTimeoutSeconds > 0 &&
        nowUs - lastActivity >= static_cast<int64_t>(cfg.displayTimeoutSeconds) * 1000000;

    lgfx::LGFX_Device* ext = externalDevice();
    if (ext != s_lastExternalDevice) {
        s_lastExternalDevice = ext;
        s_externalAwake = true;
        s_externalPowerSuspended = false;
        s_externalAlertSuspended = false;
    }

    for (uint8_t index = 0; index < 2; ++index) {
        const bool external = index == 1;
        bool desired = true;
        const bool lidBlocks = snap.lidClosed && includesTarget(cfg.lidTarget, external);
        if (timedOut && includesTarget(cfg.timeoutTarget, external)) desired = false;
        if (lidBlocks) desired = false;
        if (snap.co2Alert && includesTarget(cfg.alertTarget, external) && !lidBlocks) {
            desired = true;
        }
        setDevicePower(external, desired);
    }
    updateAlertOverlay(nowUs, cfg, snap);
}

void performRescan() {
    if (lockState()) {
        s_scanning = true;
        copyText(s_lightStatus, sizeof(s_lightStatus), "Scanning PaHub...");
        copyText(s_scdStatus, sizeof(s_scdStatus), "Scanning PaHub...");
        unlockState();
    }
    s_lightStarted = s_scdStarted = false;
    I2CDeviceInfo devices[32];
    if (I2CManager::isEnabled()) I2CManager::fullScan(devices, 32);
    detectRoutesFromRegistry();
    initializeSensors();
    if (lockState()) {
        s_scanning = false;
        unlockState();
    }
}

void sensorTask(void*) {
    detectRoutesFromRegistry();
    initializeSensors();
    TickType_t lastWake = xTaskGetTickCount();
    while (s_running) {
        const int64_t nowUs = esp_timer_get_time();
        bool rescan = false;
        bool calibrate = false;
        portENTER_CRITICAL(&s_flagMux);
        rescan = s_rescanRequested;
        calibrate = s_calibrateRequested;
        s_rescanRequested = false;
        s_calibrateRequested = false;
        portEXIT_CRITICAL(&s_flagMux);
        if (rescan) {
            performRescan();
            lastWake = xTaskGetTickCount();
        }

        const Config cfg = configSnapshot();
        const Snapshot snap = snapshot();
        if (I2CManager::isEnabled() && s_lightStarted &&
            nowUs >= s_nextLightReadUs) {
            float lux = 0.0f;
            if (readBh1750(snap.lightRoute, lux)) {
                processLight(lux, nowUs, cfg, calibrate);
                if (lockState()) {
                    copyText(s_lightStatus, sizeof(s_lightStatus), "BH1750 OK");
                    unlockState();
                }
            } else if (lockState()) {
                copyText(s_lightStatus, sizeof(s_lightStatus), "BH1750 read failed");
                unlockState();
            }
            s_nextLightReadUs = nowUs + kLightSampleUs;
        } else if (calibrate && lockState()) {
            if (s_lightValid) {
                s_baselineLux = s_lux;
                s_baselineValid = true;
                s_lidWarmupUntilUs = nowUs + 3000000;
                s_lidClosed = false;
                s_lidState = LidState::Warming;
            }
            unlockState();
        }

        bool manualPending = false;
        if (lockState()) {
            manualPending = s_manualPending;
            unlockState();
        }
        if (I2CManager::isEnabled() && s_scdStarted &&
            (manualPending || nowUs >= s_nextScdReadUs)) {
            bool ready = false;
            if (scdGetDataReady(snap.scdRoute, ready) && ready) {
                SensorReading reading;
                if (readScd40(snap.scdRoute, reading)) {
                    processAir(reading, cfg);
                    s_nextScdReadUs = nowUs +
                        static_cast<int64_t>(cfg.co2RefreshSeconds) * 1000000;
                }
            }
        }

        applyDisplayPolicy(nowUs, cfg);
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(kTaskPeriodUs / 1000));
    }
    clearAlertOverlay(configSnapshot());
    s_task = nullptr;
    vTaskDelete(nullptr);
}

void drawHeader(const char* title, uint16_t color = TFT_NAVY) {
    Screen::fillScreen(TFT_BLACK);
    Screen::fillRect(0, 0, Screen::width(), 15, color);
    Screen::setTextFont(1);
    Screen::setTextSize(1.0f);
    Screen::setTextColor(TFT_WHITE, color);
    Screen::setCursor(4, 4);
    Screen::print(title);
}

void drawList(const char* title, String* items, int count, int selected,
              bool alert = false) {
    drawHeader(title, alert ? TFT_RED : TFT_NAVY);
    const int lineHeight = 14;
    const int visible = (Screen::height() - 18) / lineHeight;
    int first = selected - visible / 2;
    if (first < 0) first = 0;
    if (first > count - visible) first = count - visible;
    if (first < 0) first = 0;
    for (int row = 0; row < visible; ++row) {
        const int item = first + row;
        if (item >= count) break;
        const int y = 17 + row * lineHeight;
        if (item == selected) {
            Screen::fillRect(0, y - 1, Screen::width(), lineHeight, TFT_DARKCYAN);
            Screen::setTextColor(TFT_YELLOW, TFT_DARKCYAN);
        } else {
            Screen::setTextColor(TFT_WHITE, TFT_BLACK);
        }
        Screen::setCursor(4, y + 2);
        Screen::print(items[item]);
    }
    Screen::display();
}

UiEvent readUiEvent() {
    static bool enterWasDown = false;
    static bool backWasDown = false;
    static uint32_t lastNavMs = 0;
    M5.update();
    M5Cardputer.update();
    poll();

    if (ScrollInput::isConnected()) {
        ScrollInput::poll();
        const ScrollEvent wheel = ScrollInput::getMenuEvent();
        if (wheel == ScrollEvent::ScrollUp) return UiEvent::Up;
        if (wheel == ScrollEvent::ScrollDown) return UiEvent::Down;
        if (wheel == ScrollEvent::ButtonClick) return UiEvent::Enter;
    }

    const bool enterDown = InputCompat::isEnterPressed();
    const bool backDown = InputCompat::isBackPressed();
    if (backDown && !backWasDown) {
        backWasDown = true;
        return UiEvent::Back;
    }
    if (!backDown) backWasDown = false;
    if (enterDown && !enterWasDown) {
        enterWasDown = true;
        return UiEvent::Enter;
    }
    if (!enterDown) enterWasDown = false;

    const bool up = M5Cardputer.Keyboard.isKeyPressed(';') ||
                    M5Cardputer.Keyboard.isKeyPressed(',');
    const bool down = M5Cardputer.Keyboard.isKeyPressed('.') ||
                      M5Cardputer.Keyboard.isKeyPressed('/');
    const uint32_t now = millis();
    if ((up || down) && (lastNavMs == 0 || now - lastNavMs >= 150)) {
        lastNavMs = now;
        notifyUserActivity();
        return up ? UiEvent::Up : UiEvent::Down;
    }
    if (!up && !down) lastNavMs = 0;
    delay(10);
    return UiEvent::None;
}

int chooseValue(const char* title, const char* const* values, int count,
                int initial) {
    int selected = clampValue(initial, 0, count - 1);
    while (true) {
        String lines[16];
        for (int i = 0; i < count && i < 16; ++i) lines[i] = values[i];
        drawList(title, lines, count, selected);
        const UiEvent event = readUiEvent();
        if (event == UiEvent::Up) selected = (selected + count - 1) % count;
        if (event == UiEvent::Down) selected = (selected + 1) % count;
        if (event == UiEvent::Enter) return selected;
        if (event == UiEvent::Back) return -1;
    }
}

void showMessage(const char* title, const String& message, uint16_t color = TFT_NAVY) {
    drawHeader(title, color);
    Screen::setTextColor(TFT_WHITE, TFT_BLACK);
    Screen::setCursor(6, 30);
    Screen::println(message);
    Screen::setTextColor(TFT_DARKGREY, TFT_BLACK);
    Screen::setCursor(6, Screen::height() - 14);
    Screen::print("DEL: back");
    Screen::display();
}

void co2View() {
    int64_t lastDraw = -1;
    while (true) {
        Snapshot snap = snapshot();
        const int64_t second = esp_timer_get_time() / 1000000;
        if (second != lastDraw) {
            lastDraw = second;
            drawHeader("CO2", snap.co2Alert ? TFT_RED : TFT_DARKGREEN);
            if (!snap.scdRoute.valid) {
                Screen::setTextColor(TFT_ORANGE, TFT_BLACK);
                Screen::setCursor(6, 28);
                Screen::println("SCD40 not found");
                Screen::setCursor(6, 44);
                Screen::println(snap.scdStatus);
            } else if (!snap.air.valid) {
                Screen::setTextColor(TFT_YELLOW, TFT_BLACK);
                Screen::setCursor(6, 30);
                Screen::println("Waiting for first 5 s sample...");
            } else {
                const uint16_t color = snap.co2Alert ? TFT_RED
                    : (snap.air.co2 >= 1000 ? TFT_ORANGE
                       : (snap.air.co2 >= 800 ? TFT_YELLOW : TFT_GREEN));
                Screen::setTextColor(color, TFT_BLACK);
                Screen::setTextSize(3.0f);
                Screen::setCursor(8, 28);
                Screen::printf("%u", snap.air.co2);
                Screen::setTextSize(1.0f);
                Screen::setCursor(112, 48);
                Screen::print("ppm");
                Screen::setTextColor(TFT_WHITE, TFT_BLACK);
                Screen::setCursor(6, 72);
                Screen::printf("T %.1f C   RH %.1f %%", snap.air.temperature, snap.air.humidity);
                Screen::setCursor(6, 88);
                Screen::printf("Alert: %u ppm", snap.config.co2AlertPpm);
            }
            Screen::setTextColor(TFT_CYAN, TFT_BLACK);
            Screen::setCursor(6, Screen::height() - 24);
            Screen::print(snap.manualPending ? "Reading when data is ready..." : "ENTER: read latest sample");
            Screen::setTextColor(TFT_DARKGREY, TFT_BLACK);
            Screen::setCursor(6, Screen::height() - 12);
            Screen::print(routeName(snap.scdRoute));
            Screen::display();
        }
        const UiEvent event = readUiEvent();
        if (event == UiEvent::Back) return;
        if (event == UiEvent::Enter && lockState()) {
            s_manualPending = true;
            unlockState();
            notifyUserActivity();
            lastDraw = -1;
        }
    }
}

void temperatureView() {
    int64_t lastDraw = -1;
    while (true) {
        Snapshot snap = snapshot();
        const int64_t second = esp_timer_get_time() / 1000000;
        if (second != lastDraw) {
            lastDraw = second;
            drawHeader("Temperature / Humidity", TFT_DARKGREEN);
            if (!snap.air.valid) {
                Screen::setTextColor(TFT_YELLOW, TFT_BLACK);
                Screen::setCursor(6, 32);
                Screen::println("No SCD40 sample yet");
            } else {
                Screen::setTextColor(TFT_CYAN, TFT_BLACK);
                Screen::setTextSize(2.5f);
                Screen::setCursor(8, 30);
                Screen::printf("%.1f C", snap.air.temperature);
                Screen::setCursor(8, 68);
                Screen::printf("%.1f %%", snap.air.humidity);
                Screen::setTextSize(1.0f);
            }
            Screen::setTextColor(TFT_DARKGREY, TFT_BLACK);
            Screen::setCursor(6, Screen::height() - 12);
            Screen::print("ENTER: refresh   DEL: back");
            Screen::display();
        }
        const UiEvent event = readUiEvent();
        if (event == UiEvent::Back) return;
        if (event == UiEvent::Enter && lockState()) {
            s_manualPending = true;
            unlockState();
            lastDraw = -1;
        }
    }
}

void luxView() {
    int64_t lastDrawBucket = -1;
    while (true) {
        Snapshot snap = snapshot();
        const int64_t bucket = esp_timer_get_time() / 500000;
        if (bucket != lastDrawBucket) {
            lastDrawBucket = bucket;
            const uint16_t header = snap.lidClosed ? TFT_RED
                : (snap.lidState == LidState::DarkGuard ? TFT_ORANGE : TFT_DARKGREEN);
            drawHeader("Lux meter / Lid", header);
            if (!snap.lightRoute.valid) {
                Screen::setTextColor(TFT_ORANGE, TFT_BLACK);
                Screen::setCursor(6, 26);
                Screen::println("No DLight/BH1750 at 0x23");
                Screen::setCursor(6, 44);
                Screen::println("Unit Light U021 is analog");
                Screen::setCursor(6, 58);
                Screen::println("and cannot use PaHub.");
            } else if (!snap.lightValid) {
                Screen::setTextColor(TFT_YELLOW, TFT_BLACK);
                Screen::setCursor(6, 30);
                Screen::println("Waiting for BH1750...");
            } else {
                Screen::setTextColor(TFT_CYAN, TFT_BLACK);
                Screen::setTextSize(2.5f);
                Screen::setCursor(6, 24);
                Screen::printf("%.1f lx", snap.lux);
                Screen::setTextSize(1.0f);
                Screen::setTextColor(TFT_WHITE, TFT_BLACK);
                Screen::setCursor(6, 58);
                Screen::printf("Open baseline: %.1f lx", snap.baselineLux);
                Screen::setCursor(6, 72);
                Screen::printf("Close below: %.1f lx", snap.baselineLux * snap.config.closeRatio);
                Screen::setCursor(6, 86);
                Screen::printf("State: %s", lidStateName(snap.lidState));
            }
            Screen::setTextColor(TFT_GREEN, TFT_BLACK);
            Screen::setCursor(6, Screen::height() - 24);
            Screen::print("ENTER: calibrate OPEN lid");
            Screen::setTextColor(TFT_DARKGREY, TFT_BLACK);
            Screen::setCursor(6, Screen::height() - 12);
            Screen::print(routeName(snap.lightRoute));
            Screen::display();
        }
        const UiEvent event = readUiEvent();
        if (event == UiEvent::Back) return;
        if (event == UiEvent::Enter) {
            portENTER_CRITICAL(&s_flagMux);
            s_calibrateRequested = true;
            portEXIT_CRITICAL(&s_flagMux);
            lastDrawBucket = -1;
        }
    }
}

int findValue(const uint16_t* values, int count, uint16_t value) {
    int best = 0;
    uint16_t bestDistance = UINT16_MAX;
    for (int i = 0; i < count; ++i) {
        const uint16_t distance = values[i] > value ? values[i] - value : value - values[i];
        if (distance < bestDistance) { bestDistance = distance; best = i; }
    }
    return best;
}

void saveAfterSetting() {
    notifyUserActivity();
    if (!saveConfig()) showMessage("Settings", "Save failed", TFT_RED);
}

void automationSettings() {
    int selected = 0;
    while (true) {
        Snapshot snap = snapshot();
        String items[14] = {
            "CO2 refresh: " + String(snap.config.co2RefreshSeconds) + " s",
            "CO2 alert: " + String(snap.config.co2AlertPpm) + " ppm",
            String("Alert enabled: ") + (snap.config.co2AlertEnabled ? "ON" : "OFF"),
            String("Alert flash: ") + (snap.config.alertFlash ? "ON" : "OFF"),
            "Idle timeout: " + String(snap.config.displayTimeoutSeconds) + " s",
            "Timeout display: " + String(targetName(snap.config.timeoutTarget)),
            String("Lid control: ") + (snap.config.lidEnabled ? "ON" : "OFF"),
            "Lid display: " + String(targetName(snap.config.lidTarget)),
            "Alert display: " + String(targetName(snap.config.alertTarget)),
            "Baseline every: " + String(snap.config.baselineUpdateSeconds) + " s",
            "Close ratio: " + String(static_cast<int>(snap.config.closeRatio * 100)) + " %",
            "Night guard: " + String(snap.config.minimumBaselineLux, 1) + " lx",
            "Close hold: " + String(snap.config.closeHoldMs) + " ms",
            "Back"
        };
        drawList("Environment automation", items, 14, selected, snap.co2Alert);
        const UiEvent event = readUiEvent();
        if (event == UiEvent::Up) selected = (selected + 13) % 14;
        if (event == UiEvent::Down) selected = (selected + 1) % 14;
        if (event == UiEvent::Back || (event == UiEvent::Enter && selected == 13)) return;
        if (event != UiEvent::Enter) continue;

        const char* const targets[] = {"Internal", "External", "Both"};
        const char* const toggles[] = {"OFF", "ON"};
        const uint16_t refreshValues[] = {5, 10, 15, 30, 60, 120, 300, 600};
        const char* const refreshLabels[] = {"5 s", "10 s", "15 s", "30 s", "60 s", "120 s", "300 s", "600 s"};
        const uint16_t timeoutValues[] = {0, 15, 30, 60, 120, 300, 600, 900, 1800, 3600};
        const char* const timeoutLabels[] = {"Disabled", "15 s", "30 s", "60 s", "2 min", "5 min", "10 min", "15 min", "30 min", "60 min"};
        const uint16_t baselineValues[] = {5, 10, 15, 30, 60, 120, 300};
        const char* const baselineLabels[] = {"5 s", "10 s", "15 s", "30 s", "60 s", "120 s", "300 s"};
        const uint16_t ratioValues[] = {10, 15, 18, 20, 25, 30, 40};
        const char* const ratioLabels[] = {"10 %", "15 %", "18 %", "20 %", "25 %", "30 %", "40 %"};
        const uint16_t nightValues[] = {1, 2, 3, 5, 10, 20, 50};
        const char* const nightLabels[] = {"1 lx", "2 lx", "3 lx", "5 lx", "10 lx", "20 lx", "50 lx"};
        const uint16_t holdValues[] = {500, 1000, 1500, 2000, 3000, 5000};
        const char* const holdLabels[] = {"500 ms", "1000 ms", "1500 ms", "2000 ms", "3000 ms", "5000 ms"};
        int choice = -1;
        if (selected == 0) {
            choice = chooseValue("CO2 refresh (min 5 s)", refreshLabels, 8,
                                 findValue(refreshValues, 8, snap.config.co2RefreshSeconds));
            if (choice >= 0 && lockState()) { s_config.co2RefreshSeconds = refreshValues[choice]; unlockState(); saveAfterSetting(); }
        } else if (selected == 1) {
            const int initial = (snap.config.co2AlertPpm - 400) / 50;
            String thresholdItems[33];
            for (int i = 0; i < 33; ++i) thresholdItems[i] = String(400 + i * 50) + " ppm";
            int current = clampValue(initial, 0, 32);
            while (true) {
                drawList("CO2 threshold 400..2000", thresholdItems, 33, current);
                UiEvent e = readUiEvent();
                if (e == UiEvent::Up) current = (current + 32) % 33;
                if (e == UiEvent::Down) current = (current + 1) % 33;
                if (e == UiEvent::Back) break;
                if (e == UiEvent::Enter) {
                    if (lockState()) { s_config.co2AlertPpm = 400 + current * 50; unlockState(); saveAfterSetting(); }
                    break;
                }
            }
        } else if (selected == 2 || selected == 3 || selected == 6) {
            const bool initial = selected == 2 ? snap.config.co2AlertEnabled
                : (selected == 3 ? snap.config.alertFlash : snap.config.lidEnabled);
            choice = chooseValue("Toggle", toggles, 2, initial ? 1 : 0);
            if (choice >= 0 && lockState()) {
                if (selected == 2) s_config.co2AlertEnabled = choice == 1;
                if (selected == 3) s_config.alertFlash = choice == 1;
                if (selected == 6) { s_config.lidEnabled = choice == 1; if (!s_config.lidEnabled) { s_lidClosed = false; s_lidState = LidState::Open; } }
                unlockState(); saveAfterSetting();
            }
        } else if (selected == 4) {
            choice = chooseValue("Exact idle timeout", timeoutLabels, 10,
                                 findValue(timeoutValues, 10, snap.config.displayTimeoutSeconds));
            if (choice >= 0 && lockState()) { s_config.displayTimeoutSeconds = timeoutValues[choice]; unlockState(); saveAfterSetting(); }
        } else if (selected == 5 || selected == 7 || selected == 8) {
            DisplayTarget currentTarget = selected == 5 ? snap.config.timeoutTarget
                : (selected == 7 ? snap.config.lidTarget : snap.config.alertTarget);
            choice = chooseValue("Display target", targets, 3, static_cast<int>(currentTarget) - 1);
            if (choice >= 0 && lockState()) {
                const DisplayTarget target = static_cast<DisplayTarget>(choice + 1);
                if (selected == 5) s_config.timeoutTarget = target;
                if (selected == 7) s_config.lidTarget = target;
                if (selected == 8) s_config.alertTarget = target;
                unlockState(); saveAfterSetting();
            }
        } else if (selected == 9) {
            choice = chooseValue("Baseline update", baselineLabels, 7,
                                 findValue(baselineValues, 7, snap.config.baselineUpdateSeconds));
            if (choice >= 0 && lockState()) { s_config.baselineUpdateSeconds = baselineValues[choice]; unlockState(); saveAfterSetting(); }
        } else if (selected == 10) {
            choice = chooseValue("Lid close ratio", ratioLabels, 7,
                                 findValue(ratioValues, 7, static_cast<uint16_t>(snap.config.closeRatio * 100)));
            if (choice >= 0 && lockState()) { s_config.closeRatio = ratioValues[choice] / 100.0f; s_config.openRatio = clampValue(s_config.closeRatio + 0.27f, s_config.closeRatio + 0.10f, 0.90f); unlockState(); saveAfterSetting(); }
        } else if (selected == 11) {
            choice = chooseValue("Night guard floor", nightLabels, 7,
                                 findValue(nightValues, 7, static_cast<uint16_t>(snap.config.minimumBaselineLux)));
            if (choice >= 0 && lockState()) { s_config.minimumBaselineLux = nightValues[choice]; unlockState(); saveAfterSetting(); }
        } else if (selected == 12) {
            choice = chooseValue("Close confirmation", holdLabels, 6,
                                 findValue(holdValues, 6, snap.config.closeHoldMs));
            if (choice >= 0 && lockState()) { s_config.closeHoldMs = holdValues[choice]; unlockState(); saveAfterSetting(); }
        }
    }
}

} // namespace

bool begin() {
    if (s_task) return true;
    if (!s_stateMutex) s_stateMutex = xSemaphoreCreateMutex();
    if (!s_stateMutex) return false;
    loadConfig();
    notifyUserActivity();
    detectRoutesFromRegistry();
    s_running = true;
    const BaseType_t created = xTaskCreatePinnedToCore(
        sensorTask, "environment", 4096, nullptr, 1, &s_task, 0);
    if (created != pdPASS) {
        s_running = false;
        s_task = nullptr;
        Serial.println(F("[Environment] Cannot allocate monitor task"));
        return false;
    }
    Serial.println(F("[Environment] Monitor started"));
    return true;
}

void end() {
    s_running = false;
}

void notifyUserActivity() {
    const int64_t nowUs = esp_timer_get_time();
    portENTER_CRITICAL(&s_activityMux);
    s_lastActivityUs = nowUs;
    portEXIT_CRITICAL(&s_activityMux);
}

void poll() {
    if (M5Cardputer.Keyboard.isChange() || M5Cardputer.Keyboard.isPressed()) {
        notifyUserActivity();
    }
}

bool consumeUiRefreshRequest() {
    bool requested;
    portENTER_CRITICAL(&s_flagMux);
    requested = s_uiRefreshRequested;
    s_uiRefreshRequested = false;
    portEXIT_CRITICAL(&s_flagMux);
    return requested;
}

void runMenu() {
    if (lockState()) { s_uiOpen = true; unlockState(); }
    notifyUserActivity();
    int selected = 0;
    bool running = true;
    while (running) {
        Snapshot snap = snapshot();
        String items[7] = {
            "CO2 monitor",
            "Temperature / humidity",
            "Lux meter / lid",
            "Automation settings",
            snap.scanning ? "Scanning PaHub..." : "Rescan PaHub devices",
            "Save configuration",
            "Back"
        };
        drawList("Environment", items, 7, selected, snap.co2Alert);
        const UiEvent event = readUiEvent();
        if (event == UiEvent::Up) selected = (selected + 6) % 7;
        if (event == UiEvent::Down) selected = (selected + 1) % 7;
        if (event == UiEvent::Back) running = false;
        if (event != UiEvent::Enter) continue;
        if (selected == 0) co2View();
        else if (selected == 1) temperatureView();
        else if (selected == 2) luxView();
        else if (selected == 3) automationSettings();
        else if (selected == 4) {
            portENTER_CRITICAL(&s_flagMux);
            s_rescanRequested = true;
            portEXIT_CRITICAL(&s_flagMux);
        } else if (selected == 5) {
            showMessage("Environment", saveConfig() ? "Configuration saved" : "Save failed",
                        saveConfig() ? TFT_DARKGREEN : TFT_RED);
        } else if (selected == 6) running = false;
    }
    if (lockState()) { s_uiOpen = false; unlockState(); }
    portENTER_CRITICAL(&s_flagMux);
    s_uiRefreshRequested = true;
    portEXIT_CRITICAL(&s_flagMux);
    inMenu = true;
}

} // namespace EnvironmentMonitor

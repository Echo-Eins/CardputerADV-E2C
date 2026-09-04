/**
 * @file i2c_manager.h
 * @brief I2C Bus Manager with PaHub v2.1 (PCA9548AP) support
 *
 * Provides:
 * - Global I2C enable/disable (config-persisted)
 * - PaHub channel switching (up to 6 channels per hub, cascadable)
 * - I2C device scanning and identification
 * - Device registry for known M5Stack peripherals
 */

#ifndef I2C_MANAGER_H
#define I2C_MANAGER_H

#include <Arduino.h>
#include <Wire.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ============================================================================
// Constants
// ============================================================================

// Default I2C pins for M5Stack Cardputer ADV
#define I2C_MGR_DEFAULT_SDA     2
#define I2C_MGR_DEFAULT_SCL     1
#define I2C_MGR_DEFAULT_FREQ    100000  // 100kHz standard mode

// PaHub v2.1 (PCA9548AP) constants
#define PAHUB_BASE_ADDR     0x70    // Base I2C address (A0=A1=A2=0)
#define PAHUB_ADDR_MASK     0x07    // A0-A2 address bits
#define PAHUB_MAX_CHANNELS  6       // PaHub v2.1 exposes 6 of 8 channels
#define PAHUB_MAX_HUBS      8       // Max hubs on bus (0x70-0x77)

// Known device addresses
#define SCROLL_UNIT_ADDR    0x40    // M5Stack Scroll Unit default address
#define SCROLL_FW_VER_REG   0xFE   // Firmware version register
#define SCROLL_I2C_ADDR_REG 0xFF   // I2C address register

// Scan limits
#define I2C_SCAN_MIN_ADDR   0x08   // Skip reserved 0x00-0x07
#define I2C_SCAN_MAX_ADDR   0x77   // Standard range

// ============================================================================
// Device Types
// ============================================================================

enum class I2CDeviceType : uint8_t {
    Unknown = 0,
    PaHub,          // PCA9548AP I2C multiplexer
    ScrollUnit,     // M5Stack Scroll Unit (STM32F030)
    ExtDisplay,     // External I2C display (SSD1306 etc.)
    IMU,            // Internal IMU
    Power,          // Internal power management
    DLight,         // M5 DLight / BH1750 (0x23)
    SCD4x,          // Sensirion SCD40/SCD41 (0x62)
    Other
};

const char* i2cDeviceTypeName(I2CDeviceType type);

// ============================================================================
// Device Info
// ============================================================================

struct I2CDeviceInfo {
    uint8_t address;
    I2CDeviceType type;
    uint8_t firmwareVersion;    // 0 if unknown/not applicable
    uint8_t paHubIndex;         // 0xFF = directly on bus, 0-5 = PaHub channel
    uint8_t paHubAddr;          // Address of parent PaHub (0 if direct)
    bool responsive;            // Last probe was successful

    String toString() const;
};

// ============================================================================
// PaHub Channel State
// ============================================================================

struct PaHubState {
    uint8_t address;            // Hub I2C address (0x70-0x77)
    bool present;               // Hub detected on bus
    uint8_t activeChannelMask;  // Currently active channel bitmask
    uint8_t deviceCount;        // Total devices found across channels
};

// ============================================================================
// I2C Manager Class
// ============================================================================

class I2CManager {
public:
    // Lifecycle
    static bool init(int sdaPin = I2C_MGR_DEFAULT_SDA, int sclPin = I2C_MGR_DEFAULT_SCL,
                     uint32_t freq = I2C_MGR_DEFAULT_FREQ);
    static void shutdown();
    static bool isInitialized();

    // Global I2C enable/disable (persisted to config)
    static bool isEnabled();
    static void setEnabled(bool enabled);
    static void toggleEnabled();

    // Bus operations
    static bool begin();    // Start Wire with stored pins/freq
    static void end();      // Release Wire

    // Recursive cross-task serialization. Drivers may hold this lock while
    // calling other I2CManager methods without deadlocking.
    static bool lockBus(uint32_t timeoutMs = UINT32_MAX);
    static void unlockBus();

    // PaHub channel management
    static bool selectPaHubChannel(uint8_t hubAddr, uint8_t channel);
    static bool deselectAllPaHubChannels(uint8_t hubAddr);
    static uint8_t getActivePaHubChannel(uint8_t hubAddr);
    static bool readPaHubChannelMask(uint8_t hubAddr, uint8_t& mask);
    static bool setPaHubChannelMask(uint8_t hubAddr, uint8_t mask);

    // Device scanning
    static uint8_t scanBus(I2CDeviceInfo* results, uint8_t maxResults);
    static uint8_t scanPaHubChannels(uint8_t hubAddr, I2CDeviceInfo* results,
                                     uint8_t maxResults);
    static uint8_t fullScan(I2CDeviceInfo* results, uint8_t maxResults);

    // Device identification
    static I2CDeviceType identifyDevice(uint8_t address);
    static uint8_t readFirmwareVersion(uint8_t address);
    static bool probeAddress(uint8_t address);

    // PaHub state
    static uint8_t getPaHubCount();
    static const PaHubState* getPaHubState(uint8_t index);

    // Device registry
    static uint8_t getDeviceCount();
    static const I2CDeviceInfo* getDevice(uint8_t index);
    static const I2CDeviceInfo* findDevice(I2CDeviceType type);

    // Utility
    static String getDeviceListFormatted();

private:
    static bool _initialized;
    static bool _enabled;
    static bool _busActive;
    static int _sdaPin;
    static int _sclPin;
    static uint32_t _freq;
    static SemaphoreHandle_t _busMutex;

    // Device registry
    static I2CDeviceInfo _devices[32];
    static uint8_t _deviceCount;

    // PaHub state
    static PaHubState _paHubs[PAHUB_MAX_HUBS];
    static uint8_t _paHubCount;

    // Internal helpers
    static bool isPaHubAddress(uint8_t addr);
    static bool writeByteToDevice(uint8_t addr, uint8_t reg, uint8_t value);
    static uint8_t readByteFromDevice(uint8_t addr, uint8_t reg);
    static void loadConfig();
    static void saveConfig();
};

class I2CBusGuard {
public:
    explicit I2CBusGuard(uint32_t timeoutMs = UINT32_MAX)
        : _locked(I2CManager::lockBus(timeoutMs)) {}
    ~I2CBusGuard() { if (_locked) I2CManager::unlockBus(); }
    I2CBusGuard(const I2CBusGuard&) = delete;
    I2CBusGuard& operator=(const I2CBusGuard&) = delete;
    bool locked() const { return _locked; }
private:
    bool _locked;
};

// Selects one PaHub channel for the lifetime of the guard and restores the
// exact previous channel mask afterwards. hubAddr=0 or channel=0xFF means a
// direct device and still locks the shared bus.
class I2CPaHubRouteGuard {
public:
    I2CPaHubRouteGuard(uint8_t hubAddr, uint8_t channel,
                       uint32_t timeoutMs = UINT32_MAX);
    ~I2CPaHubRouteGuard();
    I2CPaHubRouteGuard(const I2CPaHubRouteGuard&) = delete;
    I2CPaHubRouteGuard& operator=(const I2CPaHubRouteGuard&) = delete;
    bool ready() const { return _ready; }
private:
    uint8_t _hubAddr;
    uint8_t _previousMask;
    bool _locked;
    bool _routed;
    bool _ready;
};

#endif // I2C_MANAGER_H

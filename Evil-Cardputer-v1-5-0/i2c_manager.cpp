/**
 * @file i2c_manager.cpp
 * @brief I2C Bus Manager implementation
 */

#include "i2c_manager.h"
#include "config_manager.h"

// ============================================================================
// Static member initialization
// ============================================================================

bool I2CManager::_initialized = false;
bool I2CManager::_enabled = false;
bool I2CManager::_busActive = false;
int I2CManager::_sdaPin = I2C_MGR_DEFAULT_SDA;
int I2CManager::_sclPin = I2C_MGR_DEFAULT_SCL;
uint32_t I2CManager::_freq = I2C_MGR_DEFAULT_FREQ;

I2CDeviceInfo I2CManager::_devices[32];
uint8_t I2CManager::_deviceCount = 0;

PaHubState I2CManager::_paHubs[PAHUB_MAX_HUBS];
uint8_t I2CManager::_paHubCount = 0;

// ============================================================================
// Device type names
// ============================================================================

const char* i2cDeviceTypeName(I2CDeviceType type) {
    switch (type) {
        case I2CDeviceType::PaHub:      return "PaHub";
        case I2CDeviceType::ScrollUnit: return "Scroll";
        case I2CDeviceType::ExtDisplay: return "Display";
        case I2CDeviceType::IMU:        return "IMU";
        case I2CDeviceType::Power:      return "Power";
        case I2CDeviceType::Other:      return "Other";
        default:                        return "Unknown";
    }
}

String I2CDeviceInfo::toString() const {
    String s = "0x";
    if (address < 0x10) s += "0";
    s += String(address, HEX);
    s += " ";
    s += i2cDeviceTypeName(type);

    if (firmwareVersion > 0) {
        s += " v" + String(firmwareVersion);
    }
    if (paHubIndex != 0xFF) {
        s += " [PH:0x";
        if (paHubAddr < 0x10) s += "0";
        s += String(paHubAddr, HEX);
        s += " CH" + String(paHubIndex) + "]";
    }
    return s;
}

// ============================================================================
// Lifecycle
// ============================================================================

bool I2CManager::init(int sdaPin, int sclPin, uint32_t freq) {
    if (_initialized) return true;

    _sdaPin = sdaPin;
    _sclPin = sclPin;
    _freq = freq;

    loadConfig();

    _initialized = true;
    Serial.println(F("[I2C] Manager initialized"));

    if (_enabled) {
        if (!begin()) {
            Serial.println(F("[I2C] Warning: Failed to start bus on init"));
        } else {
            fullScan(_devices, 32);
            Serial.printf("[I2C] Found %d device(s)\n", _deviceCount);
        }
    }

    return true;
}

void I2CManager::shutdown() {
    if (!_initialized) return;
    end();
    _initialized = false;
    _deviceCount = 0;
    _paHubCount = 0;
    Serial.println(F("[I2C] Manager shutdown"));
}

bool I2CManager::isInitialized() { return _initialized; }

// ============================================================================
// Enable/Disable
// ============================================================================

bool I2CManager::isEnabled() { return _enabled; }

void I2CManager::setEnabled(bool enabled) {
    if (_enabled == enabled) return;
    _enabled = enabled;
    saveConfig();

    if (_enabled) {
        begin();
        fullScan(_devices, 32);
        Serial.printf("[I2C] Enabled, found %d device(s)\n", _deviceCount);
    } else {
        // Deselect all PaHub channels before shutting down
        for (uint8_t i = 0; i < _paHubCount; i++) {
            if (_paHubs[i].present) {
                deselectAllPaHubChannels(_paHubs[i].address);
            }
        }
        end();
        _deviceCount = 0;
        _paHubCount = 0;
        Serial.println(F("[I2C] Disabled"));
    }
}

void I2CManager::toggleEnabled() {
    setEnabled(!_enabled);
}

// ============================================================================
// Bus operations
// ============================================================================

bool I2CManager::begin() {
    if (_busActive) return true;
    if (!Wire.begin(_sdaPin, _sclPin, _freq)) {
        Serial.println(F("[I2C] Wire.begin failed"));
        return false;
    }
    _busActive = true;
    return true;
}

void I2CManager::end() {
    if (!_busActive) return;
    Wire.end();
    _busActive = false;
}

// ============================================================================
// PaHub channel management
// ============================================================================

bool I2CManager::isPaHubAddress(uint8_t addr) {
    return (addr >= PAHUB_BASE_ADDR && addr <= (PAHUB_BASE_ADDR + PAHUB_ADDR_MASK));
}

bool I2CManager::selectPaHubChannel(uint8_t hubAddr, uint8_t channel) {
    if (!_busActive || !isPaHubAddress(hubAddr)) return false;
    if (channel >= PAHUB_MAX_CHANNELS) return false;

    Wire.beginTransmission(hubAddr);
    Wire.write(1 << channel);
    uint8_t err = Wire.endTransmission();

    if (err == 0) {
        // Update PaHub state
        for (uint8_t i = 0; i < _paHubCount; i++) {
            if (_paHubs[i].address == hubAddr) {
                _paHubs[i].activeChannelMask = (1 << channel);
                break;
            }
        }
        return true;
    }

    Serial.printf("[I2C] PaHub 0x%02X channel %d select failed: %d\n",
                  hubAddr, channel, err);
    return false;
}

bool I2CManager::deselectAllPaHubChannels(uint8_t hubAddr) {
    if (!_busActive || !isPaHubAddress(hubAddr)) return false;

    Wire.beginTransmission(hubAddr);
    Wire.write(0x00);  // All channels off
    uint8_t err = Wire.endTransmission();

    if (err == 0) {
        for (uint8_t i = 0; i < _paHubCount; i++) {
            if (_paHubs[i].address == hubAddr) {
                _paHubs[i].activeChannelMask = 0;
                break;
            }
        }
        return true;
    }
    return false;
}

uint8_t I2CManager::getActivePaHubChannel(uint8_t hubAddr) {
    for (uint8_t i = 0; i < _paHubCount; i++) {
        if (_paHubs[i].address == hubAddr) {
            uint8_t mask = _paHubs[i].activeChannelMask;
            for (uint8_t ch = 0; ch < PAHUB_MAX_CHANNELS; ch++) {
                if (mask & (1 << ch)) return ch;
            }
            return 0xFF;  // No channel active
        }
    }
    return 0xFF;
}

// ============================================================================
// Device probing
// ============================================================================

bool I2CManager::probeAddress(uint8_t address) {
    if (!_busActive) return false;
    Wire.beginTransmission(address);
    return (Wire.endTransmission() == 0);
}

I2CDeviceType I2CManager::identifyDevice(uint8_t address) {
    // PaHub range
    if (isPaHubAddress(address)) {
        return I2CDeviceType::PaHub;
    }

    // Scroll Unit: probe firmware version register
    if (address == SCROLL_UNIT_ADDR) {
        uint8_t fwVer = readFirmwareVersion(address);
        if (fwVer > 0) {
            return I2CDeviceType::ScrollUnit;
        }
    }

    // Known display addresses (SSD1306)
    if (address == 0x3C || address == 0x3D) {
        return I2CDeviceType::ExtDisplay;
    }

    // M5Stack internal IMU (MPU6886/BMI270)
    if (address == 0x68 || address == 0x69) {
        return I2CDeviceType::IMU;
    }

    // M5Stack power management (AXP2101/AXP192)
    if (address == 0x34) {
        return I2CDeviceType::Power;
    }

    return I2CDeviceType::Unknown;
}

uint8_t I2CManager::readFirmwareVersion(uint8_t address) {
    return readByteFromDevice(address, SCROLL_FW_VER_REG);
}

// ============================================================================
// Scanning
// ============================================================================

uint8_t I2CManager::scanBus(I2CDeviceInfo* results, uint8_t maxResults) {
    if (!_busActive || !results) return 0;

    uint8_t count = 0;
    for (uint8_t addr = I2C_SCAN_MIN_ADDR; addr <= I2C_SCAN_MAX_ADDR && count < maxResults; addr++) {
        if (probeAddress(addr)) {
            results[count].address = addr;
            results[count].type = identifyDevice(addr);
            results[count].firmwareVersion = 0;
            results[count].paHubIndex = 0xFF;   // Direct on bus
            results[count].paHubAddr = 0;
            results[count].responsive = true;

            // Read firmware version for Scroll Units
            if (results[count].type == I2CDeviceType::ScrollUnit) {
                results[count].firmwareVersion = readFirmwareVersion(addr);
            }

            count++;
        }
    }
    return count;
}

uint8_t I2CManager::scanPaHubChannels(uint8_t hubAddr, I2CDeviceInfo* results,
                                       uint8_t maxResults) {
    if (!_busActive || !results) return 0;

    uint8_t count = 0;

    for (uint8_t ch = 0; ch < PAHUB_MAX_CHANNELS && count < maxResults; ch++) {
        if (!selectPaHubChannel(hubAddr, ch)) continue;

        delay(5);  // Allow channel to settle

        // Scan for devices on this channel (skip PaHub addresses to avoid confusion)
        for (uint8_t addr = I2C_SCAN_MIN_ADDR; addr <= I2C_SCAN_MAX_ADDR && count < maxResults; addr++) {
            if (isPaHubAddress(addr)) continue;  // Skip other PaHub addresses
            if (addr == hubAddr) continue;        // Skip self

            if (probeAddress(addr)) {
                results[count].address = addr;
                results[count].type = identifyDevice(addr);
                results[count].firmwareVersion = 0;
                results[count].paHubIndex = ch;
                results[count].paHubAddr = hubAddr;
                results[count].responsive = true;

                if (results[count].type == I2CDeviceType::ScrollUnit) {
                    results[count].firmwareVersion = readFirmwareVersion(addr);
                }

                count++;
            }
        }
    }

    // Restore: deselect all channels
    deselectAllPaHubChannels(hubAddr);

    return count;
}

uint8_t I2CManager::fullScan(I2CDeviceInfo* results, uint8_t maxResults) {
    if (!_busActive || !results) return 0;

    _paHubCount = 0;
    _deviceCount = 0;

    // Phase 1: Scan direct bus
    uint8_t directCount = scanBus(results, maxResults);

    // Register PaHubs found
    for (uint8_t i = 0; i < directCount; i++) {
        if (results[i].type == I2CDeviceType::PaHub && _paHubCount < PAHUB_MAX_HUBS) {
            _paHubs[_paHubCount].address = results[i].address;
            _paHubs[_paHubCount].present = true;
            _paHubs[_paHubCount].activeChannelMask = 0;
            _paHubs[_paHubCount].deviceCount = 0;
            _paHubCount++;
        }
    }

    uint8_t totalCount = directCount;

    // Phase 2: Scan each PaHub's channels
    for (uint8_t h = 0; h < _paHubCount && totalCount < maxResults; h++) {
        uint8_t chCount = scanPaHubChannels(
            _paHubs[h].address,
            results + totalCount,
            maxResults - totalCount
        );
        _paHubs[h].deviceCount = chCount;
        totalCount += chCount;
    }

    _deviceCount = totalCount;

    // Copy to internal registry
    if (results != _devices) {
        uint8_t copyCount = min(totalCount, (uint8_t)32);
        memcpy(_devices, results, copyCount * sizeof(I2CDeviceInfo));
        _deviceCount = copyCount;
    }

    return totalCount;
}

// ============================================================================
// Device registry
// ============================================================================

uint8_t I2CManager::getDeviceCount() { return _deviceCount; }

const I2CDeviceInfo* I2CManager::getDevice(uint8_t index) {
    if (index >= _deviceCount) return nullptr;
    return &_devices[index];
}

const I2CDeviceInfo* I2CManager::findDevice(I2CDeviceType type) {
    for (uint8_t i = 0; i < _deviceCount; i++) {
        if (_devices[i].type == type) return &_devices[i];
    }
    return nullptr;
}

uint8_t I2CManager::getPaHubCount() { return _paHubCount; }

const PaHubState* I2CManager::getPaHubState(uint8_t index) {
    if (index >= _paHubCount) return nullptr;
    return &_paHubs[index];
}

// ============================================================================
// Formatted output
// ============================================================================

String I2CManager::getDeviceListFormatted() {
    String result;
    if (_deviceCount == 0) {
        result = "No I2C devices found";
        return result;
    }

    result = String(_deviceCount) + " device(s):\n";
    for (uint8_t i = 0; i < _deviceCount; i++) {
        result += " " + _devices[i].toString() + "\n";
    }
    return result;
}

// ============================================================================
// Low-level I2C helpers
// ============================================================================

bool I2CManager::writeByteToDevice(uint8_t addr, uint8_t reg, uint8_t value) {
    if (!_busActive) return false;
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(value);
    return (Wire.endTransmission() == 0);
}

uint8_t I2CManager::readByteFromDevice(uint8_t addr, uint8_t reg) {
    if (!_busActive) return 0;

    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0;

    if (Wire.requestFrom(addr, (uint8_t)1) != 1) return 0;
    return Wire.read();
}

// ============================================================================
// Config persistence
// ============================================================================

void I2CManager::loadConfig() {
    _enabled = ConfigManager::loadBool("i2c_enabled", false);
    Serial.printf("[I2C] Config loaded: enabled=%d\n", _enabled);
}

void I2CManager::saveConfig() {
    ConfigManager::saveBool("i2c_enabled", _enabled);
}

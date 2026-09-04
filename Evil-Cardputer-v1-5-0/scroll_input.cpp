/**
 * @file scroll_input.cpp
 * @brief M5Stack Scroll Unit driver implementation
 */

#include "scroll_input.h"
#include "i2c_manager.h"
#include "environment_monitor.h"
#include <Wire.h>

// ============================================================================
// Static member initialization
// ============================================================================

uint8_t ScrollInput::_address = 0x40;
ScrollInputState ScrollInput::_state = {};
bool ScrollInput::_initialized = false;
bool ScrollInput::_buttonClickPending = false;

uint8_t ScrollInput::_paHubAddr = 0;
uint8_t ScrollInput::_paHubChannel = 0;
bool ScrollInput::_hasPaHubRoute = false;
bool ScrollInput::_routeLocked = false;
bool ScrollInput::_previousMaskValid = false;
uint8_t ScrollInput::_previousPaHubMask = 0;

// ============================================================================
// Lifecycle
// ============================================================================

bool ScrollInput::init(uint8_t address) {
    if (_initialized) return true;

    _address = address;
    memset(&_state, 0, sizeof(_state));
    _state.connected = false;
    _buttonClickPending = false;

    // Activate PaHub route if needed, then probe
    if (!activatePaHubRoute()) return false;

    // Check if device responds
    Wire.beginTransmission(_address);
    if (Wire.endTransmission() != 0) {
        deactivatePaHubRoute();
        Serial.printf("[Scroll] Device not found at 0x%02X\n", _address);
        return false;
    }

    // Read firmware version
    _state.firmwareVersion = readByte(SCROLL_REG_FW_VERSION);
    _state.connected = true;

    // Reset incremental counter for clean start
    readInt32(SCROLL_REG_INC_ENCODER);

    // Read initial button state
    _state.buttonPressed = (readByte(SCROLL_REG_BUTTON) == 0);
    _state.buttonChanged = false;

    deactivatePaHubRoute();

    _initialized = true;
    Serial.printf("[Scroll] Initialized at 0x%02X, FW v%d\n",
                  _address, _state.firmwareVersion);
    return true;
}

void ScrollInput::shutdown() {
    if (!_initialized) return;
    setLEDOff();
    _initialized = false;
    _state.connected = false;
    Serial.println(F("[Scroll] Shutdown"));
}

bool ScrollInput::isInitialized() { return _initialized; }
bool ScrollInput::isConnected() { return _initialized && _state.connected; }

// ============================================================================
// Polling
// ============================================================================

bool ScrollInput::poll() {
    if (!_initialized || !I2CManager::isEnabled()) return false;

    if (!activatePaHubRoute()) return false;

    // Check device is still responsive
    Wire.beginTransmission(_address);
    if (Wire.endTransmission() != 0) {
        _state.connected = false;
        deactivatePaHubRoute();
        return false;
    }
    _state.connected = true;

    // Read incremental encoder (auto-resets on read)
    _state.encoderDelta = readInt32(SCROLL_REG_INC_ENCODER);

    // Read absolute encoder position
    _state.encoderAbsolute = readInt32(SCROLL_REG_ENCODER);

    // Read button state
    bool prevButton = _state.buttonPressed;
    _state.buttonPressed = (readByte(SCROLL_REG_BUTTON) == 0);
    _state.buttonChanged = (prevButton != _state.buttonPressed);

    if (_state.encoderDelta != 0 || _state.buttonChanged) {
        EnvironmentMonitor::notifyUserActivity();
    }

    // Detect click (press then release)
    if (_state.buttonChanged && !_state.buttonPressed && prevButton) {
        _buttonClickPending = true;
    }

    _state.lastPollMs = millis();

    deactivatePaHubRoute();
    return true;
}

// ============================================================================
// State access
// ============================================================================

const ScrollInputState& ScrollInput::getState() { return _state; }

int32_t ScrollInput::getDelta() {
    int32_t d = _state.encoderDelta;
    _state.encoderDelta = 0;
    return d;
}

bool ScrollInput::isButtonPressed() { return _state.buttonPressed; }

bool ScrollInput::wasButtonClicked() {
    if (_buttonClickPending) {
        _buttonClickPending = false;
        return true;
    }
    return false;
}

// ============================================================================
// Menu navigation helper
// ============================================================================

ScrollEvent ScrollInput::getMenuEvent() {
    if (!_initialized || !_state.connected) return ScrollEvent::None;

    // Button click takes priority
    if (wasButtonClicked()) {
        return ScrollEvent::ButtonClick;
    }

    int32_t delta = getDelta();
    if (delta < 0) return ScrollEvent::ScrollUp;
    if (delta > 0) return ScrollEvent::ScrollDown;

    if (_state.buttonChanged) {
        if (_state.buttonPressed) return ScrollEvent::ButtonPress;
        else return ScrollEvent::ButtonRelease;
    }

    return ScrollEvent::None;
}

// ============================================================================
// Remote Desktop helper
// ============================================================================

int8_t ScrollInput::getScrollDelta() {
    if (!_initialized || !_state.connected) return 0;

    int32_t delta = getDelta();

    // Clamp to int8_t range
    if (delta > 127) delta = 127;
    if (delta < -128) delta = -128;

    return (int8_t)delta;
}

// ============================================================================
// Encoder control
// ============================================================================

bool ScrollInput::resetEncoder() {
    if (!_initialized) return false;
    if (!activatePaHubRoute()) return false;
    bool ok = writeByte(SCROLL_REG_RESET, 1);
    deactivatePaHubRoute();
    return ok;
}

bool ScrollInput::setEncoderValue(int32_t value) {
    if (!_initialized) return false;
    if (!activatePaHubRoute()) return false;

    uint8_t data[4] = {
        (uint8_t)(value & 0xFF),
        (uint8_t)((value >> 8) & 0xFF),
        (uint8_t)((value >> 16) & 0xFF),
        (uint8_t)((value >> 24) & 0xFF)
    };
    bool ok = writeBytes(SCROLL_REG_ENCODER, data, 4);
    deactivatePaHubRoute();
    return ok;
}

// ============================================================================
// LED control
// ============================================================================

bool ScrollInput::setLED(uint8_t r, uint8_t g, uint8_t b) {
    if (!_initialized) return false;
    if (!activatePaHubRoute()) return false;
    uint8_t data[4] = {0, r, g, b};  // LED index 0
    bool ok = writeBytes(SCROLL_REG_RGB_LED, data, 4);
    deactivatePaHubRoute();
    return ok;
}

bool ScrollInput::setLEDOff() {
    return setLED(0, 0, 0);
}

// ============================================================================
// Configuration
// ============================================================================

void ScrollInput::setAddress(uint8_t addr) { _address = addr; }
uint8_t ScrollInput::getAddress() { return _address; }

void ScrollInput::setPaHubRoute(uint8_t hubAddr, uint8_t channel) {
    _paHubAddr = hubAddr;
    _paHubChannel = channel;
    _hasPaHubRoute = true;
}

void ScrollInput::clearPaHubRoute() {
    _hasPaHubRoute = false;
}

// ============================================================================
// PaHub routing
// ============================================================================

bool ScrollInput::activatePaHubRoute() {
    if (_routeLocked) return true;
    if (!I2CManager::lockBus(100)) return false;
    _routeLocked = true;
    _previousMaskValid = false;
    if (!_hasPaHubRoute) return true;
    if (!I2CManager::readPaHubChannelMask(_paHubAddr, _previousPaHubMask) ||
        !I2CManager::setPaHubChannelMask(
            _paHubAddr, static_cast<uint8_t>(1U << _paHubChannel))) {
        I2CManager::unlockBus();
        _routeLocked = false;
        return false;
    }
    _previousMaskValid = true;
    return true;
}

void ScrollInput::deactivatePaHubRoute() {
    if (!_routeLocked) return;
    if (_hasPaHubRoute && _previousMaskValid) {
        I2CManager::setPaHubChannelMask(_paHubAddr, _previousPaHubMask);
    }
    _previousMaskValid = false;
    _routeLocked = false;
    I2CManager::unlockBus();
}

// ============================================================================
// Low-level I2C
// ============================================================================

int32_t ScrollInput::readInt32(uint8_t reg) {
    Wire.beginTransmission(_address);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0;

    if (Wire.requestFrom(_address, (uint8_t)4) != 4) return 0;

    int32_t value = 0;
    value |= (int32_t)Wire.read();
    value |= (int32_t)Wire.read() << 8;
    value |= (int32_t)Wire.read() << 16;
    value |= (int32_t)Wire.read() << 24;

    return value;
}

uint8_t ScrollInput::readByte(uint8_t reg) {
    Wire.beginTransmission(_address);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0;

    if (Wire.requestFrom(_address, (uint8_t)1) != 1) return 0;
    return Wire.read();
}

bool ScrollInput::writeByte(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(_address);
    Wire.write(reg);
    Wire.write(value);
    return (Wire.endTransmission() == 0);
}

bool ScrollInput::writeBytes(uint8_t reg, const uint8_t* data, uint8_t len) {
    Wire.beginTransmission(_address);
    Wire.write(reg);
    for (uint8_t i = 0; i < len; i++) {
        Wire.write(data[i]);
    }
    return (Wire.endTransmission() == 0);
}

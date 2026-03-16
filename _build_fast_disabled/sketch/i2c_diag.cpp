#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\i2c_diag.cpp"
/**
 * @file i2c_diag.cpp
 * @brief I2C bus diagnostic implementation.
 */

#include "i2c_diag.h"
#include <Wire.h>

namespace I2CDiag {

static constexpr int kSdaPin = 8;
static constexpr int kSclPin = 9;

static bool ensureWire() {
  static bool s_begun = false;
  if (!s_begun) {
    Wire.begin(kSdaPin, kSclPin);
    s_begun = true;
  }
  return true;
}

int scanAndLog(const char *tag) {
  if (!ensureWire())
    return -1;

  Serial.printf("[I2C][%s] Scanning bus (SDA=%d SCL=%d)...\n", tag, kSdaPin,
                kSclPin);
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.printf("[I2C][%s]   0x%02X -> OK\n", tag, addr);
      found++;
    }
  }
  Serial.printf("[I2C][%s] Scan complete: %d device(s) found\n", tag, found);
  return found;
}

int countDevices() {
  if (!ensureWire())
    return -1;

  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      found++;
    }
  }
  return found;
}

} // namespace I2CDiag

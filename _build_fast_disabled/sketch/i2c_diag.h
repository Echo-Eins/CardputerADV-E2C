#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\i2c_diag.h"
/**
 * @file i2c_diag.h
 * @brief I2C bus diagnostic utilities.
 *
 * Provides scan/log routines for debugging I2C device availability.
 * Uses Wire (SDA=GPIO8, SCL=GPIO9) — Cardputer ADV default.
 */

#ifndef I2C_DIAG_H
#define I2C_DIAG_H

#include <Arduino.h>

namespace I2CDiag {

/// Scan I2C bus and log all responding addresses to Serial.
/// @param tag Descriptive label for the log lines.
/// @return Number of devices found.
int scanAndLog(const char *tag);

/// Return number of devices on the I2C bus (quick scan, minimal logging).
int countDevices();

} // namespace I2CDiag

#endif // I2C_DIAG_H

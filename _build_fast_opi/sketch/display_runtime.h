#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\display_runtime.h"
/**
 * @file display_runtime.h
 * @brief Runtime display backend switching + SD arbitration hooks.
 */

#ifndef DISPLAY_RUNTIME_H
#define DISPLAY_RUNTIME_H

#include <Arduino.h>
#include <M5Unified.h>
#include "display_config.h"

namespace DisplayRuntime {

bool init();
const char* getLastError();

bool applyActiveProfile(bool restartGuiPipeline = true);
bool applyProfileIndex(uint8_t index,
                       bool persistActive = true,
                       bool restartGuiPipeline = true);

const DisplayProfile* getAppliedProfile();
DisplayDriver getAppliedDriver();
lgfx::LGFX_Device* getActiveDevice();
bool usingExternalDisplay();

void beginSdTransaction();
void endSdTransaction();

class ScopedSdDisplayRelease {
public:
    ScopedSdDisplayRelease() { beginSdTransaction(); }
    ~ScopedSdDisplayRelease() { endSdTransaction(); }
};

}  // namespace DisplayRuntime

#endif  // DISPLAY_RUNTIME_H

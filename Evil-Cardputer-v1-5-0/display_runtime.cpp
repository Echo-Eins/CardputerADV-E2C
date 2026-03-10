/**
 * @file display_runtime.cpp
 * @brief Runtime display backend switching and SD arbitration.
 */

#include "display_runtime.h"
#include "gui/gui.h"
#include "gui/core/gui_display_target.h"
#include "gui/core/gui_display_lock.h"
#include <lgfx/v1/platforms/esp32/Bus_SPI.hpp>
#include <lgfx/v1/panel/Panel_LCD.hpp>
#include <driver/spi_master.h>
#include <cstring>

namespace DisplayRuntime {
namespace {

constexpr size_t kErrorLen = 160;
char s_lastError[kErrorLen] = "";
bool s_initialized = false;
bool s_hasAppliedProfile = false;
DisplayProfile s_appliedProfile;
lgfx::LGFX_Device* s_activeDevice = &M5.Display;

void setError(const String& msg) {
    strncpy(s_lastError, msg.c_str(), kErrorLen - 1);
    s_lastError[kErrorLen - 1] = '\0';
    Serial.printf("[DisplayRuntime] %s\n", s_lastError);
}

spi_host_device_t resolveHost(DisplaySpiHost host) {
    switch (host) {
        case DisplaySpiHost::SPI3: return SPI3_HOST;
        case DisplaySpiHost::SPI2:
        case DisplaySpiHost::AUTO:
        default:
            return SPI2_HOST;
    }
}

bool shouldInitBuiltinFirst(const DisplayProfile& profile) {
    switch (profile.initOrder) {
        case DisplayInitOrder::INTERNAL_FIRST: return true;
        case DisplayInitOrder::EXTERNAL_FIRST: return false;
        case DisplayInitOrder::ACTIVE_FIRST:
        default:
            return profile.builtin;
    }
}

class PanelILI9488_Custom : public lgfx::Panel_LCD {
public:
    PanelILI9488_Custom() {
        _cfg.memory_width = _cfg.panel_width = 320;
        _cfg.memory_height = _cfg.panel_height = 480;
    }

protected:
    const uint8_t* getInitCommands(uint8_t listno) const override {
        static constexpr uint8_t list0[] = {
            0xE0, 15, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78, 0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F,
            0xE1, 15, 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45, 0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F,
            0xC0,  2, 0x17, 0x15,
            0xC1,  1, 0x41,
            0xC5,  3, 0x00, 0x12, 0x80,
            0x36,  1, 0x48,
            0x3A,  1, 0x55,
            0xB0,  1, 0x00,
            0xB1,  1, 0xA0,
            0xB4,  1, 0x02,
            0xB6,  2, 0x02, 0x22,
            0xE9,  1, 0x00,
            0xF7,  4, 0xA9, 0x51, 0x2C, 0x82,
            CMD_SLPOUT, 0 + CMD_INIT_DELAY, 120,
            CMD_DISPON, 0 + CMD_INIT_DELAY, 20,
            0xFF, 0xFF,
        };

        switch (listno) {
            case 0: return list0;
            default: return nullptr;
        }
    }

    uint8_t getMadCtl(uint8_t r) const override {
        static constexpr uint8_t table[] = {
            MAD_MX | MAD_BGR,
            MAD_MV | MAD_MX | MAD_BGR,
            MAD_MY | MAD_BGR,
            MAD_MV | MAD_MY | MAD_BGR,
            MAD_MX | MAD_MY | MAD_BGR,
            MAD_MV | MAD_BGR,
            MAD_BGR,
            MAD_MV | MAD_MX | MAD_MY | MAD_BGR,
        };
        return table[r & 7];
    }
};

class LgfxIli9488Device : public lgfx::LGFX_Device {
public:
    bool configure(const DisplayProfile& profile) {
        auto busCfg = _bus.config();
        busCfg.spi_host = resolveHost(profile.spiHost);
        busCfg.spi_mode = profile.spiMode;
        busCfg.freq_write = profile.freqWrite;
        busCfg.freq_read = profile.freqRead;
        busCfg.spi_3wire = profile.spi3Wire;
        busCfg.dma_channel = profile.dmaChannel >= 0 ? profile.dmaChannel : 0;
        busCfg.use_lock = profile.useLock;
        busCfg.pin_sclk = profile.pins.sclk;
        busCfg.pin_mosi = profile.pins.mosi;
        busCfg.pin_miso = profile.spi3Wire ? -1 : profile.pins.miso;
        busCfg.pin_dc = profile.pins.dc;
        _bus.config(busCfg);

        auto panelCfg = _panel.config();
        panelCfg.pin_cs = profile.pins.cs;
        panelCfg.pin_rst = profile.pins.rst;
        panelCfg.pin_busy = -1;

        uint16_t baseW = profile.width;
        uint16_t baseH = profile.height;
        if (profile.rotation & 0x01) {
            baseW = profile.height;
            baseH = profile.width;
        }
        panelCfg.memory_width = baseW;
        panelCfg.memory_height = baseH;
        panelCfg.panel_width = baseW;
        panelCfg.panel_height = baseH;
        panelCfg.offset_x = 0;
        panelCfg.offset_y = 0;
        panelCfg.offset_rotation = 0;
        panelCfg.readable = (!profile.spi3Wire && profile.pins.miso >= 0);
        panelCfg.bus_shared = profile.busShared;
        panelCfg.invert = false;
        panelCfg.rgb_order = false;
        _panel.config(panelCfg);

        _panel.setBus(&_bus);
        setPanel(&_panel);
        return true;
    }

private:
    lgfx::Bus_SPI _bus;
    PanelILI9488_Custom _panel;
};

LgfxIli9488Device s_externalLgfx;

bool restartGuiForCurrentDisplay() {
    if (!GUI::guiInit()) {
        return false;
    }
    if (!GUI::guiStart()) {
        return false;
    }
    GUI::LegacyBridge::init();
    return true;
}

void fallbackToBuiltin(bool restartGuiPipeline) {
    GUI::resetRuntimeDisplayToBuiltin();
    M5.Display.setRotation(1);
    s_activeDevice = &M5.Display;
    s_hasAppliedProfile = false;
    if (restartGuiPipeline) {
        GUI::guiStop();
        GUI::guiShutdown();
        if (GUI::guiInit()) {
            GUI::guiStart();
            GUI::LegacyBridge::init();
        }
    }
}

bool configureAndInitExternal(const DisplayProfile& profile, DisplayDriver& appliedDriver) {
    appliedDriver = profile.driver;

#if !__has_include(<TFT_eSPI.h>)
    if (appliedDriver == DisplayDriver::TFT_ESPI_ILI9488) {
        Serial.println(F("[DisplayRuntime] TFT_eSPI not present, using lgfx_ili9488 backend"));
        appliedDriver = DisplayDriver::LGFX_ILI9488;
    }
#endif

    if (appliedDriver != DisplayDriver::LGFX_ILI9488 &&
        appliedDriver != DisplayDriver::TFT_ESPI_ILI9488) {
        setError("unsupported external driver");
        return false;
    }

    if (!s_externalLgfx.configure(profile)) {
        setError("external LGFX configure failed");
        return false;
    }
    if (!s_externalLgfx.init()) {
        setError("external LGFX init failed");
        return false;
    }
    s_externalLgfx.setRotation(profile.rotation & 0x07);
    s_externalLgfx.setColorDepth(
        profile.colorDepth == 24
            ? lgfx::color_depth_t::rgb888_3Byte
            : lgfx::color_depth_t::rgb565_2Byte
    );
    return true;
}

bool applyProfileInternal(const DisplayProfile& profile,
                          int8_t indexForPersist,
                          bool persistActive,
                          bool restartGuiPipeline) {
    s_lastError[0] = '\0';

    if (restartGuiPipeline) {
        GUI::guiStop();
        GUI::guiShutdown();
    }

    DisplayDriver appliedDriver = profile.driver;
    lgfx::LGFX_Device* targetDevice = &M5.Display;
    DisplayProfile appliedProfile = profile;

    if (profile.builtin || profile.driver == DisplayDriver::M5_BUILTIN) {
        M5.Display.setRotation(profile.rotation & 0x07);
        targetDevice = &M5.Display;
        appliedDriver = DisplayDriver::M5_BUILTIN;
    } else {
        if (shouldInitBuiltinFirst(profile)) {
            GUI::setRuntimeDisplay(&M5.Display, 16);
            GUI::refreshRuntimeDisplayMetrics();
        }

        if (!configureAndInitExternal(profile, appliedDriver)) {
            fallbackToBuiltin(restartGuiPipeline);
            return false;
        }
        targetDevice = &s_externalLgfx;
    }

    if (!GUI::setRuntimeDisplay(targetDevice, profile.colorDepth)) {
        setError("setRuntimeDisplay failed");
        fallbackToBuiltin(restartGuiPipeline);
        return false;
    }

    GUI::refreshRuntimeDisplayMetrics();
    if (profile.initDelayMs > 0) {
        delay(profile.initDelayMs);
    }

    if (restartGuiPipeline) {
        if (!restartGuiForCurrentDisplay()) {
            setError("GUI restart failed");
            fallbackToBuiltin(true);
            return false;
        }
    } else {
        GUI::LegacyBridge::init();
    }

    appliedProfile.driver = appliedDriver;
    s_appliedProfile = appliedProfile;
    s_activeDevice = targetDevice;
    s_hasAppliedProfile = true;

    if (indexForPersist >= 0) {
        if (!DisplayProfileManager::setActive(static_cast<uint8_t>(indexForPersist), persistActive)) {
            setError(String("profile applied but persist failed: ") + DisplayProfileManager::getLastError());
            return false;
        }
    }

    Serial.printf("[DisplayRuntime] Applied profile '%s' (%s, %dx%d rot=%u)\n",
                  s_appliedProfile.name,
                  displayDriverToString(s_appliedProfile.driver),
                  static_cast<int>(targetDevice->width()),
                  static_cast<int>(targetDevice->height()),
                  static_cast<unsigned>(profile.rotation));
    return true;
}

}  // namespace

bool init() {
    if (s_initialized) return true;
    GUI::setRuntimeDisplay(&M5.Display, 16);
    GUI::refreshRuntimeDisplayMetrics();
    s_activeDevice = &M5.Display;
    s_initialized = true;
    return true;
}

const char* getLastError() {
    return s_lastError;
}

bool applyActiveProfile(bool restartGuiPipeline) {
    if (!init()) return false;
    if (!DisplayProfileManager::isInitialized() && !DisplayProfileManager::init()) {
        setError(String("DisplayProfileManager::init failed: ") + DisplayProfileManager::getLastError());
        return false;
    }

    const int8_t activeIndex = DisplayProfileManager::getActiveIndex();
    const DisplayProfile* profile = DisplayProfileManager::getActiveProfile();
    if (!profile) {
        setError("no active display profile");
        return false;
    }

    return applyProfileInternal(*profile, activeIndex, false, restartGuiPipeline);
}

bool applyProfileIndex(uint8_t index, bool persistActive, bool restartGuiPipeline) {
    if (!init()) return false;

    const DisplayProfile* profile = DisplayProfileManager::getProfile(index);
    if (!profile) {
        setError(String("invalid profile index: ") + index);
        return false;
    }

    return applyProfileInternal(*profile, static_cast<int8_t>(index), persistActive, restartGuiPipeline);
}

const DisplayProfile* getAppliedProfile() {
    return s_hasAppliedProfile ? &s_appliedProfile : nullptr;
}

DisplayDriver getAppliedDriver() {
    return s_hasAppliedProfile ? s_appliedProfile.driver : DisplayDriver::M5_BUILTIN;
}

lgfx::LGFX_Device* getActiveDevice() {
    return s_activeDevice ? s_activeDevice : &M5.Display;
}

bool usingExternalDisplay() {
    if (!s_hasAppliedProfile) return false;
    return !s_appliedProfile.builtin;
}

void beginSdTransaction() {
    if (!s_hasAppliedProfile) return;
    if (!s_appliedProfile.sharesBusWithSd || !s_appliedProfile.releaseBeforeSd) return;

    GUI::LegacyBridge::sync();
    GUI::DisplayLockGuard lockGuard;
    if (lockGuard.locked()) {
        GUI::runtimeDisplay().waitDisplay();
        GUI::runtimeDisplay().endWrite();
    }

    if (s_appliedProfile.pins.cs >= 0) {
        pinMode(s_appliedProfile.pins.cs, OUTPUT);
        digitalWrite(s_appliedProfile.pins.cs, HIGH);
    }
}

void endSdTransaction() {
    // Policy is one-way release-before-SD. No explicit post-op action required.
}

}  // namespace DisplayRuntime

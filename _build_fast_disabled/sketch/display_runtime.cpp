#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\display_runtime.cpp"
/**
 * @file display_runtime.cpp
 * @brief Runtime display backend switching and SD arbitration.
 */

#include "display_runtime.h"
#include "gui/core/gui_display_lock.h"
#include "gui/core/gui_display_target.h"
#include "gui/gui.h"
#include <cstring>
#include <driver/spi_master.h>
#include <lgfx/v1/panel/Panel_LCD.hpp>
#include <lgfx/v1/platforms/esp32/Bus_SPI.hpp>

namespace DisplayRuntime {
namespace {

constexpr size_t kErrorLen = 160;
char s_lastError[kErrorLen] = "";
bool s_initialized = false;
bool s_hasAppliedProfile = false;
DisplayProfile s_appliedProfile;
lgfx::LGFX_Device *s_activeDevice = &M5.Display;
bool s_externalSelfTestDone = false;

void setError(const String &msg) {
  strncpy(s_lastError, msg.c_str(), kErrorLen - 1);
  s_lastError[kErrorLen - 1] = '\0';
  Serial.printf("[DisplayRuntime] %s\n", s_lastError);
}

spi_host_device_t resolveHost(DisplaySpiHost host) {
  switch (host) {
  case DisplaySpiHost::SPI3:
    return SPI3_HOST;
  case DisplaySpiHost::SPI2:
  case DisplaySpiHost::AUTO:
  default:
    return SPI2_HOST;
  }
}

bool shouldInitBuiltinFirst(const DisplayProfile &profile) {
  switch (profile.initOrder) {
  case DisplayInitOrder::INTERNAL_FIRST:
    return true;
  case DisplayInitOrder::EXTERNAL_FIRST:
    return false;
  case DisplayInitOrder::ACTIVE_FIRST:
  default:
    return profile.builtin;
  }
}

class PanelILI9488_Custom : public lgfx::Panel_LCD {
public:
  // Fix #1: Do NOT hardcode panel size here.
  // configure() sets memory_width/height from the active profile.
  PanelILI9488_Custom() = default;

  // Fix #6: ILI9488 does NOT support native RGB565 over SPI.
  // Always use rgb888_3Byte write depth — LovyanGFX will convert
  // RGB565 pixel data into 3-byte RGB888 for the SPI transfer.
  void setColorDepth_impl(lgfx::color_depth_t depth) override {
    (void)depth;
    _write_depth = lgfx::rgb888_3Byte;
    _read_depth = lgfx::rgb888_3Byte;
  }

protected:
  const uint8_t *getInitCommands(uint8_t listno) const override {
    static constexpr uint8_t list0[] = {
        0xC0,
        2,
        0x17,
        0x15, // Power Control 1
        0xC1,
        1,
        0x41, // Power Control 2
        0xC5,
        3,
        0x00,
        0x12,
        0x80, // VCOM Control
        0xB1,
        1,
        0xA0, // Frame Rate Control
        0xB4,
        1,
        0x02, // Display Inversion Control
        0xB6,
        3,
        0x02,
        0x22,
        0x3B, // Display Function Control
        0xB7,
        1,
        0xC6, // Entry Mode Set
        // Fix #6: 0x66 = 18-bit RGB666 pixel format.
        // ILI9488 does NOT natively support 16-bit RGB565 over SPI.
        // LovyanGFX converts pixel data to 3-byte RGB888 for transport.
        0x3A,
        1,
        0x66,
        0xF7,
        4,
        0xA9,
        0x51,
        0x2C,
        0x82, // Adjust Control 3
        CMD_SLPOUT,
        0 + CMD_INIT_DELAY,
        120,
        CMD_IDMOFF,
        0,
        CMD_DISPON,
        0 + CMD_INIT_DELAY,
        100,
        0xFF,
        0xFF,
    };

    switch (listno) {
    case 0:
      return list0;
    default:
      return nullptr;
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
  bool configure(const DisplayProfile &profile) {
    // Release SPI resources before reconfiguring; this is the
    // cross-version-safe API available in M5GFX/LovyanGFX.
    if (_configured) {
      Serial.println(F("[LgfxIli9488] releaseBus() before reconfigure"));
      releaseBus();
    }

    auto busCfg = _bus.config();
    busCfg.spi_host = resolveHost(profile.spiHost);
    busCfg.spi_mode = profile.spiMode;
    busCfg.freq_write = profile.freqWrite;
    busCfg.freq_read = profile.freqRead;
    busCfg.spi_3wire = profile.spi3Wire;
    // Fix #7: Use SPI_DMA_CH_AUTO for reliable auto-selection.
    const int dmaChannel =
        (profile.dmaChannel > 0) ? profile.dmaChannel : static_cast<int>(SPI_DMA_CH_AUTO);
    busCfg.dma_channel = static_cast<uint8_t>(dmaChannel);
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
    panelCfg.dlen_16bit = false;
    panelCfg.dummy_read_pixel = 8;
    panelCfg.dummy_read_bits = 1;
    _panel.config(panelCfg);

    _panel.setBus(&_bus);
    setPanel(&_panel);

    // Fix #9: Log 3-wire SPI diagnostic info
    if (profile.spi3Wire) {
      Serial.printf(
          "[LgfxIli9488] 3-wire SPI mode: MISO forced to -1, readable=false\n");
    }

    _configured = true;
    return true;
  }

private:
  lgfx::Bus_SPI _bus;
  PanelILI9488_Custom _panel;
  bool _configured = false;
};

LgfxIli9488Device s_externalLgfx;

void configureBacklightPin(const DisplayProfile &profile) {
  if (profile.pins.bl < 0) {
    return;
  }
  pinMode(profile.pins.bl, OUTPUT);
  // Current profile schema has no polarity flag; default to active-high.
  digitalWrite(profile.pins.bl, HIGH);
}

void runExternalSelfTestOnce() {
  if (s_externalSelfTestDone) {
    return;
  }
  s_externalSelfTestDone = true;

  GUI::DisplayLockGuard lockGuard;
  if (!lockGuard.locked()) {
    Serial.println(F("[DisplayRuntime] External self-test skipped: display "
                     "lock unavailable"));
    return;
  }

  // One-shot diagnostic pattern to confirm panel/backlight are physically
  // working.
  s_externalLgfx.fillScreen(TFT_RED);
  delay(60);
  s_externalLgfx.fillScreen(TFT_GREEN);
  delay(60);
  s_externalLgfx.fillScreen(TFT_BLUE);
  delay(60);
  s_externalLgfx.fillScreen(TFT_BLACK);
  s_externalLgfx.setTextColor(TFT_WHITE, TFT_BLACK);
  s_externalLgfx.setTextSize(2);
  s_externalLgfx.setCursor(12, 12);
  s_externalLgfx.print("EXT ILI9488 OK");
  s_externalLgfx.display();
  Serial.println(F("[DisplayRuntime] External self-test pattern drawn"));
}

bool restartGuiForCurrentDisplay() {
  if (ESP.getFreePsram() == 0) {
    // Cardputer-ADV units without working PSRAM cannot reliably run the
    // async renderer task + framebuffer path. Keep LegacyBridge in direct
    // mode and report success so profile switch remains deterministic.
    GUI::guiStop();
    GUI::guiShutdown();
    GUI::LegacyBridge::init();
    Serial.println(F("[DisplayRuntime] PSRAM unavailable -> keeping GUI in "
                     "direct mode (async task disabled)"));
    return true;
  }

  Serial.printf("[DisplayRuntime] GUI restart begin (heap=%u psram=%u)\n",
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getFreePsram()));
  if (!GUI::guiInit()) {
    const char *reason = GUI::guiLastError();
    Serial.printf("[DisplayRuntime] GUI restart failed at guiInit: %s\n",
                  (reason && reason[0]) ? reason : "unknown");
    return false;
  }
  if (!GUI::guiStart()) {
    const char *reason = GUI::guiLastError();
    Serial.printf("[DisplayRuntime] GUI restart failed at guiStart: %s\n",
                  (reason && reason[0]) ? reason : "unknown");
    return false;
  }
  GUI::LegacyBridge::init();
  Serial.printf("[DisplayRuntime] GUI restart ok (heap=%u psram=%u)\n",
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getFreePsram()));
  return true;
}

void fallbackToBuiltin(bool restartGuiPipeline) {
  GUI::resetRuntimeDisplayToBuiltin();
  M5.Display.setRotation(1);
  s_activeDevice = &M5.Display;
  s_hasAppliedProfile = false;
  // Fix #2: Reset self-test flag so it runs again when external is
  // re-activated.
  s_externalSelfTestDone = false;
  if (restartGuiPipeline) {
    GUI::guiStop();
    GUI::guiShutdown();
    if (GUI::guiInit()) {
      GUI::guiStart();
      GUI::LegacyBridge::init();
    }
  }
}

bool configureAndInitExternal(const DisplayProfile &profile,
                              DisplayDriver &appliedDriver) {
  appliedDriver = profile.driver;

#if !__has_include(<TFT_eSPI.h>)
  if (appliedDriver == DisplayDriver::TFT_ESPI_ILI9488) {
    Serial.println(
        F("[DisplayRuntime] TFT_eSPI not present, using lgfx_ili9488 backend"));
    appliedDriver = DisplayDriver::LGFX_ILI9488;
  }
#endif

  if (appliedDriver != DisplayDriver::LGFX_ILI9488 &&
      appliedDriver != DisplayDriver::TFT_ESPI_ILI9488) {
    setError("unsupported external driver");
    return false;
  }

  Serial.printf(
      "[DisplayRuntime] External init request: driver=%s host=%s mode=%u "
      "wr=%lu rd=%lu 3wire=%u dma=%d shared=%u lock=%u pins(cs=%d dc=%d rst=%d "
      "mosi=%d sclk=%d miso=%d bl=%d)\n",
      displayDriverToString(appliedDriver),
      displaySpiHostToString(profile.spiHost),
      static_cast<unsigned>(profile.spiMode),
      static_cast<unsigned long>(profile.freqWrite),
      static_cast<unsigned long>(profile.freqRead), profile.spi3Wire ? 1u : 0u,
      static_cast<int>(profile.dmaChannel), profile.busShared ? 1u : 0u,
      profile.useLock ? 1u : 0u, static_cast<int>(profile.pins.cs),
      static_cast<int>(profile.pins.dc), static_cast<int>(profile.pins.rst),
      static_cast<int>(profile.pins.mosi), static_cast<int>(profile.pins.sclk),
      static_cast<int>(profile.pins.miso), static_cast<int>(profile.pins.bl));

  if (!s_externalLgfx.configure(profile)) {
    setError("external LGFX configure failed");
    return false;
  }
  if (!s_externalLgfx.init()) {
    setError("external LGFX init failed");
    return false;
  }
  s_externalLgfx.setRotation(profile.rotation & 0x07);
  s_externalLgfx.setColorDepth(profile.colorDepth == 24
                                   ? lgfx::color_depth_t::rgb888_3Byte
                                   : lgfx::color_depth_t::rgb565_2Byte);
  runExternalSelfTestOnce();
  Serial.printf("[DisplayRuntime] External LGFX init ok: size=%dx%d depth=%u\n",
                static_cast<int>(s_externalLgfx.width()),
                static_cast<int>(s_externalLgfx.height()),
                static_cast<unsigned>(profile.colorDepth));
  return true;
}

bool applyProfileInternal(const DisplayProfile &profile, int8_t indexForPersist,
                          bool persistActive, bool restartGuiPipeline) {
  s_lastError[0] = '\0';

  if (restartGuiPipeline) {
    GUI::guiStop();
    GUI::guiShutdown();
  }

  DisplayDriver appliedDriver = profile.driver;
  lgfx::LGFX_Device *targetDevice = &M5.Display;
  DisplayProfile appliedProfile = profile;

  if (profile.builtin || profile.driver == DisplayDriver::M5_BUILTIN) {
    M5.Display.setRotation(profile.rotation & 0x07);
    targetDevice = &M5.Display;
    appliedDriver = DisplayDriver::M5_BUILTIN;
    // Fix #2: Reset self-test flag so it fires when external is re-activated.
    s_externalSelfTestDone = false;
  } else {
    if (shouldInitBuiltinFirst(profile)) {
      GUI::setRuntimeDisplay(&M5.Display, 16);
      GUI::refreshRuntimeDisplayMetrics();
    }

    if (!configureAndInitExternal(profile, appliedDriver)) {
      fallbackToBuiltin(restartGuiPipeline);
      return false;
    }
    configureBacklightPin(profile);
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

  Serial.printf(
      "[DisplayRuntime] Runtime target ready: name=%s driver=%s builtin=%u "
      "host=%s size=%dx%d depth=%u initOrder=%s initDelay=%u sdShare=%u "
      "releaseBeforeSd=%u\n",
      profile.name, displayDriverToString(appliedDriver),
      profile.builtin ? 1u : 0u, displaySpiHostToString(profile.spiHost),
      static_cast<int>(targetDevice->width()),
      static_cast<int>(targetDevice->height()),
      static_cast<unsigned>(profile.colorDepth),
      displayInitOrderToString(profile.initOrder),
      static_cast<unsigned>(profile.initDelayMs),
      profile.sharesBusWithSd ? 1u : 0u, profile.releaseBeforeSd ? 1u : 0u);

  if (restartGuiPipeline) {
    bool restarted = restartGuiForCurrentDisplay();
    if (!restarted) {
      // One retry after a short delay helps when RTOS resources are still
      // settling right after task teardown.
      GUI::guiStop();
      GUI::guiShutdown();
      delay(20);
      restarted = restartGuiForCurrentDisplay();
    }

    if (!restarted) {
      // Do not fail display switch solely because async GUI task restart
      // failed. LegacyBridge will continue in direct mode when GUI is not
      // running, so the active display still changes deterministically.
      GUI::guiStop();
      GUI::guiShutdown();
      GUI::LegacyBridge::init();
      const char *reason = GUI::guiLastError();
      Serial.printf("[DisplayRuntime] warning: GUI restart failed, continuing "
                    "in direct mode (reason: %s)\n",
                    (reason && reason[0]) ? reason : "unknown");
    }
  } else {
    GUI::LegacyBridge::init();
  }

  appliedProfile.driver = appliedDriver;
  s_appliedProfile = appliedProfile;
  s_activeDevice = targetDevice;
  s_hasAppliedProfile = true;

  if (indexForPersist >= 0) {
    if (!DisplayProfileManager::setActive(static_cast<uint8_t>(indexForPersist),
                                          persistActive)) {
      setError(String("profile applied but persist failed: ") +
               DisplayProfileManager::getLastError());
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

} // namespace

bool init() {
  if (s_initialized)
    return true;
  GUI::setRuntimeDisplay(&M5.Display, 16);
  GUI::refreshRuntimeDisplayMetrics();
  s_activeDevice = &M5.Display;
  s_initialized = true;
  return true;
}

const char *getLastError() { return s_lastError; }

bool applyActiveProfile(bool restartGuiPipeline) {
  if (!init())
    return false;
  if (!DisplayProfileManager::isInitialized() &&
      !DisplayProfileManager::init()) {
    setError(String("DisplayProfileManager::init failed: ") +
             DisplayProfileManager::getLastError());
    return false;
  }

  const int8_t activeIndex = DisplayProfileManager::getActiveIndex();
  const DisplayProfile *profile = DisplayProfileManager::getActiveProfile();
  if (!profile) {
    setError("no active display profile");
    return false;
  }

  return applyProfileInternal(*profile, activeIndex, false, restartGuiPipeline);
}

bool applyProfileIndex(uint8_t index, bool persistActive,
                       bool restartGuiPipeline) {
  if (!init())
    return false;

  const DisplayProfile *profile = DisplayProfileManager::getProfile(index);
  if (!profile) {
    setError(String("invalid profile index: ") + index);
    return false;
  }

  // Selecting the currently active/applied profile should be a no-op.
  if (s_hasAppliedProfile &&
      strcmp(s_appliedProfile.name, profile->name) == 0) {
    if (!DisplayProfileManager::setActive(index, persistActive)) {
      setError(String("profile already active but persist failed: ") +
               DisplayProfileManager::getLastError());
      return false;
    }
    Serial.printf("[DisplayRuntime] Profile '%s' already active\n",
                  profile->name);
    return true;
  }

  return applyProfileInternal(*profile, static_cast<int8_t>(index),
                              persistActive, restartGuiPipeline);
}

const DisplayProfile *getAppliedProfile() {
  return s_hasAppliedProfile ? &s_appliedProfile : nullptr;
}

DisplayDriver getAppliedDriver() {
  return s_hasAppliedProfile ? s_appliedProfile.driver
                             : DisplayDriver::M5_BUILTIN;
}

lgfx::LGFX_Device *getActiveDevice() {
  return s_activeDevice ? s_activeDevice : &M5.Display;
}

bool usingExternalDisplay() {
  if (!s_hasAppliedProfile)
    return false;
  return !s_appliedProfile.builtin;
}

void beginSdTransaction() {
  if (!s_hasAppliedProfile)
    return;
  if (!s_appliedProfile.sharesBusWithSd || !s_appliedProfile.releaseBeforeSd)
    return;

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

} // namespace DisplayRuntime

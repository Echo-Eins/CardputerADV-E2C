/**
 * @file display_runtime.cpp
 * @brief Runtime display backend switching and SD arbitration.
 */

#include "display_runtime.h"
#include "hardware.h"
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
constexpr int8_t kSdCsPin = 12;
constexpr int8_t kFallbackExternalCsPin = 5;
constexpr int8_t kSdSclkPin = 40;
constexpr int8_t kSdMosiPin = 14;
constexpr int8_t kSdMisoPin = 39;
constexpr int8_t kAdvExternalDcPin = 6;
constexpr int8_t kAdvExternalRstPin = 3;
constexpr uint32_t kDefaultExternalWriteHz = 40000000UL;
constexpr uint32_t kMaximumExternalWriteHz = 80000000UL;
uint16_t s_sdTxnDepth = 0;
bool s_sdDisplayLockHeld = false;
constexpr bool kSpiTraceEnabled = true;

DisplayProfile normalizeProfileForHardware(const DisplayProfile &source) {
  DisplayProfile profile = source;
  if (profile.builtin || !hwIsCardputerADV()) {
    return profile;
  }

  // Verified Cardputer ADV + ILI9488 wiring. SDO is physically disconnected;
  // G39 remains configured only as the bus MISO required by the onboard SD.
  profile.spiHost = DisplaySpiHost::SPI2;
  profile.spiMode = 0;
  // Keep the profile frequency selectable. Only repair an invalid value and
  // clamp values beyond the supported tuning range; never silently replace a
  // deliberate 10/20/26.7/40/80 MHz choice from displays.json.
  if (profile.freqWrite == 0) {
    profile.freqWrite = kDefaultExternalWriteHz;
  } else if (profile.freqWrite > kMaximumExternalWriteHz) {
    profile.freqWrite = kMaximumExternalWriteHz;
  }
  profile.freqRead = 0;
  profile.spi3Wire = false;
  profile.busShared = true;
  profile.useLock = true;
  profile.sharesBusWithSd = true;
  profile.releaseBeforeSd = true;
  profile.pins.cs = kFallbackExternalCsPin;
  profile.pins.dc = kAdvExternalDcPin;
  profile.pins.rst = kAdvExternalRstPin;
  profile.pins.mosi = kSdMosiPin;
  profile.pins.sclk = kSdSclkPin;
  profile.pins.miso = kSdMisoPin;
  profile.pins.bl = -1;
  return profile;
}

DisplayProfile makeVerifiedExternalBootProfile() {
  DisplayProfile profile;
  strncpy(profile.name, "external_ili9488", DISPLAY_NAME_MAX_LEN - 1);
  profile.name[DISPLAY_NAME_MAX_LEN - 1] = '\0';
  profile.driver = DisplayDriver::LGFX_ILI9488;
  profile.width = 480;
  profile.height = 320;
  profile.rotation = 3;
  profile.colorDepth = 16;
  profile.builtin = false;
  return normalizeProfileForHardware(profile);
}

void setError(const String &msg) {
  strncpy(s_lastError, msg.c_str(), kErrorLen - 1);
  s_lastError[kErrorLen - 1] = '\0';
  Serial.printf("[DisplayRuntime] %s\n", s_lastError);
}

void drivePinHigh(int8_t pin) {
  if (pin < 0) {
    return;
  }
  pinMode(pin, OUTPUT);
  digitalWrite(pin, HIGH);
}

int readPinLevel(int8_t pin) {
  if (pin < 0) {
    return -1;
  }
  return digitalRead(pin);
}

void logSpiState(const char *tag, const DisplayProfile *profile = nullptr) {
  if (!kSpiTraceEnabled) {
    return;
  }

  int8_t extCsPin = -1;
  int8_t extMisoPin = -1;
  bool hasApplied = s_hasAppliedProfile;
  bool sharesSd = false;
  bool releaseSd = false;

  if (profile) {
    extCsPin = profile->pins.cs;
    extMisoPin = profile->pins.miso;
    sharesSd = profile->sharesBusWithSd;
    releaseSd = profile->releaseBeforeSd;
    hasApplied = true;
  } else if (s_hasAppliedProfile) {
    extCsPin = s_appliedProfile.pins.cs;
    extMisoPin = s_appliedProfile.pins.miso;
    sharesSd = s_appliedProfile.sharesBusWithSd;
    releaseSd = s_appliedProfile.releaseBeforeSd;
  }

  Serial.printf(
      "[DisplayRuntime][SPI] %s | sd_cs(pin=%d lvl=%d) ext_cs(pin=%d lvl=%d) "
      "fallback_cs(pin=%d lvl=%d) sd_miso(pin=%d lvl=%d) ext_miso(pin=%d "
      "lvl=%d) depth=%u applied=%u share=%u release=%u\n",
      tag, static_cast<int>(kSdCsPin), readPinLevel(kSdCsPin),
      static_cast<int>(extCsPin), readPinLevel(extCsPin),
      static_cast<int>(kFallbackExternalCsPin),
      readPinLevel(kFallbackExternalCsPin), static_cast<int>(MISO),
      readPinLevel(MISO), static_cast<int>(extMisoPin),
      readPinLevel(extMisoPin), static_cast<unsigned>(s_sdTxnDepth),
      hasApplied ? 1u : 0u, sharesSd ? 1u : 0u, releaseSd ? 1u : 0u);
}

void deselectSdAndDisplayCsPins() {
  // Keep SD deselected unless an SD transaction explicitly owns the bus.
  drivePinHigh(kSdCsPin);

  // IMPORTANT:
  // Do not reconfigure/toggle external display CS by GPIO after LGFX has
  // attached the pin to the SPI peripheral, otherwise hardware CS ownership
  // can be broken and the panel stays black.
  // Only drive fallback CS before profile apply (boot) or when builtin display
  // is active.
  if (!s_hasAppliedProfile || s_appliedProfile.builtin) {
    drivePinHigh(kFallbackExternalCsPin);
  }
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

  // Match reference implementation: on SPI bus, ILI9488 needs rgb888_3Byte
  // because the chip only supports 18/24-bit pixels over SPI.
  // On parallel bus (if ever used), allow rgb565 when depth <= 16.
  void setColorDepth_impl(lgfx::color_depth_t depth) override {
    _write_depth = (((int)depth & lgfx::color_depth_t::bit_mask) > 16 ||
                    (_bus && _bus->busType() == lgfx::bus_spi))
                       ? lgfx::rgb888_3Byte
                       : lgfx::rgb565_2Byte;
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
        // NOTE: Do NOT send 0x3A (Pixel Format) here!
        // LovyanGFX handles this internally via setColorDepth().
        // Sending it twice (once here, once by LGFX) can cause conflicts.
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

class PersistentSharedSpiBus : public lgfx::Bus_SPI {
public:
  void setPersistent(bool persistent) { _persistent = persistent; }

  bool init(void) override {
    if (_persistent && _initializedOnce) {
      return true;
    }
    const bool ok = lgfx::Bus_SPI::init();
    if (ok) {
      _initializedOnce = true;
    }
    return ok;
  }

  void release(void) override {
    if (_persistent) {
      return;
    }
    lgfx::Bus_SPI::release();
    _initializedOnce = false;
  }

private:
  bool _persistent = false;
  bool _initializedOnce = false;
};

class LgfxIli9488Device : public lgfx::LGFX_Device {
public:
  bool configure(const DisplayProfile &profile) {
    // Release SPI resources before reconfiguring; this is the
    // cross-version-safe API available in M5GFX/LovyanGFX.
    if (_configured) {
      if (profile.sharesBusWithSd) {
        Serial.println(F("[LgfxIli9488] reusing persistent shared SPI2 bus"));
        return true;
      }
      Serial.println(F("[LgfxIli9488] releaseBus() before reconfigure"));
      _bus.setPersistent(false);
      releaseBus();
    }

    _bus.setPersistent(profile.sharesBusWithSd);

    auto busCfg = _bus.config();
    busCfg.spi_host = resolveHost(profile.spiHost);
    busCfg.spi_mode = profile.spiMode;
    busCfg.freq_write = profile.freqWrite;
    busCfg.freq_read = profile.freqRead;
    busCfg.spi_3wire = profile.sharesBusWithSd ? false : profile.spi3Wire;
    // Fix #7: Use SPI_DMA_CH_AUTO for reliable auto-selection.
    const int dmaChannel = (profile.dmaChannel > 0)
                               ? profile.dmaChannel
                               : static_cast<int>(SPI_DMA_CH_AUTO);
    busCfg.dma_channel = static_cast<uint8_t>(dmaChannel);
    busCfg.use_lock = profile.useLock;
    busCfg.pin_sclk = profile.pins.sclk;
    busCfg.pin_mosi = profile.pins.mosi;
    // Shared bus with SD must be write-only from display side to avoid MISO
    // contention. Readback is allowed only on dedicated (non-shared) bus.
    const bool allowReadback = (!profile.sharesBusWithSd && !profile.spi3Wire &&
                                profile.pins.miso >= 0);
    // Keep the physical bus MISO for SD even though the TFT is write-only.
    busCfg.pin_miso = profile.pins.miso;
    if (!allowReadback) {
      busCfg.freq_read = 0;
    }
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
    panelCfg.readable = allowReadback;
    panelCfg.bus_shared = profile.busShared;
    panelCfg.invert = false;
    panelCfg.rgb_order = false;
    panelCfg.dlen_16bit = false;
    panelCfg.dummy_read_pixel = allowReadback ? 8 : 0;
    panelCfg.dummy_read_bits = allowReadback ? 1 : 0;
    _panel.config(panelCfg);

    _panel.setBus(&_bus);
    setPanel(&_panel);

    // Fix #9: Log 3-wire SPI diagnostic info
    Serial.printf("[LgfxIli9488] SPI cfg: 3wire=%u miso=%d readable=%u\n",
                  profile.spi3Wire ? 1u : 0u, static_cast<int>(busCfg.pin_miso),
                  allowReadback ? 1u : 0u);
    if (profile.sharesBusWithSd && !allowReadback) {
      Serial.println(
          F("[LgfxIli9488] readback disabled by policy (shared bus with SD)"));
    }

    _configured = true;
    return true;
  }

  bool prepareSharedBus(const DisplayProfile &profile) {
    if (!configure(profile)) {
      return false;
    }
    return _bus.init();
  }

private:
  PersistentSharedSpiBus _bus;
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

  Serial.println(F("[DisplayRuntime] Starting external self-test..."));

  GUI::DisplayLockGuard lockGuard;
  if (!lockGuard.locked()) {
    Serial.println(F("[DisplayRuntime] External self-test skipped: display "
                     "lock unavailable"));
    return;
  }

  // Wrap all drawing in a single SPI transaction for correct bus handling.
  s_externalLgfx.startWrite();
  Serial.println(F("[DisplayRuntime] self-test: startWrite ok"));

  s_externalLgfx.fillScreen(TFT_RED);
  Serial.println(F("[DisplayRuntime] self-test: RED"));
  delay(120);

  s_externalLgfx.fillScreen(TFT_GREEN);
  Serial.println(F("[DisplayRuntime] self-test: GREEN"));
  delay(120);

  s_externalLgfx.fillScreen(TFT_BLUE);
  Serial.println(F("[DisplayRuntime] self-test: BLUE"));
  delay(120);

  s_externalLgfx.fillScreen(TFT_BLACK);
  s_externalLgfx.setTextColor(TFT_WHITE, TFT_BLACK);
  s_externalLgfx.setTextSize(2);
  s_externalLgfx.setCursor(12, 12);
  s_externalLgfx.print("EXT ILI9488 OK");
  Serial.println(F("[DisplayRuntime] self-test: text drawn"));

  s_externalLgfx.endWrite();
  s_externalLgfx.display();
  s_externalSelfTestDone = true;
  Serial.println(F("[DisplayRuntime] External self-test COMPLETE"));
}

GUI::LowMemoryRenderMode toGuiRenderMode(DisplayCompositorMode mode) {
  switch (mode) {
  case DisplayCompositorMode::SCALED_FULL:
    return GUI::LowMemoryRenderMode::ScaledFull;
  case DisplayCompositorMode::SCALED_TILES:
    return GUI::LowMemoryRenderMode::ScaledTiles;
  case DisplayCompositorMode::AUTO:
    return GUI::LowMemoryRenderMode::Auto;
  case DisplayCompositorMode::DIRECT:
  default:
    return GUI::LowMemoryRenderMode::Direct;
  }
}

bool restartGuiForCurrentDisplay(const DisplayProfile &profile,
                                 lgfx::LGFX_Device *physicalDevice) {
  GUI::lowMemoryCompositor().end();
  if (!profile.builtin &&
      profile.compositorMode != DisplayCompositorMode::DIRECT) {
    GUI::guiStop();
    GUI::guiShutdown();
    GUI::LowMemoryCompositorConfig config;
    config.mode = toGuiRenderMode(profile.compositorMode);
    config.logicalWidth = profile.logicalWidth;
    config.logicalHeight = profile.logicalHeight;
    config.tileSize = profile.tileSize;
    config.fullFlushThreshold = profile.fullFlushThreshold;
    if (GUI::lowMemoryCompositor().begin(physicalDevice, config)) {
      GUI::LegacyBridge::init();
      Serial.printf("[DisplayRuntime] low-memory renderer active: %s "
                    "%ux%u tile=%u threshold=%u%%\n",
                    displayCompositorModeToString(profile.compositorMode),
                    profile.logicalWidth, profile.logicalHeight,
                    profile.tileSize, profile.fullFlushThreshold);
      return true;
    }
    Serial.println(F("[DisplayRuntime] WARNING: low-memory renderer failed; "
                     "falling back to direct output"));
    GUI::restoreRuntimePhysicalDisplay();
  }

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
  GUI::lowMemoryCompositor().end();
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
    if (ESP.getFreePsram() == 0) {
      GUI::LegacyBridge::init();
    } else if (GUI::guiInit()) {
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
  logSpiState("external-init:request", &profile);

  // Ensure SD is deselected before claiming shared SPI lines for display init.
  deselectSdAndDisplayCsPins();
  if (profile.pins.cs >= 0) {
    drivePinHigh(profile.pins.cs);
  }
  logSpiState("external-init:after-deselect", &profile);

  if (!s_externalLgfx.configure(profile)) {
    setError("external LGFX configure failed");
    return false;
  }
  Serial.printf("[DisplayRuntime] LGFX configure ok, calling init()...\n");

  if (!s_externalLgfx.init()) {
    setError("external LGFX init failed");
    logSpiState("external-init:lgfx-init-fail", &profile);
    return false;
  }
  Serial.printf("[DisplayRuntime] LGFX init() returned true (w=%d h=%d)\n",
                static_cast<int>(s_externalLgfx.width()),
                static_cast<int>(s_externalLgfx.height()));

  s_externalLgfx.setRotation(profile.rotation & 0x07);
  Serial.printf("[DisplayRuntime] setRotation(%u) -> w=%d h=%d\n",
                profile.rotation & 0x07u,
                static_cast<int>(s_externalLgfx.width()),
                static_cast<int>(s_externalLgfx.height()));

  // ILI9488 over SPI always needs 24-bit color depth.
  // setColorDepth triggers setColorDepth_impl which forces rgb888_3Byte
  // when bus type is SPI.
  s_externalLgfx.setColorDepth(24);
  Serial.printf("[DisplayRuntime] setColorDepth(24) done\n");

  runExternalSelfTestOnce();
  logSpiState("external-init:success", &profile);
  Serial.printf("[DisplayRuntime] External LGFX init ok: size=%dx%d depth=%u\n",
                static_cast<int>(s_externalLgfx.width()),
                static_cast<int>(s_externalLgfx.height()),
                static_cast<unsigned>(profile.colorDepth));
  return true;
}

bool applyProfileInternal(const DisplayProfile &profile, int8_t indexForPersist,
                          bool persistActive, bool restartGuiPipeline) {
  s_lastError[0] = '\0';

  DisplayProfile appliedProfile = normalizeProfileForHardware(profile);

  if (restartGuiPipeline) {
    GUI::guiStop();
    GUI::guiShutdown();
  }
  GUI::lowMemoryCompositor().end();

  DisplayDriver appliedDriver = appliedProfile.driver;
  lgfx::LGFX_Device *targetDevice = &M5.Display;

  if (appliedProfile.builtin ||
      appliedProfile.driver == DisplayDriver::M5_BUILTIN) {
    M5.Display.setRotation(appliedProfile.rotation & 0x07);
    targetDevice = &M5.Display;
    appliedDriver = DisplayDriver::M5_BUILTIN;
    // Fix #2: Reset self-test flag so it fires when external is re-activated.
    s_externalSelfTestDone = false;
  } else {
    if (shouldInitBuiltinFirst(appliedProfile)) {
      GUI::setRuntimeDisplay(&M5.Display, 16);
      GUI::refreshRuntimeDisplayMetrics();
    }

    if (!configureAndInitExternal(appliedProfile, appliedDriver)) {
      fallbackToBuiltin(restartGuiPipeline);
      return false;
    }
    configureBacklightPin(appliedProfile);
    targetDevice = &s_externalLgfx;
  }

  if (!GUI::setRuntimeDisplay(targetDevice, appliedProfile.colorDepth)) {
    setError("setRuntimeDisplay failed");
    fallbackToBuiltin(restartGuiPipeline);
    return false;
  }

  GUI::refreshRuntimeDisplayMetrics();
  if (appliedProfile.initDelayMs > 0) {
    delay(appliedProfile.initDelayMs);
  }

  Serial.printf(
      "[DisplayRuntime] Runtime target ready: name=%s driver=%s builtin=%u "
      "host=%s size=%dx%d depth=%u initOrder=%s initDelay=%u sdShare=%u "
      "releaseBeforeSd=%u\n",
      appliedProfile.name, displayDriverToString(appliedDriver),
      appliedProfile.builtin ? 1u : 0u,
      displaySpiHostToString(appliedProfile.spiHost),
      static_cast<int>(targetDevice->width()),
      static_cast<int>(targetDevice->height()),
      static_cast<unsigned>(appliedProfile.colorDepth),
      displayInitOrderToString(appliedProfile.initOrder),
      static_cast<unsigned>(appliedProfile.initDelayMs),
      appliedProfile.sharesBusWithSd ? 1u : 0u,
      appliedProfile.releaseBeforeSd ? 1u : 0u);

  if (restartGuiPipeline) {
    bool restarted = restartGuiForCurrentDisplay(appliedProfile, targetDevice);
    if (!restarted) {
      // One retry after a short delay helps when RTOS resources are still
      // settling right after task teardown.
      GUI::guiStop();
      GUI::guiShutdown();
      delay(20);
      restarted = restartGuiForCurrentDisplay(appliedProfile, targetDevice);
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
    restartGuiForCurrentDisplay(appliedProfile, targetDevice);
  }

  appliedProfile.driver = appliedDriver;
  s_appliedProfile = appliedProfile;
  s_activeDevice = targetDevice;
  s_hasAppliedProfile = true;

  if (indexForPersist >= 0) {
    if (!DisplayProfileManager::setActive(static_cast<uint8_t>(indexForPersist),
                                          persistActive)) {
      // Persist failure is non-fatal — display is already visually active.
      // SD write may fail if SPI bus is held by external display (MISO
      // conflict).
      Serial.printf("[DisplayRuntime] WARNING: persist failed: %s "
                    "(display is active, config may not survive reboot)\n",
                    DisplayProfileManager::getLastError());
    }
  }

  Serial.printf("[DisplayRuntime] Applied profile '%s' (%s, %dx%d rot=%u)\n",
                s_appliedProfile.name,
                displayDriverToString(s_appliedProfile.driver),
                static_cast<int>(targetDevice->width()),
                static_cast<int>(targetDevice->height()),
                static_cast<unsigned>(appliedProfile.rotation));
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

void prepareBusForSdBoot() {
  deselectSdAndDisplayCsPins();
  if (hwIsCardputerADV()) {
    const DisplayProfile sharedProfile = makeVerifiedExternalBootProfile();
    if (s_externalLgfx.prepareSharedBus(sharedProfile)) {
      Serial.println(F("[DisplayRuntime] persistent shared SPI2 prepared "
                       "(SCK=40 MOSI=14 MISO=39 SD_CS=12 TFT_CS=5)"));
    } else {
      Serial.println(F("[DisplayRuntime] ERROR: shared SPI2 prepare failed"));
    }
    deselectSdAndDisplayCsPins();
  }
  logSpiState("boot-prepare");
}

void beginSdTransaction() {
  if (s_sdTxnDepth < 0xFFFFu) {
    ++s_sdTxnDepth;
  }
  if (s_sdTxnDepth > 1u) {
    logSpiState("sd-begin:nested");
    return;
  }

  if (!s_hasAppliedProfile || s_appliedProfile.builtin ||
      !s_appliedProfile.sharesBusWithSd || !s_appliedProfile.releaseBeforeSd) {
    logSpiState("sd-begin:policy-skip");
    return;
  }

  GUI::LegacyBridge::sync();
  GUI::lowMemoryCompositor().suspend();
  s_sdDisplayLockHeld = GUI::lockDisplay();
  if (s_sdDisplayLockHeld) {
    GUI::physicalDisplay().waitDisplay();
    GUI::physicalDisplay().endWrite();
  } else {
    Serial.println(F("[DisplayRuntime] WARNING: SD display lock failed"));
  }

  deselectSdAndDisplayCsPins();
  logSpiState("sd-begin:policy-release");
}

void endSdTransaction() {
  if (s_sdTxnDepth == 0u) {
    logSpiState("sd-end:underflow");
    return;
  }
  --s_sdTxnDepth;
  if (s_sdTxnDepth > 0u) {
    logSpiState("sd-end:nested");
    return;
  }

  // Keep SD deselected after transaction. Display CS remains under display
  // driver control and will be asserted during startWrite().
  deselectSdAndDisplayCsPins();
  logSpiState("sd-end:released");
  if (s_sdDisplayLockHeld) {
    s_sdDisplayLockHeld = false;
    GUI::unlockDisplay();
  }
  GUI::lowMemoryCompositor().resume();
}

} // namespace DisplayRuntime

#include <M5Unified.h>
#include <driver/spi_master.h>
#include <lgfx/v1/panel/Panel_LCD.hpp>
#include <lgfx/v1/platforms/esp32/Bus_SPI.hpp>

// Cardputer ADV write-only ILI9488 diagnostic.
//
// This sketch deliberately does not mount SD. It verifies panel power,
// reset, SPI writes, color order, and repeatable full-screen updates without
// involving the shared SD bus.
//
// Wiring:
//   EXT G40 (pin 7)  -> TFT SCK
//   EXT G14 (pin 9)  -> TFT SDI/MOSI
//   EXT G5  (pin 13) -> TFT CS
//   EXT G6  (pin 5)  -> TFT DC/RS
//   EXT G3  (pin 1)  -> TFT RST
//   EXT GND (pin 4)  -> TFT GND
//   EXT 5VOUT (pin 6)-> TFT VCC, only for a 5V-capable module
//   TFT SDO/MISO     -> NOT CONNECTED
//
// The module backlight must be powered according to the module schematic.

namespace {

constexpr int kTftSclk = 40;
constexpr int kTftMosi = 14;
constexpr int kTftCs = 5;
constexpr int kTftDc = 6;
constexpr int kTftRst = 3;
constexpr uint32_t kTftWriteHz = 10000000;

class DiagnosticILI9488Panel : public lgfx::Panel_LCD {
 public:
  void setColorDepth_impl(lgfx::color_depth_t depth) override {
    _write_depth = (((int)depth & lgfx::color_depth_t::bit_mask) > 16 ||
                    (_bus && _bus->busType() == lgfx::bus_spi))
                       ? lgfx::rgb888_3Byte
                       : lgfx::rgb565_2Byte;
    _read_depth = lgfx::rgb888_3Byte;
  }

 protected:
  const uint8_t* getInitCommands(uint8_t listno) const override {
    static constexpr uint8_t list0[] = {
        0xC0, 2, 0x17, 0x15,
        0xC1, 1, 0x41,
        0xC5, 3, 0x00, 0x12, 0x80,
        0xB1, 1, 0xA0,
        0xB4, 1, 0x02,
        0xB6, 3, 0x02, 0x22, 0x3B,
        0xB7, 1, 0xC6,
        0xF7, 4, 0xA9, 0x51, 0x2C, 0x82,
        CMD_SLPOUT, 0 + CMD_INIT_DELAY, 120,
        CMD_IDMOFF, 0,
        CMD_DISPON, 0 + CMD_INIT_DELAY, 100,
        0xFF, 0xFF,
    };
    return listno == 0 ? list0 : nullptr;
  }

  uint8_t getMadCtl(uint8_t rotation) const override {
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
    return table[rotation & 7];
  }
};

class DiagnosticILI9488 : public lgfx::LGFX_Device {
 public:
  DiagnosticILI9488() {
    {
      auto cfg = bus_.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = kTftWriteHz;
      cfg.freq_read = 4000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = kTftSclk;
      cfg.pin_mosi = kTftMosi;
      cfg.pin_miso = -1;
      cfg.pin_dc = kTftDc;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }

    {
      auto cfg = panel_.config();
      cfg.pin_cs = kTftCs;
      cfg.pin_rst = kTftRst;
      cfg.pin_busy = -1;
      cfg.memory_width = 320;
      cfg.memory_height = 480;
      cfg.panel_width = 320;
      cfg.panel_height = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = false;
      cfg.invert = false;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      panel_.config(cfg);
    }

    setPanel(&panel_);
  }

 private:
  lgfx::Bus_SPI bus_;
  DiagnosticILI9488Panel panel_;
};

DiagnosticILI9488 externalDisplay;
uint8_t testPage = 0;
uint32_t nextPageAt = 0;

void printStatus(const char* message) {
  Serial.printf("[ILI9488-DIAG] %s\n", message);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setCursor(4, 8);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.println("ILI9488 diagnostic");
  M5.Display.println(message);
}

void drawColorPage(uint32_t color, const char* label) {
  externalDisplay.fillScreen(color);
  externalDisplay.setTextColor(TFT_WHITE, color);
  externalDisplay.setTextSize(3);
  externalDisplay.setCursor(20, 20);
  externalDisplay.print(label);
  externalDisplay.setTextSize(2);
  externalDisplay.setCursor(20, 70);
  externalDisplay.printf("SPI2 %lu MHz", kTftWriteHz / 1000000UL);
  externalDisplay.setCursor(20, 100);
  externalDisplay.print("SDO disconnected");
  Serial.printf("[ILI9488-DIAG] page=%s size=%dx%d\n", label,
                externalDisplay.width(), externalDisplay.height());
}

void drawPatternPage() {
  const int width = externalDisplay.width();
  const int height = externalDisplay.height();
  externalDisplay.fillScreen(TFT_BLACK);
  for (int x = 0; x < width; x += 40) {
    const uint32_t color = ((x / 40) & 1) ? TFT_WHITE : TFT_YELLOW;
    externalDisplay.fillRect(x, 0, 20, height, color);
  }
  externalDisplay.drawRect(0, 0, width, height, TFT_RED);
  externalDisplay.drawLine(0, 0, width - 1, height - 1, TFT_GREEN);
  externalDisplay.drawLine(width - 1, 0, 0, height - 1, TFT_BLUE);
  externalDisplay.setTextColor(TFT_CYAN, TFT_BLACK);
  externalDisplay.setTextSize(2);
  externalDisplay.setCursor(20, height / 2 - 10);
  externalDisplay.print("ILI9488 WRITE OK");
  Serial.println("[ILI9488-DIAG] page=pattern");
}

void drawNextPage() {
  switch (testPage++ & 3U) {
    case 0:
      drawColorPage(TFT_RED, "RED");
      break;
    case 1:
      drawColorPage(TFT_GREEN, "GREEN");
      break;
    case 2:
      drawColorPage(TFT_BLUE, "BLUE");
      break;
    default:
      drawPatternPage();
      break;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[ILI9488-DIAG] boot");

  auto m5cfg = M5.config();
  M5.begin(m5cfg);
  M5.Display.setRotation(1);
  printStatus("Preparing external panel...");

  // Keep TFT deselected and in reset while its power rail stabilizes.
  pinMode(kTftCs, OUTPUT);
  digitalWrite(kTftCs, HIGH);
  pinMode(kTftRst, OUTPUT);
  digitalWrite(kTftRst, LOW);
  // Preserve the verified working power wiring. No Cardputer GPIO controls
  // VCC or LED in this test: VCC is on 5VOUT and LED is on 5VIN.
  delay(250);
  digitalWrite(kTftRst, HIGH);
  delay(150);

  printStatus("Initializing SPI2 TFT...");
  Serial.printf("[ILI9488-DIAG] pins sck=%d mosi=%d cs=%d dc=%d rst=%d\n",
                kTftSclk, kTftMosi, kTftCs, kTftDc, kTftRst);

  if (!externalDisplay.init()) {
    printStatus("External init FAILED");
    Serial.println("[ILI9488-DIAG] externalDisplay.init() failed");
    return;
  }

  externalDisplay.setRotation(3);
  externalDisplay.setColorDepth(24);
  printStatus("External init OK");
  Serial.printf("[ILI9488-DIAG] ready size=%dx%d\n", externalDisplay.width(),
                externalDisplay.height());
  drawNextPage();
  nextPageAt = millis() + 2000;
}

void loop() {
  M5.update();
  if (nextPageAt != 0 && static_cast<int32_t>(millis() - nextPageAt) >= 0) {
    drawNextPage();
    nextPageAt = millis() + 2000;
  }
  delay(5);
}

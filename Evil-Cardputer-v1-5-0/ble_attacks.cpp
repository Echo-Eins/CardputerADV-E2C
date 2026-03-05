/*
 * ble_attacks.cpp - BLE Attack Module for Evil-Cardputer
 *
 * Contains:
 *   - Wall of Flipper (Flipper Zero detector)
 *   - BLE Name Flood
 *   - Wall of AirTags (Apple AirTag detector)
 *   - FindMyEvil (fake AirTag transmitter)
 */

#include "ble_attacks.h"
#include <M5Unified.h>
#include "M5Cardputer.h"
#include <SD.h>
#include "gui/gui.h"

using LB = GUI::LegacyBridge;

// ============================================================================
// Shared BLE UI Helpers
// ============================================================================

// Draw a colored header bar with title (left) and status text (right)
static void bleDrawHeaderBar(int height, uint16_t bgColor, uint16_t fgColor,
                              const char* title, const char* rightText = nullptr) {
    LB::fillRect(0, 0, 240, height, bgColor);
    LB::setTextSize(1);
    LB::setTextColor(fgColor, bgColor);
    LB::setCursor(6, 4);
    LB::print(title);
    if (rightText) {
        int w = LB::textWidth(rightText);
        LB::setCursor(240 - w - 6, 4);
        LB::print(rightText);
    }
}

// Draw a rounded box with title and 2 scrolling payload lines
static void bleDrawSpamBox(int boxY, int boxH, const char* title,
                            const String lines[2], int head,
                            uint16_t borderColor, uint16_t titleColor,
                            uint16_t textColor, uint16_t bgColor,
                            int maxChars = 0) {
    LB::drawRoundRect(3, boxY, 234, boxH - 4, 5, borderColor);
    LB::setTextSize(1);
    LB::setTextColor(titleColor, bgColor);
    LB::setCursor(10, boxY + 2);
    LB::print(title);
    LB::fillRect(5, boxY + 14, 230, boxH - 20, bgColor);
    LB::setTextColor(textColor, bgColor);
    int lineY = boxY + 18;
    for (int i = 0; i < 2; ++i) {
        int idx = (head - 1 - i + 2) % 2;
        if (lines[idx].length() == 0) continue;
        LB::setCursor(maxChars ? 6 : 10, lineY + i * 12);
        if (maxChars && (int)lines[idx].length() > maxChars) {
            LB::print(lines[idx].substring(0, maxChars));
        } else {
            LB::print(lines[idx]);
        }
    }
}

// Clear a rectangular area and print colored text at (x,y)
static void bleUpdateField(int x, int y, int clearW, int clearH,
                            float textSize, uint16_t color, const char* text) {
    LB::fillRect(x, y, clearW, clearH, TFT_BLACK);
    LB::setCursor(x, y);
    LB::setTextSize(textSize);
    LB::setTextColor(color);
    LB::print(text);
}

// ============================================================================
// BLE State
// ============================================================================
bool isBLEInitialized = false;

void releaseBLE() {
  BLEScan* pBLEScan = BLEDevice::getScan();
  if (pBLEScan) {
    pBLEScan->stop();
  }
  if (!isBLEInitialized) return;

  BLEDevice::deinit();

  static bool released = false;
  if (!released) {
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (err == ESP_OK) {
      Serial.println(F("BLE memory released."));
    } else {
      Serial.printf("BLE release skipped (err=0x%x)\n", err);
    }
    released = true;
  }

  isBLEInitialized = false;
}

void initializeBLEIfNeeded() {
  if (!isBLEInitialized) {
    BLEDevice::init("");
    isBLEInitialized = true;
    Serial.println(F("BLE initialized for scanning."));
  }
}

// ============================================================================
// Wall of Flipper - Constants and Variables
// ============================================================================

// Timing for last Flipper detection
static unsigned long lastFlipperFoundMillis = 0;

// Forbidden packet patterns (BLE spam detection)
static std::vector<ForbiddenPacket> forbiddenPackets = {
  {"4c0007190_______________00_____", "APPLE_DEVICE_POPUP"},
  {"4c000f05c0_____________________", "APPLE_ACTION_MODAL"},
  {"4c00071907_____________________", "APPLE_DEVICE_CONNECT"},
  {"4c0004042a0000000f05c1__604c950", "APPLE_DEVICE_SETUP"},
  {"2cfe___________________________", "ANDROID_DEVICE_CONNECT"},
  {"750000000000000000000000000000_", "SAMSUNG_BUDS_POPUP"},
  {"7500010002000101ff000043_______", "SAMSUNG_WATCH_PAIR"},
  {"0600030080_____________________", "WINDOWS_SWIFT_PAIR"},
  {"ff006db643ce97fe427c___________", "LOVE_TOYS"}
};

// Pattern matcher
static bool matchPattern(const char* pattern, const uint8_t* payload, size_t length) {
  size_t patternLength = strlen(pattern);
  for (size_t i = 0, j = 0; i < patternLength && j < length; i += 2, j++) {
    char byteString[3] = {pattern[i], pattern[i + 1], 0};
    if (byteString[0] == '_' && byteString[1] == '_') continue;
    uint8_t byteValue = strtoul(byteString, nullptr, 16);
    if (payload[j] != byteValue) return false;
  }
  return true;
}

// Check if MAC is already recorded
static bool isMacAddressRecorded(const String& macAddress) {
  File file = SD.open("/evil/WoF.txt", FILE_READ);
  if (!file) return false;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    if (line.indexOf(macAddress) >= 0) {
      file.close();
      return true;
    }
  }
  file.close();
  return false;
}

// Record detected Flipper to SD
static void recordFlipper(const String& name, const String& macAddress, const String& color, bool isValidMac) {
  if (!isMacAddressRecorded(macAddress)) {
    File file = SD.open("/evil/WoF.txt", FILE_APPEND);
    if (file) {
      String status = isValidMac ? " - normal" : " - spoofed";
      file.println(name + " - " + macAddress + " - " + color + status);
      Serial.println("Flipper saved: \n" + name + " - " + macAddress + " - " + color + status);
      file.close();
    }
  }
}

// ===================== UI CONFIG =====================
#ifndef TFT_ORANGE
  #define TFT_ORANGE (uint16_t)0xFD20
#endif
#define WOF_BG              TFT_BLACK
#define WOF_ACCENT          TFT_ORANGE
#define WOF_TEXT            TFT_WHITE
#define WOF_MUTED           TFT_DARKGREY

// Flipper colors
#define WOF_WHITE   TFT_WHITE
#define WOF_BLACK   TFT_BLACK
#define WOF_TRANS   TFT_CYAN

static const int WOF_HDR_H    = 18;
static const int WOF_SPAM_H   = 3 * 12 + 8;
static const int WOF_LIST_Y   = WOF_HDR_H + 2;
static const int WOF_LIST_H   = 135 - WOF_HDR_H - WOF_SPAM_H - 4;
static const int WOF_LINE_H   = 13;
static const int WOF_VISIBLE  = (WOF_LIST_H / WOF_LINE_H);
static const int WOF_MAX      = 24;

static std::vector<WofItem> wofItems;
static int wofCount = 0;

static int wofTop = 0;
static int wofSel = 0;
static unsigned long wofLastHeaderUpdate = 0;
static unsigned long wofLastListUpdate   = 0;
static unsigned long wofLastSpamUpdate   = 0;

static String wofSpam[2];
static int wofSpamHead = 0;

static int wofValidCount = 0;
static int wofSpoofCount = 0;
static unsigned long wofLastActivityMs = 0;

// Helpers
static inline String fitLeft(const String& s, int maxChars) {
  if ((int)s.length() <= maxChars) return s;
  return s.substring(0, maxChars - 2) + "..";
}

static void wofRecountValidity() {
  int ok = 0, sp = 0;
  for (int i = 0; i < wofCount; ++i) {
    if (wofItems[i].valid) ok++;
    else sp++;
  }
  wofValidCount = ok;
  wofSpoofCount = sp;
}

// ===================== UI RENDERING =====================
static void wofDrawHeader(bool force = false) {
  unsigned long now = millis();
  if (!force && (now - wofLastHeaderUpdate) < 250) return;
  wofLastHeaderUpdate = now;
  char buf[40];
  snprintf(buf, sizeof(buf), "OK:%d  SP:%d", wofValidCount, wofSpoofCount);
  bleDrawHeaderBar(WOF_HDR_H, WOF_ACCENT, TFT_BLACK, "Wall of Flippers", buf);
}

static void wofDrawList(bool force = false) {
  unsigned long now = millis();
  if (!force && (now - wofLastListUpdate) < 150) return;
  wofLastListUpdate = now;

  // Purge inactive Flippers (>1.5s)
  unsigned long ms = millis();
  for (int i = 0; i < wofCount; ) {
    if (ms - wofItems[i].lastSeen > 1500) {
      wofItems.erase(wofItems.begin() + i);
      wofCount = (int)wofItems.size();
    } else {
      i++;
    }
  }

  LB::fillRect(0, WOF_LIST_Y, 240, WOF_LIST_H, WOF_BG);
  LB::setTextSize(1.5);
  for (int row = 0; row < WOF_VISIBLE; ++row) {
    int idx = wofTop + row;
    if (idx >= wofCount) break;
    int y = WOF_LIST_Y + row * WOF_LINE_H;

    LB::setTextColor(WOF_TEXT, WOF_BG);

    // Color dot based on Flipper color
    uint16_t dot = WOF_TEXT;
    if (wofItems[idx].color == "White") dot = WOF_WHITE;
    else if (wofItems[idx].color == "Black") dot = WOF_BLACK;
    else if (wofItems[idx].color == "Transp") dot = WOF_TRANS;

    LB::fillCircle(5, y + (WOF_LINE_H / 2), 3, dot);

    // Name (without "Flipper " prefix)
    String dispName = wofItems[idx].name;
    if (dispName.startsWith("Flipper ")) dispName.remove(0, 8);

    String left  = fitLeft(dispName, 14);
    String right = String(wofItems[idx].rssi) + " dBm";

    LB::setCursor(15, y + 2); LB::print(left);
    int w = LB::textWidth(right);
    LB::setCursor(240 - w - 4, y + 2); LB::print(right);
  }
}

static void wofDrawSpamBox(bool force = false) {
  unsigned long now = millis();
  if (!force && (now - wofLastSpamUpdate) < 150) return;
  wofLastSpamUpdate = now;
  bleDrawSpamBox(135 - WOF_SPAM_H, WOF_SPAM_H, "BLE Frames (last 2)",
                  wofSpam, wofSpamHead, WOF_MUTED, WOF_MUTED, WOF_TEXT, WOF_BG);
}

// ===================== DATA/UI BINDING =====================
static void wofResetUI() {
  wofTop = 0; wofSel = 0; wofValidCount = 0; wofSpoofCount = 0;
  wofItems.clear();
  wofItems.reserve(WOF_MAX);
  wofCount = 0;
  for (int i = 0; i < 2; ++i) wofSpam[i] = "";
  wofSpamHead = 0;
  LB::fillScreen(WOF_BG);
  wofDrawHeader(true); wofDrawList(true); wofDrawSpamBox(true);
}

static void wofPushSpam(const char* type) {
  wofSpam[wofSpamHead] = type;
  wofSpamHead = (wofSpamHead + 1) % 2;
  wofDrawSpamBox(true);
}

static int wofFindByMac(const String& mac) {
  for (int i = 0; i < wofCount; ++i) if (wofItems[i].mac == mac) return i;
  return -1;
}

static void wofPushFlipper(const String& name,
                           const String& mac,
                           const String& color,
                           int8_t rssi,
                           bool valid) {
  int idx = wofFindByMac(mac);
  if (idx < 0) {
    if (wofCount >= WOF_MAX) {
      if (!wofItems.empty()) wofItems.erase(wofItems.begin());
    }
    wofItems.emplace_back();
    idx = (int)wofItems.size() - 1;
    wofCount = (int)wofItems.size();
  }
  wofItems[idx].name = name;
  wofItems[idx].mac = mac;
  wofItems[idx].color = color;
  wofItems[idx].rssi = rssi;
  wofItems[idx].valid = valid;
  wofItems[idx].lastSeen = millis();
  wofLastActivityMs = wofItems[idx].lastSeen;
  wofRecountValidity();
  wofDrawHeader(false); wofDrawList(true);
}

// ===================== BLE CALLBACK =====================
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    String deviceColor = "Unknown"; bool isValidMac = false; bool isFlipper = false;
    if (advertisedDevice.isAdvertisingService(BLEUUID("00003082-0000-1000-8000-00805f9b34fb"))) {
      deviceColor = "White"; isFlipper = true;
    } else if (advertisedDevice.isAdvertisingService(BLEUUID("00003081-0000-1000-8000-00805f9b34fb"))) {
      deviceColor = "Black"; isFlipper = true;
    } else if (advertisedDevice.isAdvertisingService(BLEUUID("00003083-0000-1000-8000-00805f9b34fb"))) {
      deviceColor = "Transp"; isFlipper = true;
    }
    if (isFlipper) {
      String macAddress = advertisedDevice.getAddress().toString().c_str();
      if (macAddress.startsWith("80:e1:26") || macAddress.startsWith("80:e1:27") || macAddress.startsWith("0C:FA:22")) {
        isValidMac = true;
      }
      String name = advertisedDevice.getName().c_str();
      wofPushFlipper(name, macAddress, deviceColor, (int8_t)advertisedDevice.getRSSI(), isValidMac);
      recordFlipper(name, macAddress, deviceColor, isValidMac);
      lastFlipperFoundMillis = millis();
    }
    std::string advData = advertisedDevice.getManufacturerData();
    if (!advData.empty()) {
      const uint8_t* payload = reinterpret_cast<const uint8_t*>(advData.data());
      size_t length = advData.length();
      for (auto& packet : forbiddenPackets) {
        if (matchPattern(packet.pattern, payload, length)) {
          wofPushSpam(packet.type);
          break;
        }
      }
    }
  }
};

// ===================== NAVIGATION =====================
static void wofHandleKeys(bool& shouldExit) {
  M5Cardputer.update();
  M5.update();

  if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE) ||
      M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
    shouldExit = true;
    return;
  }

  static unsigned long lastKey = 0;
  const unsigned long rpt = 130;

  if (M5Cardputer.Keyboard.isKeyPressed(';')) {
    if (millis() - lastKey > rpt) {
      if (wofSel > 0) wofSel--;
      if (wofSel < wofTop) wofTop = wofSel;
      wofDrawList(true);
      lastKey = millis();
    }
  } else if (M5Cardputer.Keyboard.isKeyPressed('.')) {
    if (millis() - lastKey > rpt) {
      if (wofSel < max(0, wofCount - 1)) wofSel++;
      if (wofSel >= (wofTop + WOF_VISIBLE))
        wofTop = max(0, wofSel - (WOF_VISIBLE - 1));
      wofDrawList(true);
      lastKey = millis();
    }
  }
}

// ===================== MAIN WOF LOOP =====================
void wallOfFlipper() {
  bool exitRequested = false;
  wofResetUI();
  LB::setTextSize(1.5);
  LB::setTextColor(WOF_TEXT, WOF_BG);
  LB::setCursor(8, WOF_LIST_Y + 4);
  LB::print("Scan BLE en cours...");
  initializeBLEIfNeeded();
  delay(120);
  enterDebounce();
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(), true);
  while (!exitRequested) {
    wofHandleKeys(exitRequested);
    pBLEScan->setActiveScan(true);
    pBLEScan->start(1, false);
    wofDrawList(false);
    wofDrawSpamBox(false);
  }
  releaseBLE();
  waitAndReturnToMenu("Stop detection...");
}


// ============================================================================
// BLE Name Flood
// ============================================================================

String generateRandomBLEName_Full() {
  static const char* ascii = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!@#$%&*-_=+?";
  const int asciiCount = strlen(ascii);

  static const char* emojis[] = {
    "\xF0\x9F\x98\x88", "\xF0\x9F\x92\xA3", "\xE2\x9A\xA1", "\xF0\x9F\x92\x80",
    "\xF0\x9F\x94\xA5", "\xF0\x9F\x91\xBE", "\xF0\x9F\x9A\xA8", "\xF0\x9F\x93\xA1",
    "\xF0\x9F\x94\x9E", "\xF0\x9F\x94\x90", "\xF0\x9F\x92\xA5", "\xF0\x9F\xA7\xA0"
  };
  const int emojiCount = sizeof(emojis) / sizeof(emojis[0]);

  String out; out.reserve(32);
  size_t used = 0;

  while (used < 29) {
    bool pickEmoji = (rand() % 100) < 35;
    if (pickEmoji) {
      const char* e = emojis[rand() % emojiCount];
      size_t eb = strlen(e);
      if (used + eb <= 29) {
        out += e;
        used += eb;
        continue;
      }
    }
    if (used + 1 <= 29) {
      out += ascii[rand() % asciiCount];
      used += 1;
    }
  }
  return out;
}

// ===== UI Helpers =====
static inline void uiHeader(const char* title) {
  LB::fillRect(0, 0, 240, 135, TFT_BLACK);
  LB::setTextSize(2);
  LB::setTextColor(TFT_CYAN);
  int tw = LB::textWidth(title);
  LB::setCursor((240 - tw) / 2, 8);
  LB::println(title);

  LB::fillRect(0, 28, 240, 2, TFT_PURPLE);
  LB::drawRect(6, 34, 228, 88, TFT_PURPLE);

  LB::setTextSize(1);
  LB::setTextColor(TFT_WHITE);
  LB::setCursor(8, 126);
  LB::print("BACKSPACE: exit");
}

static inline void uiStaticLabels(const char* modeText) {
  LB::setTextSize(1.5);
  LB::setTextColor(TFT_WHITE);

  LB::setCursor(14, 40);  LB::print("MODE:");
  LB::setCursor(14, 58);  LB::print("NAME:");
  LB::setCursor(14, 76);  LB::print("ADS :");
  LB::setCursor(14, 94);  LB::print("TIME:");
  LB::setCursor(130, 94); LB::print("RATE:");

  LB::setTextColor(TFT_YELLOW);
  LB::setCursor(62, 40);
  LB::print(modeText);

  LB::fillCircle(220, 20, 4, TFT_RED);
}

static inline void uiUpdateSpeedLabel(int mode) {
  const char* txt[] = { "NORMAL", "TURBO", "SLOW" };
  uint16_t color[] = { TFT_DARKGREY, TFT_RED, TFT_BLUE };
  bleUpdateField(130, 76, 100, 14, 1, color[mode], txt[mode]);
}

// Rainbow wheel for TURBO mode
static inline uint32_t wheel(byte pos) {
  if (pos < 85) return pixels.Color(pos * 3, 255 - pos * 3, 0);
  if (pos < 170) {
    pos -= 85;
    return pixels.Color(255 - pos * 3, 0, pos * 3);
  }
  pos -= 170; return pixels.Color(0, pos * 3, 255 - pos * 3);
}

// ===== BLE NameFlood Main Function =====
void bleNameFloodUI() {
  bool useRandom = confirmPopup("Use RANDOM names?");

  uiHeader("BLE NameFlood");
  uiStaticLabels(useRandom ? "RANDOM" : "SD FILE");
  uiUpdateSpeedLabel(0);

  std::vector<String> bleNames;
  if (!useRandom) {
    const char* filePath = "/evil/ble/names.txt";
    if (!SD.exists("/evil/ble")) SD.mkdir("/evil/ble");
    if (!SD.exists(filePath)) {
      File f = SD.open(filePath, FILE_WRITE);
      if (!f) {
        LB::setTextColor(TFT_RED);
        LB::setCursor(14, 60);
        LB::println("Error creating /evil/ble/names.txt");
        delay(2000); inMenu = true; return;
      }
      f.println("Evil\xF0\x9F\x91\xB9""Beacon"); f.println("\xF0\x9F\x92\x80""HackMyPhone"); f.println("\xF0\x9F\x98\x88""FreeVirusWiFi");
      f.println("\xF0\x9F\x93\xA1""BluetoothPolice"); f.println("\xF0\x9F\x94\xA5""PairedYouLOL"); f.println("\xF0\x9F\x91\xBE""NotASpyDevice");
      f.println("\xF0\x9F\x92\xA3""DeleteSystem32"); f.println("\xF0\x9F\x98\xB1""MomTurnOffTheBLE"); f.println("\xF0\x9F\xA4\x96""SkynetNode42");
      f.println("\xF0\x9F\x94\x9E""AdultBLEOnly"); f.println("\xF0\x9F\xA7\xA0""MindControlBLE"); f.println("\xE2\x9A\xA0\xEF\xB8\x8F""DoNotConnect");
      f.println("\xF0\x9F\x90\x8D""PythonInside"); f.println("\xF0\x9F\x92\xA5""BLEpocalypse"); f.println("\xF0\x9F\xAA\xA6""RIP_Bluetooth");
      f.println("\xF0\x9F\x98\x88""ISeeYourPhone"); f.println("\xF0\x9F\x91\xBD""Area51_Scanner"); f.println("\xF0\x9F\x94\xA5""Sith_Bluetooth");
      f.println("\xF0\x9F\x92\xBE""FirmwareUpdate??"); f.println("\xF0\x9F\x9A\xA8""NSA_Listener");
      f.close();
    }
    File file = SD.open(filePath);
    if (!file) {
      LB::setTextColor(TFT_RED);
      LB::setCursor(14, 60);
      LB::println("SD read error.");
      delay(2000);
      inMenu = true;
      return;
    }
    while (file.available()) {
      String line = file.readStringUntil('\n'); line.trim();
      if (line.length() > 0) bleNames.push_back(line);
    }
    file.close();
    if (bleNames.empty()) {
      LB::setTextColor(TFT_RED);
      LB::setCursor(14, 60);
      LB::println("No names in file!");
      delay(2000);
      inMenu = true;
      return;
    }
  }

  BLEDevice::init("Evil-Cardputer");
  esp_ble_gap_stop_advertising();
  BLEDevice::startAdvertising();
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->setScanResponse(true);

  static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0x20, .adv_int_max = 0x40, .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_RANDOM, .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
  };

  size_t index = 0;
  unsigned long lastSwitch = 0;
  unsigned long startTs = millis();
  unsigned long switchInterval = 55;
  unsigned long pubCount = 0, prevCount = 0, lastRateTs = millis();
  float rate = 0.0f;
  int speedMode = 0;
  uint8_t rainbowPos = 0;
  uint8_t ledState = 0;

  LB::fillCircle(220, 20, 4, TFT_GREEN);

  auto printName = [&](const String & nm) {
    LB::fillRect(62, 56, 166, 16, TFT_BLACK);
    LB::fillRect(62, 74, 60, 16, TFT_BLACK);
    LB::fillRect(50, 92, 70, 16, TFT_BLACK);
    LB::fillRect(170, 92, 50, 16, TFT_BLACK);

    LB::setTextSize(1.5);
    LB::setTextColor(TFT_CYAN);
    LB::setCursor(62, 58);
    String shown = nm; if (shown.length() > 18) shown = shown.substring(0, 18);
    LB::print(shown);

    LB::setTextColor(TFT_GREEN);
    LB::setCursor(62, 76);
    LB::printf("%lu", pubCount);

    unsigned long sec = (millis() - startTs) / 1000UL;
    unsigned int mm = sec / 60U; unsigned int ss = sec % 60U;
    LB::setTextColor(TFT_WHITE);
    LB::setCursor(60, 94);
    char buf[8]; snprintf(buf, sizeof(buf), "%02u:%02u", mm, ss);
    LB::print(buf);

    LB::setTextColor(TFT_YELLOW);
    LB::setCursor(170, 94);
    LB::printf("%.1f", rate);
  };

  String currentName;

  while (!M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
    M5Cardputer.update();
    unsigned long now = millis();

    // ==== SPEED TOGGLES ====
    if (M5Cardputer.Keyboard.isKeyPressed('t')) {
      while (M5Cardputer.Keyboard.isKeyPressed('t')) {
        M5Cardputer.update();
        delay(10);
      }
      speedMode = 1;
      switchInterval = 5;
      uiUpdateSpeedLabel(speedMode);
      delay(150);
    }
    if (M5Cardputer.Keyboard.isKeyPressed('s')) {
      while (M5Cardputer.Keyboard.isKeyPressed('s')) {
        M5Cardputer.update();
        delay(10);
      }
      speedMode = 2;
      switchInterval = 500;
      uiUpdateSpeedLabel(speedMode);
      delay(150);
    }
    if (M5Cardputer.Keyboard.isKeyPressed('n')) {
      while (M5Cardputer.Keyboard.isKeyPressed('n')) {
        M5Cardputer.update();
        delay(10);
      }
      speedMode = 0;
      switchInterval = 55;
      uiUpdateSpeedLabel(speedMode);
      delay(150);
    }

    if (now - lastSwitch >= switchInterval) {
      esp_ble_gap_stop_advertising();
      delay(3);

      if (useRandom) currentName = generateRandomBLEName_Full();
      else {
        currentName = bleNames[index];
        while (currentName.length() > 29) currentName.remove(currentName.length() - 1);
        index = (index + 1) % bleNames.size();
      }

      uint8_t newAddr[6];
      esp_fill_random(newAddr, sizeof(newAddr));
      newAddr[0] = (newAddr[0] & 0x3F) | 0xC0;
      esp_ble_gap_set_rand_addr(newAddr);

      BLEAdvertisementData advData; advData.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT); advData.setName(currentName.c_str());
      pAdvertising->setAdvertisementData(advData);

      BLEAdvertisementData scanData; scanData.setName(currentName.c_str());
      pAdvertising->setScanResponseData(scanData);

      esp_ble_gap_set_device_name(currentName.c_str());
      esp_ble_gap_start_advertising(&adv_params);

      ++pubCount;
      printName(currentName);

      // ==== LED BEHAVIOR ====
      if (ledOn) {
        if (speedMode == 1) {  // TURBO => RAINBOW
          pixels.setPixelColor(0, wheel(rainbowPos++));
          if (rainbowPos == 255) rainbowPos = 0;
        } else {               // NORMAL + SLOW => RGB CYCLE
          switch (ledState) {
            case 0: pixels.setPixelColor(0, pixels.Color(255, 0, 0)); break;
            case 1: pixels.setPixelColor(0, pixels.Color(0, 255, 0)); break;
            case 2: pixels.setPixelColor(0, pixels.Color(0, 0, 255)); break;
          }
          ledState = (ledState + 1) % 3;
        }
        pixels.show();
      }

      lastSwitch = now;
    }

    if (now - lastRateTs >= 1000) {
      rate = (float)(pubCount - prevCount) / ((now - lastRateTs) / 1000.0f);
      prevCount = pubCount;
      lastRateTs = now;
      LB::fillRect(170, 92, 50, 16, TFT_BLACK);
      LB::setTextSize(1.5);
      LB::setTextColor(TFT_YELLOW);
      LB::setCursor(170, 94);
      LB::printf("%.1f", rate);
    }

    delay(1);
  }

  esp_ble_gap_stop_advertising();
  releaseBLE();
  pixels.setPixelColor(0, 0);
  pixels.show();
  LB::fillCircle(220, 20, 4, TFT_RED);
  delay(120);
  waitAndReturnToMenu("BLENameFlood stopped");
}


// ============================================================================
// Wall of AirTags
// ============================================================================

// UI constants
#define AT_BG            TFT_BLACK
#define AT_ACCENT        TFT_LIGHTGREY
#define AT_TEXT          TFT_WHITE
#define AT_MUTED         TFT_DARKGREY

static const int AT_HDR_H    = 18;
static const int AT_SPAM_H   = 3 * 12 + 8;
static const int AT_LIST_Y   = AT_HDR_H + 2;
static const int AT_LIST_H   = 135 - AT_HDR_H - AT_SPAM_H - 4;
static const int AT_LINE_H   = 9;
static const int AT_GAP      = 2;
static const int AT_VISIBLE  = (AT_LIST_H / (AT_LINE_H + AT_GAP));

#ifndef AT_DEBUG
#define AT_DEBUG 1
#endif
#if AT_DEBUG
#define AT_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define AT_LOG(...)
#endif

static std::vector<AtItem> atItems;
static int atCount = 0;

static int atTop = 0;
static int atSel = 0;

static unsigned long atLastListUpdate = 0;
static unsigned long atLastHeaderUpdate = 0;
static unsigned long atLastSpamUpdate = 0;

static String atSpam[2];
static int atSpamHead = 0;

static QueueHandle_t atEvtQueue = nullptr;
static volatile uint32_t atDroppedEvents = 0;

static String determineTrend(int newRSSI, int oldRSSI) {
  int delta = abs(newRSSI - oldRSSI);
  if (delta < 3) return "Stable";
  return (newRSSI > oldRSSI) ? "Closer" : "Farther";
}

static float calculateDistance(int rssi) {
  const int txPower = -59;
  const float n = 2.0f;
  return pow(10.0f, ((float)txPower - (float)rssi) / (10.0f * n));
}

static inline String hexPayloadFromMd(const uint8_t* data, uint8_t len) {
  String out; out.reserve(len * 2 + 1);
  for (int i = 0; i < len; i++) {
    char buf[3]; snprintf(buf, sizeof(buf), "%02X", data[i]);
    out += buf;
  }
  return out;
}

// UI drawing
static void atDrawHeader(bool force = false) {
  unsigned long now = millis();
  if (!force && (now - atLastHeaderUpdate) < 250) return;
  atLastHeaderUpdate = now;
  char buf[24];
  snprintf(buf, sizeof(buf), "Tags:%d", atCount);
  bleDrawHeaderBar(AT_HDR_H, AT_ACCENT, TFT_BLACK, "Wall of AirTags", buf);
}

static void atDrawList(bool force = false) {
  unsigned long now = millis();
  if (!force && (now - atLastListUpdate) < 120) return;
  atLastListUpdate = now;

  // Purge old items (>5s)
  unsigned long ms = millis();
  for (int i = 0; i < atCount;) {
    if (ms - atItems[i].lastSeen > 5000) {
      atItems.erase(atItems.begin() + i);
      atCount = (int)atItems.size();
    } else {
      i++;
    }
  }

  LB::fillRect(0, AT_LIST_Y, 240, AT_LIST_H, AT_BG);
  LB::setTextSize(1);

  for (int row = 0; row < AT_VISIBLE; ++row) {
    int idx = atTop + row;
    if (idx >= atCount) break;

    int y = AT_LIST_Y + row * (AT_LINE_H + AT_GAP);
    LB::setTextColor(AT_TEXT, AT_BG);

    // MAC short
    String macShort = atItems[idx].mac.substring(9);
    LB::setCursor(4, y);
    LB::print(macShort);

    // Distance
    char distBuf[10];
    snprintf(distBuf, sizeof(distBuf), "%.1fm", atItems[idx].distance);
    LB::setCursor(80, y);
    LB::print(distBuf);

    // RSSI
    LB::setCursor(130, y);
    LB::printf("%ddBm", atItems[idx].rssi);

    // Trend
    LB::setCursor(185, y);
    LB::print(atItems[idx].trend.substring(0, 6));
  }
}

static void atDrawSpamBox(bool force = false) {
  unsigned long now = millis();
  if (!force && (now - atLastSpamUpdate) < 150) return;
  atLastSpamUpdate = now;
  bleDrawSpamBox(135 - AT_SPAM_H, AT_SPAM_H, "Last 2 payloads",
                  atSpam, atSpamHead, AT_MUTED, AT_MUTED, AT_TEXT, AT_BG, 36);
}

static void atPushSpam(const String& s) {
  atSpam[atSpamHead] = s;
  atSpamHead = (atSpamHead + 1) % 2;
}

static void atPushItem(const String& mac, int rssi, const String& payload, const String& name, const String& uuid) {
  // Check if exists
  int found = -1;
  for (int i = 0; i < atCount; ++i) {
    if (atItems[i].mac == mac) { found = i; break; }
  }

  if (found < 0) {
    if (atCount >= 24) {
      atItems.erase(atItems.begin());
    }
    AtItem item;
    item.mac = mac;
    item.rssi = rssi;
    item.distance = calculateDistance(rssi);
    item.trend = "New";
    item.payload = payload;
    item.name = name;
    item.uuid = uuid;
    item.lastSeen = millis();
    atItems.push_back(item);
    atCount = (int)atItems.size();
  } else {
    atItems[found].trend = determineTrend(rssi, atItems[found].rssi);
    atItems[found].rssi = rssi;
    atItems[found].distance = calculateDistance(rssi);
    atItems[found].payload = payload;
    atItems[found].name = name;
    atItems[found].uuid = uuid;
    atItems[found].lastSeen = millis();
  }

  atDrawHeader(false);
}

// BLE Callback for AirTags
class AtAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    std::string md = advertisedDevice.getManufacturerData();
    if (md.length() < 4) return;

    const uint8_t* data = (const uint8_t*)md.data();
    // Check for Apple Manufacturer ID (0x004C) and FindMy type (0x12)
    if (data[0] != 0x4C || data[1] != 0x00) return;
    if (md.length() < 6 || data[2] != 0x12) return;

    if (!atEvtQueue) return;

    AtEvent evt;
    strncpy(evt.mac, advertisedDevice.getAddress().toString().c_str(), sizeof(evt.mac) - 1);
    evt.mac[sizeof(evt.mac) - 1] = '\0';
    evt.rssi = advertisedDevice.getRSSI();
    evt.md_len = (md.length() > sizeof(evt.md)) ? sizeof(evt.md) : md.length();
    memcpy(evt.md, md.data(), evt.md_len);

    if (advertisedDevice.haveName()) {
      strncpy(evt.name, advertisedDevice.getName().c_str(), sizeof(evt.name) - 1);
    } else {
      evt.name[0] = '\0';
    }

    if (advertisedDevice.haveServiceUUID()) {
      strncpy(evt.uuid, advertisedDevice.getServiceUUID().toString().c_str(), sizeof(evt.uuid) - 1);
    } else {
      evt.uuid[0] = '\0';
    }

    if (xQueueSend(atEvtQueue, &evt, 0) != pdTRUE) {
      atDroppedEvents++;
    }
  }
};

static void atHandleKeys(bool& shouldExit) {
  M5Cardputer.update();
  M5.update();

  if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE) ||
      M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
    shouldExit = true;
    return;
  }

  static unsigned long lastKey = 0;
  const unsigned long rpt = 130;

  if (M5Cardputer.Keyboard.isKeyPressed(';')) {
    if (millis() - lastKey > rpt) {
      if (atSel > 0) atSel--;
      if (atSel < atTop) atTop = atSel;
      atDrawList(true);
      lastKey = millis();
    }
  } else if (M5Cardputer.Keyboard.isKeyPressed('.')) {
    if (millis() - lastKey > rpt) {
      if (atSel < max(0, atCount - 1)) atSel++;
      if (atSel >= (atTop + AT_VISIBLE))
        atTop = max(0, atSel - (AT_VISIBLE - 1));
      atDrawList(true);
      lastKey = millis();
    }
  }
}

void wallOfAirTags() {
  bool exitRequested = false;
  atTop = 0; atSel = 0;
  atItems.clear();
  atItems.reserve(24);
  atCount = 0;
  atSpam[0] = ""; atSpam[1] = ""; atSpamHead = 0;
  atDroppedEvents = 0;

  if (!atEvtQueue) {
    atEvtQueue = xQueueCreate(16, sizeof(AtEvent));
    if (!atEvtQueue) {
      AT_LOG("[ERR] atEvtQueue create failed\n");
    } else {
      AT_LOG("[OK] atEvtQueue ready\n");
    }
  }

  LB::fillScreen(AT_BG);
  atDrawHeader(true);
  atDrawList(true);
  atDrawSpamBox(true);

  initializeBLEIfNeeded();
  enterDebounce();

  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new AtAdvertisedDeviceCallbacks(), true);
  pBLEScan->setActiveScan(true);

  unsigned long lastStats = millis();

  while (!exitRequested) {
    atHandleKeys(exitRequested);

    pBLEScan->start(1, nullptr, false);

    AtEvent evt;
    int processed = 0;
    while (atEvtQueue && xQueueReceive(atEvtQueue, &evt, 0) == pdTRUE) {
      String mac = String(evt.mac);
      String hexPayload = hexPayloadFromMd(evt.md, evt.md_len);
      String name = (evt.name[0]) ? String(evt.name) : "";
      String uuid = (evt.uuid[0]) ? String(evt.uuid) : "";

      if (uuid.length() == 0) {
        uuid = hexPayload;
      }

      if (name.length()) AT_LOG("[ADV] name=%s\n", name.c_str());
      if (uuid.length()) AT_LOG("[ADV] uuid=%s\n", uuid.c_str());

      atPushItem(mac, evt.rssi, hexPayload, name, uuid);
      atPushSpam(hexPayload);

      processed++;
      if (processed >= 12) break;
    }

    if (millis() - lastStats > 500) {
      lastStats = millis();
      if (processed > 0 || atDroppedEvents > 0) {
        AT_LOG("[BLE] tick processed=%d, dropped=%lu, total=%d\n",
               processed, (unsigned long)atDroppedEvents, atCount);
      }
    }

    atDrawList(false);
    atDrawSpamBox(false);
  }

  waitAndReturnToMenu("Scan stopped...");
}


// ============================================================================
// FindMyEvil
// ============================================================================

// State
static bool        fmTxOn         = false;
static uint32_t    fmRotateMs     = 10000;
static uint32_t    fmLastRot      = 0;
static uint16_t    fmIntMs        = 200;
static esp_power_level_t fmTxPwr  = ESP_PWR_LVL_P3;

static bool        fmModeChosen       = false;
static bool        fmUseFindMy        = false;

static const int FMTAG_MAX = 20;
static std::vector<FakeTag> fmTags;
static int      fmTagCount   = 0;
static int      fmTagIdx     = 0;
static uint32_t fmLastSwitch = 0;
static uint32_t fmSlotMs     = 100;

static esp_ble_adv_params_t fmAdvParams;

// Helpers
static inline void fmGenAdvKeyLab(uint8_t adv_key[28]) {
  for (int i = 0; i < 28; ++i) {
    adv_key[i] = (uint8_t)esp_random();
  }
}

static inline void fmMakeStaticRandom(uint8_t mac[6]) {
  uint32_t r1 = esp_random();
  uint32_t r2 = esp_random();
  mac[0] = 0xC0 | (r1 & 0x3F);
  mac[1] = (r1 >> 8) & 0xFF;
  mac[2] = (r1 >> 16) & 0xFF;
  mac[3] = (r1 >> 24) & 0xFF;
  mac[4] = r2 & 0xFF;
  mac[5] = (r2 >> 8) & 0xFF;
}

static inline void fmMacFromAdvKey(const uint8_t adv_key[28], uint8_t mac[6]) {
  mac[0] = (adv_key[0] & 0x3F) | 0xC0;
  mac[1] = adv_key[1];
  mac[2] = adv_key[2];
  mac[3] = adv_key[3];
  mac[4] = adv_key[4];
  mac[5] = adv_key[5];
}

static uint16_t fmAdvUnitsFromMs(uint16_t ms) {
  if (ms < 200)    ms = 200;
  if (ms > 10000) ms = 10000;
  uint32_t num = (uint32_t)ms * 1000 + 312;
  return (uint16_t)(num / 625);
}

static uint8_t fmBuildRawAdv(const uint8_t adv_key[28], uint8_t* out) {
  uint8_t i = 0;
  out[i++] = 0x1E;    // length
  out[i++] = 0xFF;    // AD type
  out[i++] = 0x4C;    // Apple
  out[i++] = 0x00;
  out[i++] = 0x12;    // type
  out[i++] = 0x19;    // length
  uint8_t state = 0x00;
  out[i++] = state;
  memcpy(&out[i], &adv_key[6], 22);
  i += 22;
  out[i++] = adv_key[0] >> 6;
  out[i++] = 0x00;
  return i;
}

static int fmHexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static bool fmParseHexAdvKey(const char* line, uint8_t adv_key[28]) {
  int count = 0;
  int hi    = -1;

  for (int i = 0; line[i] != '\0' && line[i] != '\r' && line[i] != '\n'; ++i) {
    int v = fmHexNibble(line[i]);
    if (v < 0) continue;

    if (hi < 0) {
      hi = v;
    } else {
      adv_key[count++] = (uint8_t)((hi << 4) | v);
      hi = -1;
      if (count >= 28) break;
    }
  }
  return (count == 28);
}

static bool fmLoadFindMyKeysFromSD() {
  const char* path = "/evil/FindMyEvil_keys.txt";
  if (!SD.exists(path)) {
    AT_LOG("[FM-TX] No SD key file: %s\n", path);
    return false;
  }

  File file = SD.open(path, FILE_READ);
  if (!file) {
    AT_LOG("[FM-TX] Cannot open: %s\n", path);
    return false;
  }

  fmTags.clear();
  int loaded = 0;
  while (file.available() && loaded < FMTAG_MAX) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() < 56) continue;

    FakeTag t;
    if (fmParseHexAdvKey(line.c_str(), t.adv_key)) {
      fmMacFromAdvKey(t.adv_key, t.mac);
      fmTags.push_back(t);
      loaded++;
      AT_LOG("[FM-TX] Loaded key #%d: %02X%02X%02X%02X...\n",
             loaded - 1, t.adv_key[0], t.adv_key[1], t.adv_key[2], t.adv_key[3]);
    }
  }
  file.close();

  fmTagCount = (int)fmTags.size();
  fmTagIdx = 0;
  AT_LOG("[FM-TX] Total loaded: %d keys\n", fmTagCount);
  return (fmTagCount > 0);
}

static void fmStopRawAdv() {
  esp_ble_gap_stop_advertising();
}

static void fmStartTagRaw(const FakeTag& t) {
  fmStopRawAdv();

  esp_ble_gap_set_rand_addr((uint8_t*)t.mac);

  uint8_t raw[31];
  uint8_t len = fmBuildRawAdv(t.adv_key, raw);
  esp_ble_gap_config_adv_data_raw(raw, len);

  memset(&fmAdvParams, 0, sizeof(fmAdvParams));
  uint16_t iu = fmAdvUnitsFromMs(fmIntMs);
  fmAdvParams.adv_int_min       = iu;
  fmAdvParams.adv_int_max       = iu;
  fmAdvParams.adv_type          = ADV_TYPE_NONCONN_IND;
  fmAdvParams.own_addr_type     = BLE_ADDR_TYPE_RANDOM;
  fmAdvParams.channel_map       = ADV_CHNL_ALL;
  fmAdvParams.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;

  vTaskDelay(pdMS_TO_TICKS(5));
  esp_err_t st = esp_ble_gap_start_advertising(&fmAdvParams);

  AT_LOG("[FM-TX] tag #%d start ADV: int=%ums, mac=%02X:%02X:%02X:%02X:%02X:%02X, key=%02X%02X%02X%02X...\n",
         fmTagIdx, fmIntMs,
         t.mac[0], t.mac[1], t.mac[2], t.mac[3], t.mac[4], t.mac[5],
         t.adv_key[0], t.adv_key[1], t.adv_key[2], t.adv_key[3]);
  (void)st;
}

// UI
static void fmDrawHeader(bool on) {
  // Build right-side status: "85% ON" or "85% OFF"
  String batteryLevel = getBatteryLevel();
  String rightText = batteryLevel + "% " + (on ? "ON" : "OFF");
  bleDrawHeaderBar(AT_HDR_H, AT_ACCENT, TFT_BLACK, "FindMyEvil", rightText.c_str());
}

static void fmDrawBody() {
  LB::fillRect(0, AT_LIST_Y, 240, 135 - AT_LIST_Y, AT_BG);
  LB::setTextSize(1);
  LB::setTextColor(AT_TEXT, AT_BG);

  int y = AT_LIST_Y + 2;
  LB::setCursor(8, y);   LB::print("Interval: "); LB::print(fmIntMs); LB::print(" ms");
  y += 12;
  LB::setCursor(8, y);   LB::print("Slot:     "); LB::print(fmSlotMs); LB::print(" ms");
  y += 12;

  LB::setCursor(8, y);
  LB::print("Mode: ");
  LB::print(fmUseFindMy ? "FindMy Keys" : "LAB random");
  y += 12;

  LB::setCursor(8, y);   LB::print("Tags:     "); LB::print(fmTagCount);
  y += 12;

  if (fmTagCount) {
    char line[3 * 8 + 1]; int p = 0;
    for (int i = 0; i < 8; i++) {
      p += snprintf(line + p, sizeof(line) - p, "%02X ", fmTags[fmTagIdx].adv_key[i]);
    }
    LB::setCursor(8, y); LB::print("KEY: "); LB::print(line); y += 14;

    char macs[18];
    snprintf(macs, sizeof(macs), "%02X:%02X:%02X:%02X:%02X:%02X",
             fmTags[fmTagIdx].mac[0], fmTags[fmTagIdx].mac[1], fmTags[fmTagIdx].mac[2],
             fmTags[fmTagIdx].mac[3], fmTags[fmTagIdx].mac[4], fmTags[fmTagIdx].mac[5]);
    LB::setCursor(8, y); LB::print("MAC: "); LB::print(macs); y += 16;
  }

  LB::setTextColor(AT_MUTED, AT_BG);
  LB::setCursor(8, y);   LB::print("[SPACE] ON/OFF  [B] Save mode");
  y += 12;
  LB::setCursor(8, y);   LB::print("[R] rotate KEY  [+]/[-] interval");
  y += 12;
  LB::setCursor(8, y);   LB::print("[N] add tag  [BKSP/ENTER] exit");
}

static void fmRedrawAll() {
  fmDrawHeader(fmTxOn);
  fmDrawBody();
}

void FindMyEvilTx() {
  bool exitRequested = false;

  fmTxOn           = false;
  fmTagCount       = 0;
  fmTagIdx         = 0;
  fmLastSwitch     = 0;
  fmModeChosen     = false;
  fmUseFindMy      = false;
  fmTags.clear();

  initializeBLEIfNeeded();

  LB::fillScreen(AT_BG);
  fmRedrawAll();

  enterDebounce();

  while (!exitRequested) {
    M5Cardputer.update();
    M5.update();

    if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE) ||
        M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
      exitRequested = true;
      continue;
    }

    // Toggle TX
    if (M5Cardputer.Keyboard.isKeyPressed(' ')) {
      if (fmTxOn) {
        fmStopRawAdv();
        fmTxOn = false;
        AT_LOG("[FM-TX] OFF\n");
      } else {
        if (!fmModeChosen) {
          fmUseFindMy = confirmPopup("Use keys from SD?");
          fmModeChosen = true;

          if (fmUseFindMy) {
            if (!fmLoadFindMyKeysFromSD()) {
              fmUseFindMy = false;
              AT_LOG("[FM-TX] fallback to LAB random (no SD keys)\n");
            }
          }
        }

        if (fmTagCount == 0) {
          if (fmUseFindMy) {
            fmUseFindMy = false;
          }

          if (!fmUseFindMy) {
            fmTags.clear();
            fmTags.emplace_back();
            fmGenAdvKeyLab(fmTags[0].adv_key);
            fmMacFromAdvKey(fmTags[0].adv_key, fmTags[0].mac);
            fmTagCount = (int)fmTags.size();
            fmTagIdx   = 0;

            AT_LOG("[FM-TX] LAB tag #0 KEY=%02X%02X%02X%02X...\n",
                   fmTags[0].adv_key[0], fmTags[0].adv_key[1],
                   fmTags[0].adv_key[2], fmTags[0].adv_key[3]);
          }
        }

        if (fmTagCount > 0) {
          fmStartTagRaw(fmTags[fmTagIdx]);
          fmLastSwitch = millis();
          fmTxOn       = true;
        } else {
          AT_LOG("[FM-TX] no tags available, TX not started\n");
        }
      }
      fmRedrawAll();
      delay(160);
    }

    // Add FakeTag (max 20) - LAB only
    if (M5Cardputer.Keyboard.isKeyPressed('n') || M5Cardputer.Keyboard.isKeyPressed('N')) {
      if (fmUseFindMy) {
        AT_LOG("[FM-TX] FindMy mode: 'N' disabled (keys from SD)\n");
      } else {
        while(M5Cardputer.Keyboard.isKeyPressed('n') || M5Cardputer.Keyboard.isKeyPressed('N')){
          M5Cardputer.update();
          delay(50);
        }
        if (fmTagCount < FMTAG_MAX) {
          fmTags.emplace_back();
          fmGenAdvKeyLab(fmTags[fmTagCount].adv_key);
          fmMacFromAdvKey(fmTags[fmTagCount].adv_key, fmTags[fmTagCount].mac);
          AT_LOG("[FM-TX] LAB add tag #%d MAC=%02X:%02X:%02X:%02X:%02X:%02X KEY=%02X%02X%02X%02X...\n",
                 fmTagCount,
                 fmTags[fmTagCount].mac[0], fmTags[fmTagCount].mac[1], fmTags[fmTagCount].mac[2],
                 fmTags[fmTagCount].mac[3], fmTags[fmTagCount].mac[4], fmTags[fmTagCount].mac[5],
                 fmTags[fmTagCount].adv_key[0], fmTags[fmTagCount].adv_key[1],
                 fmTags[fmTagCount].adv_key[2], fmTags[fmTagCount].adv_key[3]);
          fmTagCount = min(fmTagCount + 1, FMTAG_MAX);
          if (fmTxOn) {
            fmTagIdx = fmTagCount - 1;
            fmStartTagRaw(fmTags[fmTagIdx]);
            fmLastSwitch = millis();
          }
          fmRedrawAll();
        } else {
          AT_LOG("[FM-TX] max 20 LAB tags reached\n");
        }
      }
      delay(160);
    }

    // Rotate adv_key (LAB only)
    if (M5Cardputer.Keyboard.isKeyPressed('r') || M5Cardputer.Keyboard.isKeyPressed('R')) {
      if (fmUseFindMy) {
        AT_LOG("[FM-TX] FindMy mode: 'R' disabled (fixed keys from SD)\n");
      } else {
        if (fmTagCount > 0) {
          fmGenAdvKeyLab(fmTags[fmTagIdx].adv_key);
          fmMacFromAdvKey(fmTags[fmTagIdx].adv_key, fmTags[fmTagIdx].mac);
          if (fmTxOn) fmStartTagRaw(fmTags[fmTagIdx]);
          fmRedrawAll();
          AT_LOG("[FM-TX] LAB rotate tag #%d key=%02X%02X%02X%02X...\n",
                 fmTagIdx,
                 fmTags[fmTagIdx].adv_key[0], fmTags[fmTagIdx].adv_key[1],
                 fmTags[fmTagIdx].adv_key[2], fmTags[fmTagIdx].adv_key[3]);
        }
      }
      delay(120);
    }

    // Interval +/-
    if (M5Cardputer.Keyboard.isKeyPressed('+') || M5Cardputer.Keyboard.isKeyPressed('=')) {
      if (fmIntMs < 10000) fmIntMs = (uint16_t)min(10000, (int)fmIntMs + 200);
      if (fmTxOn && fmTagCount) fmStartTagRaw(fmTags[fmTagIdx]);
      fmRedrawAll();
      delay(120);
    }
    if (M5Cardputer.Keyboard.isKeyPressed('-') || M5Cardputer.Keyboard.isKeyPressed('_')) {
      if (fmIntMs > 200) fmIntMs = (uint16_t)max(200, (int)fmIntMs - 200);
      if (fmTxOn && fmTagCount) fmStartTagRaw(fmTags[fmTagIdx]);
      fmRedrawAll();
      delay(120);
    }

    // Standby mode
    if (M5Cardputer.Keyboard.isKeyPressed('b') || M5Cardputer.Keyboard.isKeyPressed('B')) {
      LB::fillScreen(TFT_BLACK);
      LB::setBrightness(10);
      AT_LOG("[FM-TX]Screen in standby mode, reduced brightness\n");
      while(M5Cardputer.Keyboard.isKeyPressed('b') || M5Cardputer.Keyboard.isKeyPressed('B')){
        M5Cardputer.update();
        delay(50);
      }
      while (!M5Cardputer.Keyboard.isChange()) {
        M5Cardputer.update();
        delay(50);
      }

      LB::setBrightness(defaultBrightness);
      fmRedrawAll();
      delay(150);
    }

    // Round-robin between tags if ON and >1
    if (fmTxOn && fmTagCount > 1 && (millis() - fmLastSwitch) >= fmSlotMs) {
      fmLastSwitch = millis();
      fmTagIdx = (fmTagIdx + 1) % fmTagCount;
      fmStartTagRaw(fmTags[fmTagIdx]);
    }

    delay(8);
  }

  if (fmTxOn) fmStopRawAdv();
  waitAndReturnToMenu("FindMyEvil desactivated...");
}

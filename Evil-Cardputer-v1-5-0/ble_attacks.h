/*
 * ble_attacks.h - BLE Attack Module for Evil-Cardputer
 *
 * Contains:
 *   - Wall of Flipper (Flipper Zero detector)
 *   - BLE Name Flood
 *   - Wall of AirTags (Apple AirTag detector)
 *   - FindMyEvil (fake AirTag transmitter)
 */

#ifndef BLE_ATTACKS_H
#define BLE_ATTACKS_H

#include <Arduino.h>
#include <vector>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <freertos/queue.h>
#include "esp_bt.h"
#include "esp_gap_ble_api.h"

// ============================================================================
// Forward declarations for external dependencies (defined in main .ino)
// ============================================================================
extern bool confirmPopup(String message);
extern void waitAndReturnToMenu(String message);
extern void enterDebounce();
extern String getBatteryLevel();
extern bool inMenu;
extern int defaultBrightness;
extern bool ledOn;

// NeoPixel (from main)
#include <Adafruit_NeoPixel.h>
extern Adafruit_NeoPixel pixels;

// ============================================================================
// BLE State (shared)
// ============================================================================
extern bool isBLEInitialized;
void releaseBLE();
void initializeBLEIfNeeded();

// ============================================================================
// Wall of Flipper
// ============================================================================

// Forbidden packet pattern structure
struct ForbiddenPacket {
  const char* pattern;
  const char* type;
};

// WofItem structure for detected Flippers
struct WofItem {
  String name;
  String mac;
  String color;
  int8_t rssi;
  bool valid;
  unsigned long lastSeen;
};

// Public functions
void wallOfFlipper();

// ============================================================================
// BLE Name Flood
// ============================================================================
void bleNameFloodUI();
String generateRandomBLEName_Full();

// ============================================================================
// Wall of AirTags
// ============================================================================

// AtItem structure for detected AirTags
struct AtItem {
  String mac;
  int rssi;
  float distance;
  String trend;
  String payload;
  String name;
  String uuid;
  unsigned long lastSeen;
};

// AtEvent for BLE event queue
typedef struct {
  char mac[18];           // "AA:BB:CC:DD:EE:FF"
  int rssi;
  uint8_t md[32];         // Manufacturer Data (clamped)
  uint8_t md_len;
  char name[32];          // GAP name if present
  char uuid[40];          // Service UUID if present
} AtEvent;

// Public functions
void wallOfAirTags();

// ============================================================================
// FindMyEvil
// ============================================================================

// FakeTag structure for FindMy emulation
struct FakeTag {
  uint8_t adv_key[28];  // advertising key (P-224 compressed)
  uint8_t mac[6];       // Random static derived from adv_key
};

// Public functions
void FindMyEvilTx();

#endif // BLE_ATTACKS_H

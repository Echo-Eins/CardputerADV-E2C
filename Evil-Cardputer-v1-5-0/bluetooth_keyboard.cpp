/*
 * bluetooth_keyboard.cpp - Bluetooth HID Keyboard Module for Evil-Cardputer
 *
 * Allows the Cardputer to act as a Bluetooth HID keyboard
 */

#include "bluetooth_keyboard.h"
#include <M5Cardputer.h>

// ============================================================================
// Global Variables
// ============================================================================

BLEHIDDevice* hid;
BLECharacteristic* keyboardInput;
bool isConnected = false;
bool isBluetoothKeyboardActive = false;

// ============================================================================
// HID Report Map
// ============================================================================

const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01,  // Usage Pg (Generic Desktop)
    0x09, 0x06,  // Usage (Keyboard)
    0xA1, 0x01,  // Collection: (Application)
    0x85, 0x01,  // Report Id (1)
    0x05, 0x07,  //   Usage Pg (Key Codes)
    0x19, 0xE0,  //   Usage Min (224)
    0x29, 0xE7,  //   Usage Max (231)
    0x15, 0x00,  //   Log Min (0)
    0x25, 0x01,  //   Log Max (1)
    0x75, 0x01,  //   Report Size (1)
    0x95, 0x08,  //   Report Count (8)
    0x81, 0x02,  //   Input: (Data, Variable, Absolute)
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x08,  //   Report Size (8)
    0x81, 0x01,  //   Input: (Constant)
    0x95, 0x05,  //   Report Count (5)
    0x75, 0x01,  //   Report Size (1)
    0x05, 0x08,  //   Usage Pg (LEDs)
    0x19, 0x01,  //   Usage Min (1)
    0x29, 0x05,  //   Usage Max (5)
    0x91, 0x02,  //   Output: (Data, Variable, Absolute)
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x03,  //   Report Size (3)
    0x91, 0x01,  //   Output: (Constant)
    0x95, 0x06,  // Report Count (6)
    0x75, 0x08,  // Report Size (8)
    0x15, 0x00,  // Log Min (0)
    0x25, 0xF1,  // Log Max (241)
    0x05, 0x07,  // Usage Pg (Key Codes)
    0x19, 0x00,  // Usage Min (0)
    0x29, 0xf1,  // Usage Max (241)
    0x81, 0x00,  // Input: (Data, Array)
    0xC0         // End Collection
};

// ============================================================================
// Forward declarations
// ============================================================================

void updateBluetoothStatus(bool status);
void displayWaitingForConnection(String deviceName);
void handleKeyboardInput();
void keyboardLoop();

// ============================================================================
// BLE Server Callbacks
// ============================================================================

class MyBLEServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) override {
        Serial.println(F("Client connected to BLE server."));
        isConnected = true;
        updateBluetoothStatus(isConnected);
    }

    void onDisconnect(BLEServer* pServer) override {
        Serial.println(F("Client disconnected from BLE server."));
        isConnected = false;
        updateBluetoothStatus(isConnected);
        cleanupBluetooth();
    }
};

// ============================================================================
// Helper Functions
// ============================================================================

void generateRandomMacAddress(uint8_t* macAddr) {
    for (int i = 0; i < 6; i++) {
        macAddr[i] = random(0, 256);
    }
    macAddr[0] = (macAddr[0] & 0xFC) | 0x02;  // Locally administered address
}

// ============================================================================
// Main Initialization
// ============================================================================

void initBluetoothKeyboard() {
    cleanupBluetooth();

    // Generate a random MAC address
    uint8_t newMacAddr[6];
    generateRandomMacAddress(newMacAddr);

    // Set the new MAC address
    esp_base_mac_addr_set(newMacAddr);

    // Print the new MAC address
    Serial.print(F("New MAC address set: "));
    for (int i = 0; i < 6; i++) {
        Serial.printf("%02X", newMacAddr[i]);
        if (i < 5) Serial.print(F(":"));
    }
    Serial.println();

    M5Cardputer.Display.clear();
    M5Cardputer.Display.setTextColor(menuTextUnFocusedColor);
    M5Cardputer.Display.setCursor(0, 10);
    M5Cardputer.Display.println("Bluetooth device name :");

    String deviceName = getUserInput(false);
    Serial.println("Bluetooth device name selected: " + deviceName);

    // Initialize Bluetooth with user-provided name
    BLEDevice::init(deviceName.c_str());
    Serial.println("Bluetooth device initialized with name: " + deviceName);

    BLEServer *pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyBLEServerCallbacks());
    Serial.println(F("BLE server created and callbacks configured."));

    hid = new BLEHIDDevice(pServer);
    keyboardInput = hid->inputReport(1);
    hid->manufacturer()->setValue("Espressif");
    hid->pnp(0x02, 0x045e, 0x028e, 0x0110);
    hid->hidInfo(0x00, 0x01);
    hid->reportMap((uint8_t*)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
    hid->startServices();
    Serial.println(F("HID services started."));

    BLEAdvertising *pAdvertising = pServer->getAdvertising();
    pAdvertising->setAppearance(HID_KEYBOARD);
    pAdvertising->addServiceUUID(hid->hidService()->getUUID());
    pAdvertising->start();
    Serial.println(F("BLE advertising started."));

    BLESecurity *pSecurity = new BLESecurity();
    pSecurity->setAuthenticationMode(ESP_LE_AUTH_BOND);
    pSecurity->setCapability(ESP_IO_CAP_NONE);
    pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    Serial.println(F("BLE security configured."));

    isBluetoothKeyboardActive = true;
    Serial.println(F("Bluetooth keyboard mode activated."));

    displayWaitingForConnection(deviceName);

    // Main loop for Bluetooth keyboard
    while (isBluetoothKeyboardActive) {
        keyboardLoop();
    }

    waitAndReturnToMenu("Connection stopped");
}

// ============================================================================
// Cleanup
// ============================================================================

void cleanupBluetooth() {
    if (isBluetoothKeyboardActive) {
        Serial.println(F("Disabling Bluetooth..."));
        BLEDevice::deinit();
        isBluetoothKeyboardActive = false;
        Serial.println(F("Bluetooth disabled."));
    }
}

// ============================================================================
// Display Functions
// ============================================================================

void displayWaitingForConnection(String deviceName) {
    M5Cardputer.Display.clear();
    M5Cardputer.Display.setTextColor(TFT_BLUE);
    M5Cardputer.Display.setCursor(0, 10);
    M5Cardputer.Display.println("Waiting on: " + deviceName);

    M5Cardputer.Display.setTextSize(3);
    const char* text = "Waiting";
    int16_t textWidth = M5Cardputer.Display.textWidth(text);
    int16_t textHeight = M5Cardputer.Display.fontHeight();
    int rectWidth = textWidth + 20;
    int rectHeight = textHeight + 20;
    int rectX = (240 - rectWidth) / 2;
    int rectY = (135 - rectHeight) / 2;
    M5Cardputer.Display.drawRoundRect(rectX, rectY, rectWidth, rectHeight, 10, TFT_BLUE);
    M5Cardputer.Display.setTextColor(TFT_BLUE);
    int textX = rectX + (rectWidth - textWidth) / 2;
    int textY = rectY + (rectHeight - textHeight) / 2;
    M5Cardputer.Display.setCursor(textX, textY);
    M5Cardputer.Display.print(text);
}

void updateBluetoothStatus(bool status) {
    M5Cardputer.Display.fillScreen(menuBackgroundColor);
    M5Cardputer.Display.setTextSize(3);
    const char* text = "Connected";
    int16_t textWidth = M5Cardputer.Display.textWidth(text);
    int16_t textHeight = M5Cardputer.Display.fontHeight();
    int rectWidth = textWidth + 20;
    int rectHeight = textHeight + 20;
    int rectX = (240 - rectWidth) / 2;
    int rectY = (135 - rectHeight) / 2;

    if (status) {
        M5Cardputer.Display.drawRoundRect(rectX, rectY, rectWidth, rectHeight, 10, TFT_GREEN);
        M5Cardputer.Display.setTextColor(TFT_GREEN);
        Serial.println(F("Bluetooth status: Connected."));
    } else {
        isBluetoothKeyboardActive = false;
    }

    int textX = rectX + (rectWidth - textWidth) / 2;
    int textY = rectY + (rectHeight - textHeight) / 2;
    M5Cardputer.Display.setCursor(textX, textY);
    M5Cardputer.Display.print(text);
}

// ============================================================================
// Keyboard Input Handling
// ============================================================================

void handleKeyboardInput() {
    if (isConnected && isBluetoothKeyboardActive) {
        uint8_t modifier = 0;
        uint8_t keycode[6] = {0};

        if (M5Cardputer.Keyboard.isPressed()) {
            Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
            int count = 0;
            for (auto i : status.hid_keys) {
                keycode[count] = i;
                count++;
            }

            if (status.ctrl) modifier |= 0x01;
            if (status.shift) modifier |= 0x02;
            if (status.alt) modifier |= 0x04;

            uint8_t report[8] = {modifier, 0, keycode[0], keycode[1], keycode[2], keycode[3], keycode[4], keycode[5]};
            keyboardInput->setValue(report, sizeof(report));
            keyboardInput->notify();
            delay(50);

            // Check for Ctrl + Space to return to menu
            if (status.ctrl && status.space) {
                Serial.println(F("Ctrl + space detected. Returning to menu."));
                cleanupBluetooth();
                return;
            }
        } else {
            uint8_t emptyKeyboardReport[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            keyboardInput->setValue(emptyKeyboardReport, sizeof(emptyKeyboardReport));
            keyboardInput->notify();
        }
    }
}

void keyboardLoop() {
    M5Cardputer.update();
    handleKeyboardInput();
    delay(10);
}

#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\bluetooth_keyboard.h"
/*
 * bluetooth_keyboard.h - Bluetooth HID Keyboard Module for Evil-Cardputer
 *
 * Allows the Cardputer to act as a Bluetooth HID keyboard
 */

#ifndef BLUETOOTH_KEYBOARD_H
#define BLUETOOTH_KEYBOARD_H

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include "BLEHIDDevice.h"
#include "HIDTypes.h"

// ============================================================================
// External dependencies from main file
// ============================================================================

extern void waitAndReturnToMenu(String message);
extern String getUserInput(bool isPassword);
extern int menuTextUnFocusedColor;
extern int menuBackgroundColor;

// ============================================================================
// Bluetooth Keyboard State (accessible from main)
// ============================================================================

extern BLEHIDDevice* hid;
extern BLECharacteristic* keyboardInput;
extern bool isConnected;
extern bool isBluetoothKeyboardActive;

// ============================================================================
// Public Functions
// ============================================================================

// Main entry point - called from menu
void initBluetoothKeyboard();

// Cleanup function
void cleanupBluetooth();

#endif // BLUETOOTH_KEYBOARD_H

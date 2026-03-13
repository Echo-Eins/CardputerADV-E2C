#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\screens\\example_integration.h"
/**
 * @file example_integration.h
 * @brief Example integration of MenuScreen and SettingsScreen
 *
 * This file shows how to integrate the new GUI screens
 * into the main Evil-Cardputer application.
 *
 * USAGE:
 * ------
 * 1. Include this header in your main .ino file
 * 2. Call guiScreensInit() in setup() after M5.begin()
 * 3. Use drawMenuGUI() instead of drawMenu() for rendering
 * 4. Use showSettingsGUI() instead of showSettingsMenu()
 *
 * MIGRATION PATH:
 * ---------------
 * The new screens work alongside existing code through:
 * - Signal/Slot callbacks that map to existing functions
 * - Same keyboard navigation (;/. for up/down, Enter, Backspace)
 * - Theme colors from existing global variables
 */

#ifndef EXAMPLE_INTEGRATION_H
#define EXAMPLE_INTEGRATION_H

#include "gui_menu_screen.h"
#include "gui_settings_screen.h"

// Forward declare external functions from main .ino
extern void executeMenuItem(int index);
extern void brightness();
extern void toggleSound();
extern void toggleLED();
extern bool soundOn;
extern bool ledOn;
extern int brightnessValue;

namespace GUI {

//=============================================================================
// Global Screen Instances
//=============================================================================

// Menu screen instance (lazy initialization)
static MenuScreen* g_menuScreen = nullptr;

// Settings screen instance (lazy initialization)
static SettingsScreen* g_settingsScreen = nullptr;

//=============================================================================
// Initialization
//=============================================================================

/**
 * @brief Initialize GUI screens
 *
 * Call this in setup() after M5.begin() and GUI::begin()
 *
 * @param menuItems Array of menu item strings (from main .ino)
 * @param itemCount Number of menu items
 */
inline void guiScreensInit(const char* const* menuItems, size_t itemCount) {
    // Initialize GUI system if not already done
    if (!GUI::renderer().isRunning()) {
        GUI::begin();
    }

    // Create menu screen
    if (!g_menuScreen) {
        g_menuScreen = new MenuScreen(menuItems, itemCount);

        // Set bounds to full display (240x135 for Cardputer)
        g_menuScreen->setBounds(0, 0, 240, 135);

        // Connect item activation to existing executeMenuItem
        g_menuScreen->onItemActivated([](const Event& e) {
            int realIndex = e.data.value.newValue;
            if (realIndex >= 0) {
                executeMenuItem(realIndex);
            }
        });
    }

    // Create settings screen
    if (!g_settingsScreen) {
        g_settingsScreen = new SettingsScreen("Settings");
        g_settingsScreen->setBounds(0, 0, 240, 135);

        // Add settings (example - adapt to your actual settings)
        g_settingsScreen->addAction("Brightness", brightness);
        g_settingsScreen->addToggle(soundOn ? "Disable Sound" : "Enable Sound",
                                    &soundOn, toggleSound);
        g_settingsScreen->addToggle(ledOn ? "Disable LED" : "Enable LED",
                                    &ledOn, toggleLED);

        // Add more settings as needed...
        // g_settingsScreen->addSlider("Volume", &volumeValue, 0, 100, 5);
        // g_settingsScreen->addSelection("Theme", &themeIndex, themeOptions, 5);
    }
}

/**
 * @brief Cleanup GUI screens
 *
 * Call this in cleanup or before deep sleep
 */
inline void guiScreensCleanup() {
    if (g_menuScreen) {
        delete g_menuScreen;
        g_menuScreen = nullptr;
    }
    if (g_settingsScreen) {
        delete g_settingsScreen;
        g_settingsScreen = nullptr;
    }
}

//=============================================================================
// Menu Screen Functions
//=============================================================================

/**
 * @brief Draw the main menu using new GUI system
 *
 * Replacement for drawMenu() in main .ino
 */
inline void drawMenuGUI() {
    if (g_menuScreen) {
        g_menuScreen->render();
        Draw::endFrame();
    }
}

/**
 * @brief Handle key press in menu
 *
 * @param key Key code
 * @param modifiers Modifier flags
 * @return true if key was handled
 */
inline bool handleMenuKey(char key, uint8_t modifiers = 0) {
    if (g_menuScreen) {
        return g_menuScreen->onKeyPress(key, modifiers);
    }
    return false;
}

/**
 * @brief Get current selected menu index (real index, not view index)
 */
inline int getMenuSelectedIndex() {
    if (g_menuScreen) {
        return g_menuScreen->selectedRealIndex();
    }
    return -1;
}

/**
 * @brief Update menu taskbar (time, battery, wifi)
 */
inline void updateMenuTaskbar(const char* time, uint8_t battery, bool wifi) {
    if (g_menuScreen) {
        g_menuScreen->setTime(time);
        g_menuScreen->setBatteryLevel(battery);
        g_menuScreen->setWifiConnected(wifi);
    }
}

/**
 * @brief Enter menu search mode
 */
inline void enterMenuSearch() {
    if (g_menuScreen) {
        g_menuScreen->enterSearchMode();
    }
}

/**
 * @brief Exit menu search mode
 */
inline void exitMenuSearch() {
    if (g_menuScreen) {
        g_menuScreen->exitSearchMode();
    }
}

/**
 * @brief Check if menu is in search mode
 */
inline bool isMenuSearchMode() {
    if (g_menuScreen) {
        return g_menuScreen->isSearchMode();
    }
    return false;
}

//=============================================================================
// Settings Screen Functions
//=============================================================================

/**
 * @brief Show settings screen using new GUI system
 *
 * Replacement for showSettingsMenu() in main .ino
 * This is a blocking function that returns when user exits settings.
 */
inline void showSettingsGUI() {
    if (!g_settingsScreen) return;

    bool exitSettings = false;

    // Connect back handler
    SlotId backSlot = g_settingsScreen->onBack([&exitSettings](const Event&) {
        exitSettings = true;
    });

    // Main loop for settings
    while (!exitSettings) {
        M5.update();
        M5Cardputer.update();

        // Handle keyboard
        if (M5Cardputer.Keyboard.isChange()) {
            if (M5Cardputer.Keyboard.isPressed()) {
                Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
                for (size_t i = 0; i < keys.word.length(); i++) {
                    char key = keys.word[i];
                    g_settingsScreen->onKeyPress(key, 0);
                }

                // Check for special keys
                if (keys.del) {
                    g_settingsScreen->onKeyPress('\b', 0);
                }
                if (keys.enter) {
                    g_settingsScreen->onKeyPress('\n', 0);
                }
            }
        }

        // Render
        g_settingsScreen->render();
        Draw::endFrame();

        delay(10);
    }

    // Disconnect back handler
    g_settingsScreen->signal().disconnect(backSlot);
}

/**
 * @brief Add a setting to the settings screen
 *
 * Can be called after guiScreensInit() to add custom settings
 */
inline void addSetting(const SettingItem& item) {
    if (g_settingsScreen) {
        g_settingsScreen->addSetting(item);
    }
}

/**
 * @brief Clear and rebuild settings
 *
 * Call this when settings need to be refreshed (e.g., after state change)
 */
inline void rebuildSettings() {
    if (g_settingsScreen) {
        g_settingsScreen->clearSettings();

        // Re-add all settings
        g_settingsScreen->addAction("Brightness", brightness);
        g_settingsScreen->addToggle(soundOn ? "Disable Sound" : "Enable Sound",
                                    &soundOn, toggleSound);
        g_settingsScreen->addToggle(ledOn ? "Disable LED" : "Enable LED",
                                    &ledOn, toggleLED);
        // ... add more settings
    }
}

//=============================================================================
// Example Main Loop Integration
//=============================================================================

/**
 * EXAMPLE: How to use in main .ino loop
 *
 * void loop() {
 *     M5.update();
 *     M5Cardputer.update();
 *
 *     if (inMenu) {
 *         // Handle keyboard
 *         if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
 *             Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
 *
 *             // Navigation keys
 *             if (keys.word.length() > 0) {
 *                 char key = keys.word[0];
 *                 if (!GUI::handleMenuKey(key, 0)) {
 *                     // Key not handled by menu, process normally
 *                 }
 *             }
 *
 *             // Special keys
 *             if (keys.del) {
 *                 GUI::handleMenuKey('\b', 0);
 *             }
 *             if (keys.enter) {
 *                 // Enter handled by menu's onItemActivated callback
 *             }
 *         }
 *
 *         // Update taskbar periodically
 *         static unsigned long lastTaskbarUpdate = 0;
 *         if (millis() - lastTaskbarUpdate > 1000) {
 *             lastTaskbarUpdate = millis();
 *             char timeStr[8];
 *             // Format time...
 *             GUI::updateMenuTaskbar(timeStr, getBatteryLevel(), WiFi.status() == WL_CONNECTED);
 *         }
 *
 *         // Render menu
 *         GUI::drawMenuGUI();
 *     }
 * }
 */

} // namespace GUI

#endif // EXAMPLE_INTEGRATION_H

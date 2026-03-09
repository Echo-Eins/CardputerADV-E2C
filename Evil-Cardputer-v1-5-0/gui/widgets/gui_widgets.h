/**
 * @file gui_widgets.h
 * @brief Main header for the widget system
 *
 * Include this file to use the complete widget system:
 * - Signal/Slot event handling
 * - Base Widget class with dirty flags and lazy rendering
 * - Concrete widgets (Label, Button, Input, ListView, etc.)
 * - WidgetManager for lifecycle and input routing
 * - DisplayAdapter for video driver integration
 * - WidgetFactory for easy widget creation
 *
 * Example usage:
 * @code
 * #include "gui/widgets/gui_widgets.h"
 *
 * void setup() {
 *     // Initialize widget system
 *     GUI::initWidgets();
 *
 *     // Create a screen using factory
 *     auto* screen = GUI::Factory().createSettingsScreen("Settings");
 *
 *     // Add widgets
 *     auto* content = static_cast<GUI::ScrollView*>(screen->findChild("content"));
 *     content->addChild(GUI::Factory().createLabeledInput("Server:", "192.168.1.1"));
 *     content->addChild(GUI::Factory().createLabeledInput("Port:", "5000"));
 *
 *     // Set as root
 *     GUI::WIDGETS.setRoot(screen);
 * }
 *
 * void loop() {
 *     // Update widgets (process input, render dirty regions)
 *     GUI::WIDGETS.update();
 * }
 * @endcode
 */

#ifndef GUI_WIDGETS_H
#define GUI_WIDGETS_H

// Signal/Slot system
#include "gui_signal.h"

// Base widget
#include "gui_widget.h"

// Concrete widgets
#include "gui_label.h"
#include "gui_button.h"
#include "gui_input.h"
#include "gui_container.h"
#include "gui_extras.h"

// Management
#include "gui_widget_manager.h"
#include "gui_display_adapter.h"
#include "gui_draw.h"
#include "gui_widget_factory.h"

namespace GUI {

//=============================================================================
// Widget System Initialization
//=============================================================================

/**
 * @brief Initialize the widget system
 *
 * Call this once at startup before using any widgets.
 * Initializes:
 * - Display adapter (queries display info)
 * - Widget manager
 * - Input adapter
 * - Connects to async renderer
 *
 * @param profile Display profile (default: M5Cardputer)
 */
inline void initWidgets(DisplayProfile profile = DisplayProfile::M5Cardputer) {
    // Initialize display adapter
    DisplayAdapter::instance().init(profile);

    // Initialize input adapter
    InputAdapter::instance().init();

    // Connect to renderer
    DisplayAdapter::instance().connectRenderer();
}

/**
 * @brief Initialize with custom display info
 */
inline void initWidgets(const DisplayInfo& info) {
    DisplayAdapter::instance().init(info);
    InputAdapter::instance().init();
    DisplayAdapter::instance().connectRenderer();
}

/**
 * @brief Shutdown the widget system
 *
 * Call this before program exit or when widgets are no longer needed.
 * Cleans up all resources.
 */
inline void shutdownWidgets() {
    DisplayAdapter::instance().disconnectRenderer();
    WidgetManager::instance().shutdown();
}

/**
 * @brief Update the widget system
 *
 * Call this every frame in the main loop.
 * Processes input, updates widgets, renders dirty regions.
 */
inline void updateWidgets() {
    // Update input
    InputAdapter::instance().update();

    // Update widgets (layout, render)
    WidgetManager::instance().update();

    // Submit frame boundary once per update tick.
    Draw::endFrame();
}

/**
 * @brief Process keyboard input from M5Cardputer
 *
 * Call this when a key is pressed on the M5Cardputer keyboard.
 *
 * @param key The pressed key character
 * @param fn Whether FN key is held
 * @param isPressed true for key press, false for key release
 */
inline void processKeyboard(char key, bool fn, bool isPressed = true) {
    InputAdapter::instance().processKey(key, fn, isPressed);
}

//=============================================================================
// Convenience Accessors
//=============================================================================

/**
 * @brief Get the widget manager
 */
inline WidgetManager& widgets() {
    return WidgetManager::instance();
}

/**
 * @brief Get the display adapter
 */
inline DisplayAdapter& display() {
    return DisplayAdapter::instance();
}

/**
 * @brief Get the input adapter
 */
inline InputAdapter& input() {
    return InputAdapter::instance();
}

/**
 * @brief Get the widget factory
 */
inline WidgetFactory& factory() {
    return WidgetFactory::instance();
}

//=============================================================================
// Quick Widget Creation
//=============================================================================

/**
 * @brief Quick label creation
 */
inline Label* label(const char* text, const char* name = nullptr) {
    return Factory().createLabel(text, name);
}

/**
 * @brief Quick button creation
 */
inline Button* button(const char* text, SlotFunction onClick = nullptr, const char* name = nullptr) {
    return Factory().createButton(text, onClick, name);
}

/**
 * @brief Quick input creation
 */
inline Input* inputField(const char* placeholder = nullptr, const char* name = nullptr) {
    return Factory().createInput(placeholder, InputType::Text, name);
}

/**
 * @brief Quick list view creation
 */
inline ListView* listView(const char* name = nullptr) {
    return Factory().createListView(name);
}

/**
 * @brief Quick vertical box creation
 */
inline Container* vbox(const char* name = nullptr) {
    return Factory().createVBox(name);
}

/**
 * @brief Quick horizontal box creation
 */
inline Container* hbox(const char* name = nullptr) {
    return Factory().createHBox(name);
}

/**
 * @brief Quick scroll view creation
 */
inline ScrollView* scrollView(const char* name = nullptr) {
    return Factory().createScrollView(name);
}

} // namespace GUI

#endif // GUI_WIDGETS_H

/**
 * @file example_remote_desktop_ui.h
 * @brief Example: Remote Desktop settings screen using widget system
 *
 * Demonstrates:
 * - Signal/Slot pattern for events
 * - Lazy rendering (only redraw changed widgets)
 * - Dynamic memory allocation
 * - Adaptive display (variant selection based on resolution)
 * - WidgetFactory for easy creation
 *
 * This example recreates the Remote Desktop settings screen from
 * remote_desktop.cpp using the new widget system.
 */

#ifndef EXAMPLE_REMOTE_DESKTOP_UI_H
#define EXAMPLE_REMOTE_DESKTOP_UI_H

#include "../gui_widgets.h"
#include "../gui_widget_renderer.h"

namespace GUI {
namespace Examples {

/**
 * @brief Remote Desktop Settings Screen
 *
 * A static settings screen that only redraws when user input
 * changes something. Demonstrates lazy rendering optimization.
 */
class RemoteDesktopSettings {
public:
    /**
     * @brief Constructor - creates all widgets
     */
    RemoteDesktopSettings();

    /**
     * @brief Destructor - widgets cleaned up automatically
     */
    ~RemoteDesktopSettings();

    /**
     * @brief Show the settings screen
     */
    void show();

    /**
     * @brief Hide the settings screen
     */
    void hide();

    /**
     * @brief Check if screen is visible
     */
    bool isVisible() const { return m_visible; }

    //=========================================================================
    // Settings Access
    //=========================================================================

    const char* server() const;
    void setServer(const char* server);

    int port() const;
    void setPort(int port);

    int quality() const;
    void setQuality(int q);

    int fps() const;
    void setFps(int fps);

    bool hasCookie() const { return m_hasCookie; }
    void setCookieStatus(bool has);

    bool hasKeys() const { return m_hasKeys; }
    void setKeysStatus(bool has);

    //=========================================================================
    // Signals
    //=========================================================================

    /**
     * @brief Signal emitted when user selects "Connect"
     */
    Signal& onConnect() { return m_connectSignal; }

    /**
     * @brief Signal emitted when user selects "Back"
     */
    Signal& onBack() { return m_backSignal; }

    /**
     * @brief Signal emitted when user selects "Set Server"
     */
    Signal& onSetServer() { return m_setServerSignal; }

    /**
     * @brief Signal emitted when user selects "Auto-discover"
     */
    Signal& onAutoDiscover() { return m_autoDiscoverSignal; }

private:
    void createWidgets();
    void updateStatusIndicators();
    void handleItemSelected(int index);

    // Root container
    Container* m_root;

    // Content list
    ListView* m_list;

    // Status labels (for dynamic updates)
    Label* m_serverLabel;
    Label* m_portLabel;
    Label* m_qualityLabel;
    Label* m_fpsLabel;
    StatusIndicator* m_cookieIndicator;
    StatusIndicator* m_keysIndicator;

    // Settings values
    char m_server[64];
    int m_port;
    int m_quality;
    int m_fps;
    bool m_hasCookie;
    bool m_hasKeys;

    // State
    bool m_visible;

    // Signals
    Signal m_connectSignal;
    Signal m_backSignal;
    Signal m_setServerSignal;
    Signal m_autoDiscoverSignal;
};

//=============================================================================
// Implementation
//=============================================================================

inline RemoteDesktopSettings::RemoteDesktopSettings()
    : m_root(nullptr)
    , m_list(nullptr)
    , m_serverLabel(nullptr)
    , m_portLabel(nullptr)
    , m_qualityLabel(nullptr)
    , m_fpsLabel(nullptr)
    , m_cookieIndicator(nullptr)
    , m_keysIndicator(nullptr)
    , m_port(5000)
    , m_quality(50)
    , m_fps(30)
    , m_hasCookie(false)
    , m_hasKeys(false)
    , m_visible(false)
{
    strcpy(m_server, "192.168.1.100");
    createWidgets();
}

inline RemoteDesktopSettings::~RemoteDesktopSettings() {
    // Widgets are owned by widget manager, will be cleaned up
    hide();
}

inline void RemoteDesktopSettings::createWidgets() {
    auto& f = Factory();
    auto& d = Display();

    // Create main screen container
    m_root = f.createScreen("Remote Desktop Settings", "rdScreen");

    // Get content area
    Widget* contentWidget = m_root->findChild("content");
    if (!contentWidget) return;

    Container* content = static_cast<Container*>(contentWidget);

    // Create list view for menu items
    m_list = f.createListView("rdList");
    m_list->setItemHeight(d.recommendedItemHeight());

    // Add items
    // Config section
    char buf[80];

    snprintf(buf, sizeof(buf), "Server: %s", m_server);
    m_list->addItem(buf, nullptr, LabelColors::Info);

    snprintf(buf, sizeof(buf), "Port: %d", m_port);
    m_list->addItem(buf, nullptr, LabelColors::Info);

    snprintf(buf, sizeof(buf), "Quality: %d%%", m_quality);
    m_list->addItem(buf, nullptr, LabelColors::Info);

    snprintf(buf, sizeof(buf), "FPS: %d", m_fps);
    m_list->addItem(buf, nullptr, LabelColors::Info);

    // Status section
    m_list->addItem("Cookie", m_hasCookie ? "OK" : "Missing",
                    m_hasCookie ? LabelColors::Ok : LabelColors::Error);

    m_list->addItem("Keys", m_hasKeys ? "OK" : "Missing",
                    m_hasKeys ? LabelColors::Ok : LabelColors::Error);

    // Divider (non-selectable)
    ListItem divider;
    divider.text = "---";
    divider.selectable = false;
    m_list->addItem(divider);

    // Actions
    m_list->addItem("Connect", "[Enter]", LabelColors::Ok);
    m_list->addItem("Set server", "[S]");
    m_list->addItem("Auto-discover", "[A]");
    m_list->addItem("Back", "[Esc]");

    // Connect selection signal
    m_list->onItemActivated([this](const Event& e) {
        handleItemSelected(e.data.value.newValue);
    });

    // Also handle keyboard shortcuts
    m_list->onKeyPress([this](const Event& e) {
        char key = e.data.key.key;
        switch (key) {
            case 's':
            case 'S':
                m_setServerSignal.emit(Events::click());
                break;
            case 'a':
            case 'A':
                m_autoDiscoverSignal.emit(Events::click());
                break;
            case 27:  // ESC
            case 'b':
            case 'B':
                m_backSignal.emit(Events::click());
                break;
        }
    });

    content->addChild(m_list);
}

inline void RemoteDesktopSettings::show() {
    if (m_visible) return;

    // Set as root widget
    WIDGETS.setRoot(m_root);
    m_visible = true;

    // Force initial render
    widgetRenderer().fullRedraw();
}

inline void RemoteDesktopSettings::hide() {
    if (!m_visible) return;

    WIDGETS.clearRoot();
    m_visible = false;
}

inline const char* RemoteDesktopSettings::server() const {
    return m_server;
}

inline void RemoteDesktopSettings::setServer(const char* server) {
    strncpy(m_server, server, sizeof(m_server) - 1);
    m_server[sizeof(m_server) - 1] = '\0';

    // Update list item (only this item will be redrawn - lazy!)
    if (m_list) {
        char buf[80];
        snprintf(buf, sizeof(buf), "Server: %s", m_server);
        ListItem item(buf, nullptr, LabelColors::Info);
        m_list->updateItem(0, item);
    }
}

inline int RemoteDesktopSettings::port() const {
    return m_port;
}

inline void RemoteDesktopSettings::setPort(int port) {
    m_port = port;

    if (m_list) {
        char buf[40];
        snprintf(buf, sizeof(buf), "Port: %d", m_port);
        ListItem item(buf, nullptr, LabelColors::Info);
        m_list->updateItem(1, item);
    }
}

inline int RemoteDesktopSettings::quality() const {
    return m_quality;
}

inline void RemoteDesktopSettings::setQuality(int q) {
    m_quality = q;

    if (m_list) {
        char buf[40];
        snprintf(buf, sizeof(buf), "Quality: %d%%", m_quality);
        ListItem item(buf, nullptr, LabelColors::Info);
        m_list->updateItem(2, item);
    }
}

inline int RemoteDesktopSettings::fps() const {
    return m_fps;
}

inline void RemoteDesktopSettings::setFps(int fps) {
    m_fps = fps;

    if (m_list) {
        char buf[40];
        snprintf(buf, sizeof(buf), "FPS: %d", m_fps);
        ListItem item(buf, nullptr, LabelColors::Info);
        m_list->updateItem(3, item);
    }
}

inline void RemoteDesktopSettings::setCookieStatus(bool has) {
    m_hasCookie = has;

    if (m_list) {
        ListItem item("Cookie", has ? "OK" : "Missing",
                      has ? LabelColors::Ok : LabelColors::Error);
        m_list->updateItem(4, item);
    }
}

inline void RemoteDesktopSettings::setKeysStatus(bool has) {
    m_hasKeys = has;

    if (m_list) {
        ListItem item("Keys", has ? "OK" : "Missing",
                      has ? LabelColors::Ok : LabelColors::Error);
        m_list->updateItem(5, item);
    }
}

inline void RemoteDesktopSettings::handleItemSelected(int index) {
    switch (index) {
        case 7:  // Connect
            m_connectSignal.emit(Events::click());
            break;
        case 8:  // Set server
            m_setServerSignal.emit(Events::click());
            break;
        case 9:  // Auto-discover
            m_autoDiscoverSignal.emit(Events::click());
            break;
        case 10: // Back
            m_backSignal.emit(Events::click());
            break;
    }
}

//=============================================================================
// Usage Example
//=============================================================================

/**
 * Example usage in main code:
 *
 * @code
 * #include "gui/widgets/examples/example_remote_desktop_ui.h"
 *
 * GUI::Examples::RemoteDesktopSettings* rdSettings = nullptr;
 *
 * void setup() {
 *     auto cfg = M5.config();
 *     M5Cardputer.begin(cfg);
 *
 *     // Initialize GUI
 *     GUI::begin();
 *     GUI::initWidgetSystem();
 *
 *     // Create settings screen
 *     rdSettings = new GUI::Examples::RemoteDesktopSettings();
 *
 *     // Connect signals
 *     rdSettings->onConnect().connect(GUI::SignalType::Click,
 *         [](const GUI::Event&) {
 *             Serial.println("Connect clicked!");
 *             // Start connection...
 *         });
 *
 *     rdSettings->onBack().connect(GUI::SignalType::Click,
 *         [](const GUI::Event&) {
 *             Serial.println("Back clicked!");
 *             // Return to main menu...
 *         });
 *
 *     // Show the screen
 *     rdSettings->show();
 * }
 *
 * void loop() {
 *     M5Cardputer.update();
 *     GUI::updateWidgetSystem();  // This handles all input and rendering!
 *
 *     // The screen is static - only redraws when something changes
 *     // No need for constant redraw calls!
 * }
 * @endcode
 */

} // namespace Examples
} // namespace GUI

#endif // EXAMPLE_REMOTE_DESKTOP_UI_H

/**
 * @file gui_signal.h
 * @brief Signal/Slot event system for widget communication
 *
 * Implements a type-safe Signal/Slot pattern for decoupled event handling:
 * - Signals: emit events (render requests, input events, state changes)
 * - Slots: receive and handle events (widget updates, system notifications)
 *
 * Supports both render signals (from system logic) and input signals (from user actions)
 */

#ifndef GUI_SIGNAL_H
#define GUI_SIGNAL_H

#include <Arduino.h>
#include <functional>
#include <vector>

namespace GUI {

// Forward declarations
class Widget;

//=============================================================================
// Signal Types
//=============================================================================

/**
 * @brief Types of signals in the system
 */
enum class SignalType : uint8_t {
    // Render signals (from system logic)
    Render,             // Request widget redraw
    Invalidate,         // Mark region as dirty
    ThemeChanged,       // Theme was changed
    LayoutChanged,      // Layout needs recalculation

    // Input signals (from user actions)
    KeyPress,           // Key was pressed
    KeyRelease,         // Key was released
    KeyHold,            // Key is being held
    Focus,              // Widget gained focus
    Blur,               // Widget lost focus

    // Value signals
    ValueChanged,       // Widget value changed
    SelectionChanged,   // Selection changed (lists, menus)
    TextChanged,        // Text content changed

    // Navigation signals
    ScrollRequest,      // Request to scroll
    NavigateNext,       // Navigate to next widget
    NavigatePrev,       // Navigate to previous widget

    // Action signals
    Click,              // Click/activation
    LongPress,          // Long press action
    Submit,             // Form submission
    Cancel,             // Cancel action

    // System signals
    DisplayReady,       // Display driver ready
    ResolutionChanged,  // Screen resolution changed
    MemoryWarning,      // Low memory warning

    Custom              // User-defined signals
};

/**
 * @brief Priority levels for signal processing
 */
enum class SignalPriority : uint8_t {
    Immediate = 0,      // Process immediately (input events)
    High = 1,           // High priority (value changes)
    Normal = 2,         // Normal priority (most signals)
    Low = 3,            // Low priority (animations, deferred)
    Deferred = 4        // Process during idle time
};

//=============================================================================
// Event Data Structures
//=============================================================================

/**
 * @brief Key event data
 */
struct KeyEventData {
    char key;                   // ASCII key code
    uint8_t modifiers;          // Modifier flags (FN, shift, etc.)
    uint32_t holdDuration;      // How long key held (ms)
    bool isRepeat;              // Is this a repeat event

    // Modifier bit flags
    static constexpr uint8_t MOD_FN    = 0x01;
    static constexpr uint8_t MOD_SHIFT = 0x02;
    static constexpr uint8_t MOD_CTRL  = 0x04;
    static constexpr uint8_t MOD_ALT   = 0x08;

    bool hasFn() const { return modifiers & MOD_FN; }
    bool hasShift() const { return modifiers & MOD_SHIFT; }
};

/**
 * @brief Render event data
 */
struct RenderEventData {
    int16_t x;                  // Dirty region X
    int16_t y;                  // Dirty region Y
    int16_t width;              // Dirty region width
    int16_t height;             // Dirty region height
    bool fullRedraw;            // Force full redraw
    uint8_t frameId;            // Frame sequence ID
};

/**
 * @brief Value change event data
 */
struct ValueEventData {
    int32_t oldValue;           // Previous value
    int32_t newValue;           // New value
    const char* stringValue;    // Optional string value
};

/**
 * @brief Scroll event data
 */
struct ScrollEventData {
    int16_t deltaX;             // Horizontal scroll amount
    int16_t deltaY;             // Vertical scroll amount
    int16_t targetX;            // Target scroll position X
    int16_t targetY;            // Target scroll position Y
    bool smooth;                // Use smooth scrolling
};

/**
 * @brief Display event data
 */
struct DisplayEventData {
    int16_t width;              // Display width
    int16_t height;             // Display height
    uint8_t colorDepth;         // Color depth (bits)
    uint8_t rotation;           // Display rotation
    float scaleFactor;          // Scale factor for widgets
};

/**
 * @brief Union of all event data types
 */
union EventDataUnion {
    KeyEventData key;
    RenderEventData render;
    ValueEventData value;
    ScrollEventData scroll;
    DisplayEventData display;
    void* custom;               // For custom signals

    EventDataUnion() : custom(nullptr) {}
};

/**
 * @brief Complete event structure
 */
struct Event {
    SignalType type;            // Signal type
    SignalPriority priority;    // Processing priority
    Widget* sender;             // Widget that emitted (may be null for system)
    Widget* target;             // Target widget (may be null for broadcast)
    EventDataUnion data;        // Event data
    uint32_t timestamp;         // When event was created
    bool consumed;              // Has event been handled

    Event()
        : type(SignalType::Custom)
        , priority(SignalPriority::Normal)
        , sender(nullptr)
        , target(nullptr)
        , timestamp(0)
        , consumed(false) {}

    Event(SignalType t, SignalPriority p = SignalPriority::Normal)
        : type(t)
        , priority(p)
        , sender(nullptr)
        , target(nullptr)
        , timestamp(millis())
        , consumed(false) {}

    void consume() { consumed = true; }
    bool isConsumed() const { return consumed; }
};

//=============================================================================
// Slot (Event Handler)
//=============================================================================

/**
 * @brief Slot ID for connection management
 */
using SlotId = uint16_t;
constexpr SlotId INVALID_SLOT_ID = 0;

/**
 * @brief Slot function signature
 */
using SlotFunction = std::function<void(const Event&)>;

/**
 * @brief Slot connection information
 */
struct SlotConnection {
    SlotId id;                  // Unique ID
    SlotFunction handler;       // Handler function
    SignalType signalType;      // Which signal type this handles
    Widget* receiver;           // Receiving widget (for cleanup)
    SignalPriority minPriority; // Minimum priority to handle
    bool enabled;               // Is slot enabled
    bool oneShot;               // Disconnect after first call

    SlotConnection()
        : id(INVALID_SLOT_ID)
        , signalType(SignalType::Custom)
        , receiver(nullptr)
        , minPriority(SignalPriority::Deferred)
        , enabled(true)
        , oneShot(false) {}
};

//=============================================================================
// Signal Class
//=============================================================================

/**
 * @brief Signal emitter class
 *
 * Allows connecting multiple slots and emitting events to all of them.
 * Supports filtering by signal type and priority.
 */
class Signal {
public:
    Signal() : m_nextSlotId(1) {}
    ~Signal() { disconnectAll(); }

    // Non-copyable
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

    /**
     * @brief Connect a slot to this signal
     * @param type Signal type to listen for (Custom = all types)
     * @param handler Function to call when signal emitted
     * @param receiver Optional receiver widget for cleanup tracking
     * @return Slot ID for later disconnection
     */
    SlotId connect(SignalType type, SlotFunction handler, Widget* receiver = nullptr) {
        SlotConnection conn;
        conn.id = m_nextSlotId++;
        conn.handler = handler;
        conn.signalType = type;
        conn.receiver = receiver;
        conn.enabled = true;
        conn.oneShot = false;

        m_slots.push_back(conn);
        return conn.id;
    }

    /**
     * @brief Connect a one-shot slot (auto-disconnects after first call)
     */
    SlotId connectOnce(SignalType type, SlotFunction handler, Widget* receiver = nullptr) {
        SlotId id = connect(type, handler, receiver);
        for (auto& slot : m_slots) {
            if (slot.id == id) {
                slot.oneShot = true;
                break;
            }
        }
        return id;
    }

    /**
     * @brief Disconnect a slot by ID
     * @return true if slot was found and removed
     */
    bool disconnect(SlotId id) {
        for (auto it = m_slots.begin(); it != m_slots.end(); ++it) {
            if (it->id == id) {
                m_slots.erase(it);
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Disconnect all slots for a specific receiver
     */
    void disconnectReceiver(Widget* receiver) {
        if (!receiver) return;

        m_slots.erase(
            std::remove_if(m_slots.begin(), m_slots.end(),
                [receiver](const SlotConnection& conn) {
                    return conn.receiver == receiver;
                }),
            m_slots.end()
        );
    }

    /**
     * @brief Disconnect all slots
     */
    void disconnectAll() {
        m_slots.clear();
    }

    /**
     * @brief Enable/disable a slot
     */
    void setSlotEnabled(SlotId id, bool enabled) {
        for (auto& slot : m_slots) {
            if (slot.id == id) {
                slot.enabled = enabled;
                break;
            }
        }
    }

    /**
     * @brief Emit event to all connected slots
     * @param event Event to emit
     * @return Number of slots that received the event
     */
    size_t emit(Event& event) {
        size_t count = 0;
        std::vector<SlotId> toRemove;

        for (auto& slot : m_slots) {
            // Skip disabled slots
            if (!slot.enabled) continue;

            // Check signal type match (Custom matches all)
            if (slot.signalType != SignalType::Custom &&
                slot.signalType != event.type) {
                continue;
            }

            // Check priority
            if (static_cast<uint8_t>(event.priority) >
                static_cast<uint8_t>(slot.minPriority)) {
                continue;
            }

            // Call handler
            slot.handler(event);
            count++;

            // Mark one-shot slots for removal
            if (slot.oneShot) {
                toRemove.push_back(slot.id);
            }

            // Stop if event consumed
            if (event.consumed) break;
        }

        // Remove one-shot slots
        for (SlotId id : toRemove) {
            disconnect(id);
        }

        return count;
    }

    /**
     * @brief Emit event (const version, creates copy)
     */
    size_t emit(const Event& event) {
        Event copy = event;
        return emit(copy);
    }

    /**
     * @brief Get number of connected slots
     */
    size_t slotCount() const { return m_slots.size(); }

    /**
     * @brief Check if any slots connected
     */
    bool hasSlots() const { return !m_slots.empty(); }

private:
    std::vector<SlotConnection> m_slots;
    SlotId m_nextSlotId;
};

//=============================================================================
// Event Queue for Deferred Processing
//=============================================================================

/**
 * @brief Event queue entry
 */
struct QueuedEvent {
    Event event;
    uint32_t deliverAt;         // When to deliver (0 = immediate)
};

/**
 * @brief Event queue for deferred signal delivery
 */
class EventQueue {
public:
    static constexpr size_t MAX_QUEUED_EVENTS = 32;

    EventQueue() : m_head(0), m_tail(0) {}

    /**
     * @brief Queue an event for later processing
     */
    bool push(const Event& event, uint32_t delayMs = 0) {
        size_t next = (m_tail + 1) % MAX_QUEUED_EVENTS;
        if (next == m_head) {
            // Queue full
            return false;
        }

        m_events[m_tail].event = event;
        m_events[m_tail].deliverAt = (delayMs > 0) ? millis() + delayMs : 0;
        m_tail = next;
        return true;
    }

    /**
     * @brief Pop next ready event
     * @param event Output event
     * @return true if event available
     */
    bool pop(Event& event) {
        if (m_head == m_tail) {
            return false;  // Empty
        }

        // Check if front event is ready
        if (m_events[m_head].deliverAt > 0 &&
            millis() < m_events[m_head].deliverAt) {
            return false;  // Not ready yet
        }

        event = m_events[m_head].event;
        m_head = (m_head + 1) % MAX_QUEUED_EVENTS;
        return true;
    }

    /**
     * @brief Check if queue has ready events
     */
    bool hasReady() const {
        if (m_head == m_tail) return false;
        if (m_events[m_head].deliverAt == 0) return true;
        return millis() >= m_events[m_head].deliverAt;
    }

    /**
     * @brief Check if queue is empty
     */
    bool isEmpty() const { return m_head == m_tail; }

    /**
     * @brief Get number of queued events
     */
    size_t count() const {
        if (m_tail >= m_head) {
            return m_tail - m_head;
        }
        return MAX_QUEUED_EVENTS - m_head + m_tail;
    }

    /**
     * @brief Clear all queued events
     */
    void clear() { m_head = m_tail = 0; }

private:
    QueuedEvent m_events[MAX_QUEUED_EVENTS];
    volatile size_t m_head;
    volatile size_t m_tail;
};

//=============================================================================
// Helper Functions for Creating Events
//=============================================================================

namespace Events {

/**
 * @brief Create a key press event
 */
inline Event keyPress(char key, uint8_t modifiers = 0) {
    Event e(SignalType::KeyPress, SignalPriority::Immediate);
    e.data.key.key = key;
    e.data.key.modifiers = modifiers;
    e.data.key.holdDuration = 0;
    e.data.key.isRepeat = false;
    return e;
}

/**
 * @brief Create a key hold event
 */
inline Event keyHold(char key, uint32_t duration, uint8_t modifiers = 0) {
    Event e(SignalType::KeyHold, SignalPriority::Immediate);
    e.data.key.key = key;
    e.data.key.modifiers = modifiers;
    e.data.key.holdDuration = duration;
    e.data.key.isRepeat = true;
    return e;
}

/**
 * @brief Create a render request event
 */
inline Event render(int16_t x = 0, int16_t y = 0,
                    int16_t w = 0, int16_t h = 0,
                    bool full = false) {
    Event e(SignalType::Render, SignalPriority::Normal);
    e.data.render.x = x;
    e.data.render.y = y;
    e.data.render.width = w;
    e.data.render.height = h;
    e.data.render.fullRedraw = full;
    return e;
}

/**
 * @brief Create an invalidate event
 */
inline Event invalidate(int16_t x, int16_t y, int16_t w, int16_t h) {
    Event e(SignalType::Invalidate, SignalPriority::Normal);
    e.data.render.x = x;
    e.data.render.y = y;
    e.data.render.width = w;
    e.data.render.height = h;
    e.data.render.fullRedraw = false;
    return e;
}

/**
 * @brief Create a value changed event
 */
inline Event valueChanged(int32_t oldVal, int32_t newVal,
                          const char* str = nullptr) {
    Event e(SignalType::ValueChanged, SignalPriority::High);
    e.data.value.oldValue = oldVal;
    e.data.value.newValue = newVal;
    e.data.value.stringValue = str;
    return e;
}

/**
 * @brief Create a scroll request event
 */
inline Event scrollRequest(int16_t dx, int16_t dy, bool smooth = true) {
    Event e(SignalType::ScrollRequest, SignalPriority::Normal);
    e.data.scroll.deltaX = dx;
    e.data.scroll.deltaY = dy;
    e.data.scroll.smooth = smooth;
    return e;
}

/**
 * @brief Create a display ready event
 */
inline Event displayReady(int16_t w, int16_t h, uint8_t depth,
                          uint8_t rotation, float scale) {
    Event e(SignalType::DisplayReady, SignalPriority::High);
    e.data.display.width = w;
    e.data.display.height = h;
    e.data.display.colorDepth = depth;
    e.data.display.rotation = rotation;
    e.data.display.scaleFactor = scale;
    return e;
}

/**
 * @brief Create a focus event
 */
inline Event focus() {
    return Event(SignalType::Focus, SignalPriority::High);
}

/**
 * @brief Create a blur event
 */
inline Event blur() {
    return Event(SignalType::Blur, SignalPriority::High);
}

/**
 * @brief Create a click event
 */
inline Event click() {
    return Event(SignalType::Click, SignalPriority::Immediate);
}

/**
 * @brief Create a submit event
 */
inline Event submit() {
    return Event(SignalType::Submit, SignalPriority::Immediate);
}

/**
 * @brief Create a cancel event
 */
inline Event cancel() {
    return Event(SignalType::Cancel, SignalPriority::Immediate);
}

} // namespace Events

} // namespace GUI

#endif // GUI_SIGNAL_H

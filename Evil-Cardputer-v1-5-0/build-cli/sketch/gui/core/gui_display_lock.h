#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\core\\gui_display_lock.h"
/*
 * GUI Display Lock - Cross-core serialization for direct display access
 *
 * Guarantees that direct M5.Display operations are not interleaved across
 * renderer and legacy/direct callers running on different cores.
 */

#ifndef GUI_DISPLAY_LOCK_H
#define GUI_DISPLAY_LOCK_H

#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace GUI {

// Initialize global recursive display mutex (idempotent).
bool initDisplayLock();

// Shutdown global display mutex (idempotent).
void shutdownDisplayLock();

// Lock display access for current task. Returns false on timeout/failure.
bool lockDisplay(uint32_t timeoutMs = portMAX_DELAY);

// Unlock display access.
void unlockDisplay();

class DisplayLockGuard {
public:
    explicit DisplayLockGuard(uint32_t timeoutMs = portMAX_DELAY)
        : m_locked(lockDisplay(timeoutMs)) {}

    ~DisplayLockGuard() {
        if (m_locked) {
            unlockDisplay();
        }
    }

    DisplayLockGuard(const DisplayLockGuard&) = delete;
    DisplayLockGuard& operator=(const DisplayLockGuard&) = delete;

    bool locked() const { return m_locked; }

private:
    bool m_locked;
};

} // namespace GUI

#endif // GUI_DISPLAY_LOCK_H


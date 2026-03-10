#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\core\\gui_display_lock.cpp"
/*
 * GUI Display Lock - Cross-core serialization for direct display access
 */

#include "gui_display_lock.h"

#include "../gui_config.h"

namespace GUI {
namespace {

SemaphoreHandle_t s_displayMutex = nullptr;
portMUX_TYPE s_displayMutexInitLock = portMUX_INITIALIZER_UNLOCKED;

} // namespace

bool initDisplayLock() {
    if (s_displayMutex != nullptr) {
        return true;
    }

    portENTER_CRITICAL(&s_displayMutexInitLock);
    if (s_displayMutex == nullptr) {
        s_displayMutex = xSemaphoreCreateRecursiveMutex();
        if (s_displayMutex == nullptr) {
            GUI_LOG_ERROR("DisplayLock: failed to create recursive mutex");
        } else {
            GUI_LOG("DisplayLock initialized");
        }
    }
    portEXIT_CRITICAL(&s_displayMutexInitLock);

    return s_displayMutex != nullptr;
}

void shutdownDisplayLock() {
    SemaphoreHandle_t mutexToDelete = nullptr;

    portENTER_CRITICAL(&s_displayMutexInitLock);
    mutexToDelete = s_displayMutex;
    s_displayMutex = nullptr;
    portEXIT_CRITICAL(&s_displayMutexInitLock);

    if (mutexToDelete != nullptr) {
        vSemaphoreDelete(mutexToDelete);
        GUI_LOG("DisplayLock shutdown");
    }
}

bool lockDisplay(uint32_t timeoutMs) {
    if (!initDisplayLock()) {
        return false;
    }

    const TickType_t timeoutTicks = (timeoutMs == portMAX_DELAY)
        ? portMAX_DELAY
        : pdMS_TO_TICKS(timeoutMs);

    return xSemaphoreTakeRecursive(s_displayMutex, timeoutTicks) == pdTRUE;
}

void unlockDisplay() {
    if (s_displayMutex == nullptr) {
        return;
    }

    xSemaphoreGiveRecursive(s_displayMutex);
}

} // namespace GUI

/*
 * GUI Framework - Main Include Header
 *
 * Async rendering system for ESP32 Cardputer.
 *
 * Include this single header to use the GUI framework.
 *
 * Quick Start:
 * -----------
 *   #include "gui/gui.h"
 *
 *   void setup() {
 *       M5.begin();
 *       GUI::guiInit();
 *       GUI::guiStart();
 *   }
 *
 *   void loop() {
 *       // Non-blocking draw calls
 *       GUI::Draw::fillRect(10, 10, 50, 30, GUI::Colors::Green);
 *       GUI::Draw::drawText(10, 50, "Hello", GUI::Colors::White);
 *       GUI::Draw::endFrame();
 *
 *       // ... other main loop work
 *   }
 *
 * Architecture:
 * ------------
 *   Main Loop (Core 1)          Renderer (Core 0)
 *        |                            |
 *        |   push(RenderOp)           |
 *        +------------------------->  |
 *        |                            |
 *        |     [Lock-free Queue]      |
 *        |                            |
 *        |                      pop() |
 *        |  <-------------------------+
 *        |                            |
 *        |                   execute  |
 *        |                    M5GFX   |
 *        |                            |
 *
 * Features (Phase 1):
 * - Lock-free SPSC render queue (256 commands, ~8KB)
 * - Dedicated render task on Core 0
 * - Batch processing with auto-flush
 * - Command types: FillRect, DrawRect, DrawLine, DrawPixel, DrawText, etc.
 * - Statistics and monitoring
 *
 * Planned (Phase 2):
 * - Double buffering with PSRAM
 * - DMA transfers
 * - Dirty region tracking
 *
 * Memory Usage:
 * - SRAM: ~8KB (queue) + ~4KB (task stack) = ~12KB
 * - PSRAM: 0 (Phase 1), 130KB (Phase 2 with double buffer)
 */

#ifndef GUI_H
#define GUI_H

// Arduino for Serial (printStatus)
#include <Arduino.h>

// Configuration (compile-time settings)
#include "gui_config.h"

// Basic types
#include "gui_types.h"

// Core components
#include "core/gui_render_queue.h"
#include "core/gui_renderer.h"

// ============================================================================
// Version Information
// ============================================================================

#define GUI_VERSION_MAJOR 1
#define GUI_VERSION_MINOR 0
#define GUI_VERSION_PATCH 0
#define GUI_VERSION_STRING "1.0.0-phase1"

namespace GUI {

// Get version as string
inline const char* version() {
    return GUI_VERSION_STRING;
}

// ============================================================================
// Initialization Helpers
// ============================================================================

/**
 * Full initialization and startup.
 * Equivalent to: guiInit() && guiStart()
 *
 * @return true if successful
 */
inline bool begin() {
    if (!guiInit()) {
        return false;
    }
    return guiStart();
}

/**
 * Full shutdown.
 * Equivalent to: guiStop(); guiShutdown();
 */
inline void end() {
    guiStop();
    guiShutdown();
}

// ============================================================================
// Status Information
// ============================================================================

/**
 * Get queue fill percentage (0-100)
 */
inline int queueFillPercent() {
    RenderQueue& q = renderQueue();
    return (q.pending() * 100) / q.capacity();
}

/**
 * Print status to Serial (debug)
 */
inline void printStatus() {
    RenderQueue& q = renderQueue();
    Renderer& r = renderer();
    const RendererStats& stats = r.getStats();

    Serial.printf("[GUI] Status:\n");
    Serial.printf("  Running: %s\n", r.isRunning() ? "yes" : "no");
    Serial.printf("  Queue: %d/%d (%d%%)\n",
                  q.pending(), q.capacity(), queueFillPercent());
    Serial.printf("  Overflows: %lu\n", q.getOverflowCount());
    Serial.printf("  High water: %d\n", q.getHighWaterMark());
    Serial.printf("  Commands: %lu\n", stats.commandsProcessed);
    Serial.printf("  Frames: %lu\n", stats.framesRendered);
    Serial.printf("  Flushes: %lu\n", stats.displayFlushCount);
    Serial.printf("  Avg render: %lu us\n", stats.avgRenderTimeUs());
    Serial.printf("  Max render: %lu us\n", stats.maxRenderTimeUs);
    Serial.printf("  FPS: %.1f\n", stats.fps());
}

} // namespace GUI

#endif // GUI_H

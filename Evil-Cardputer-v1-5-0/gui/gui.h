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
 * Features (Phase 2):
 * - Double buffering with PSRAM (~130KB for two buffers)
 * - DMA transfers for async display updates
 * - Framebuffer rendering for tear-free display
 *
 * Planned (Phase 3):
 * - Dirty region tracking
 * - Partial update optimization
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

// Theme system
#include "gui_theme.h"

// Core components
#include "core/gui_render_queue.h"
#include "core/gui_framebuffer.h"
#include "core/gui_dma.h"
#include "core/gui_renderer.h"

// ============================================================================
// Version Information
// ============================================================================

#define GUI_VERSION_MAJOR 2
#define GUI_VERSION_MINOR 0
#define GUI_VERSION_PATCH 0
#define GUI_VERSION_STRING "2.0.0-phase2"

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

#if GUI_DOUBLE_BUFFER
    const FramebufferStats& fbStats = Framebuffer::instance().getStats();
    Serial.printf("  Buffer swaps: %lu\n", fbStats.swapCount);
    Serial.printf("  DMA transfers: %lu\n", fbStats.dmaTransferCount);

    const DmaStats& dmaStats = DmaTransfer::instance().getStats();
    Serial.printf("  DMA avg time: %lu us\n", dmaStats.avgTransferTimeUs());
#endif
}

// ============================================================================
// Phase 2 Accessors
// ============================================================================

#if GUI_DOUBLE_BUFFER

/**
 * Get framebuffer instance
 */
inline Framebuffer& framebuffer() { return Framebuffer::instance(); }

/**
 * Get DMA transfer instance
 */
inline DmaTransfer& dmaTransfer() { return DmaTransfer::instance(); }

/**
 * Get display updater instance
 */
inline DisplayUpdater& displayUpdater() { return DisplayUpdater::instance(); }

/**
 * Check if double buffering is active
 */
inline bool isDoubleBuffered() {
    return renderer().getRenderMode() == RenderMode::DoubleBuffered;
}

#endif // GUI_DOUBLE_BUFFER

// ============================================================================
// Theme System Accessors
// ============================================================================

/**
 * Get ThemeManager instance
 */
inline ThemeManager& themeManager() { return ThemeManager::instance(); }

/**
 * Set theme by ID
 */
inline void setTheme(ThemeId id) { themeManager().setTheme(id); }

/**
 * Get current theme
 */
inline const Theme& currentTheme() { return themeManager().current(); }

/**
 * Get current theme colors (with overrides)
 */
inline const ThemeColors& currentColors() { return themeManager().effectiveColors(); }

/**
 * Get current theme fonts
 */
inline const ThemeFonts& currentFonts() { return themeManager().fonts(); }

/**
 * Get current theme spacing
 */
inline const ThemeSpacing& currentSpacing() { return themeManager().spacing(); }

} // namespace GUI

#endif // GUI_H

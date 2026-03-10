/*
 * GUI Configuration - Compile-time settings for the GUI framework
 *
 * All configurable parameters for the async rendering system.
 * Adjust these based on available memory and performance requirements.
 */

#ifndef GUI_CONFIG_H
#define GUI_CONFIG_H

#include <cstdint>
#include <cstddef>

namespace GUI {
namespace Config {

// ============================================================================
// Display Configuration
// ============================================================================

// Internal display (ST7789V)
constexpr uint16_t DISPLAY_WIDTH  = 240;
constexpr uint16_t DISPLAY_HEIGHT = 135;
constexpr uint8_t  DISPLAY_BPP    = 16;  // Bits per pixel (RGB565)

// Framebuffer size in bytes (240 * 135 * 2 = 64,800)
constexpr uint32_t FRAMEBUFFER_SIZE = DISPLAY_WIDTH * DISPLAY_HEIGHT * (DISPLAY_BPP / 8);

// ============================================================================
// Render Queue Configuration
// ============================================================================

// Number of commands in the ring buffer (must be power of 2)
// 512 commands * ~32 bytes = ~16KB SRAM
// (increased from 256 to handle heavy UI functions like DHCP starvation)
constexpr size_t QUEUE_SIZE = 512;

// Mask for efficient modulo (QUEUE_SIZE - 1)
constexpr size_t QUEUE_MASK = QUEUE_SIZE - 1;

// Static assertion to ensure QUEUE_SIZE is power of 2
static_assert((QUEUE_SIZE & (QUEUE_SIZE - 1)) == 0,
              "QUEUE_SIZE must be a power of 2");

// Maximum text length per render command (for embedded text)
constexpr size_t MAX_TEXT_LENGTH = 64;

// ============================================================================
// Queue Backpressure Policy
// ============================================================================
//
// Deterministic behavior under overload:
// - First, retry enqueue with a short backoff
// - If still full, apply overflow policy (drop newest by default)
//
// These values are compile-time and can be overridden via build flags.

constexpr uint8_t QUEUE_OVERFLOW_DROP_NEWEST = 0;
constexpr uint8_t QUEUE_OVERFLOW_BLOCK_PRODUCER = 1;

#ifndef GUI_QUEUE_OVERFLOW_POLICY
#define GUI_QUEUE_OVERFLOW_POLICY 0
#endif
constexpr uint8_t QUEUE_OVERFLOW_POLICY = GUI_QUEUE_OVERFLOW_POLICY;

#ifndef GUI_QUEUE_PUSH_MAX_WAIT_MS
#define GUI_QUEUE_PUSH_MAX_WAIT_MS 6
#endif
constexpr uint32_t QUEUE_PUSH_MAX_WAIT_MS = GUI_QUEUE_PUSH_MAX_WAIT_MS;

#ifndef GUI_QUEUE_PUSH_RETRY_DELAY_MS
#define GUI_QUEUE_PUSH_RETRY_DELAY_MS 1
#endif
constexpr uint32_t QUEUE_PUSH_RETRY_DELAY_MS = GUI_QUEUE_PUSH_RETRY_DELAY_MS;

// ============================================================================
// Renderer Task Configuration
// ============================================================================

// Core assignment for the render task
// Core 0: Render task (this)
// Core 1: Main loop + WiFi/BLE
constexpr int RENDER_TASK_CORE = 0;

// Render task stack size in bytes
// 8KB needed for M5Canvas sprite rendering + Bresenham + text/font ops
constexpr uint32_t RENDER_TASK_STACK_SIZE = 8192;

// Render task priority (higher = more priority)
// Should be higher than normal tasks but lower than critical interrupts
constexpr int RENDER_TASK_PRIORITY = 5;

// Maximum time to wait for queue operations (ms)
constexpr uint32_t QUEUE_TIMEOUT_MS = 16;  // ~60 fps flush cadence

// ============================================================================
// Memory Allocation
// ============================================================================

// Use PSRAM for framebuffers (ESP32-S3 has 8MB)
#ifndef GUI_USE_PSRAM
#define GUI_USE_PSRAM 1
#endif

// Enable double buffering (Phase 2)
#ifndef GUI_DOUBLE_BUFFER
#define GUI_DOUBLE_BUFFER 1
#endif

// Enable DMA transfers (Phase 2)
#ifndef GUI_USE_DMA
#define GUI_USE_DMA 1
#endif

// ============================================================================
// Feature Flags
// ============================================================================

// Enable legacy M5.Display compatibility bridge (deprecated, use GUI_LEGACY_BRIDGE_MODE)
#ifndef GUI_LEGACY_COMPAT
#define GUI_LEGACY_COMPAT 0
#endif

// ============================================================================
// Legacy Bridge Configuration
// ============================================================================
//
// The Legacy Bridge provides a compatibility layer for gradual migration from
// direct M5.Display calls to the new async RenderQueue system.
//
// Modes:
//   0 = DISABLED:    Bridge completely disabled, all calls go to M5.Display directly
//   1 = PASSTHROUGH: Bridge active but routes all calls to M5.Display (for debugging)
//   2 = QUEUED:      All calls routed through RenderQueue (full async rendering)
//   3 = HYBRID:      Urgent calls go direct, others through queue
//
// To fully migrate and remove the bridge:
//   1. Set GUI_LEGACY_BRIDGE_MODE to 0
//   2. Remove LegacyBridge includes and calls from migrated files
//   3. Use GUI::Draw:: namespace for new code
//
#ifndef GUI_LEGACY_BRIDGE_MODE
#define GUI_LEGACY_BRIDGE_MODE 2  // Default: QUEUED for async rendering
#endif

// Emergency rollback switch:
// When enabled, force bridge passthrough at compile-time.
#ifndef GUI_ROLLBACK_FORCE_PASSTHROUGH
#define GUI_ROLLBACK_FORCE_PASSTHROUGH 1
#endif
#if GUI_ROLLBACK_FORCE_PASSTHROUGH
#undef GUI_LEGACY_BRIDGE_MODE
#define GUI_LEGACY_BRIDGE_MODE 1
#endif

// Enable Legacy Bridge debug macros for easier migration
// When enabled, you can use M5_Display_* macros as drop-in replacements
#ifndef GUI_LEGACY_BRIDGE_MACROS
#define GUI_LEGACY_BRIDGE_MACROS 0
#endif

// Enable dirty region tracking (Phase 3)
#ifndef GUI_DIRTY_TRACKING
#define GUI_DIRTY_TRACKING 1
#endif

// Enable partial display updates (Phase 3)
// When enabled, only dirty regions are transferred to display
#ifndef GUI_PARTIAL_UPDATE
#define GUI_PARTIAL_UPDATE 1
#endif

// Emergency rollback switch:
// Disable partial updates and force full framebuffer transfers.
#ifndef GUI_ROLLBACK_DISABLE_PARTIAL_UPDATE
#define GUI_ROLLBACK_DISABLE_PARTIAL_UPDATE 0
#endif
#if GUI_ROLLBACK_DISABLE_PARTIAL_UPDATE
#undef GUI_PARTIAL_UPDATE
#define GUI_PARTIAL_UPDATE 0
#endif

// Enable debug logging
#ifndef GUI_DEBUG
#define GUI_DEBUG 0
#endif

// ============================================================================
// Debug Macros
// ============================================================================

#if GUI_DEBUG
    #include <Arduino.h>
    #define GUI_LOG(fmt, ...) Serial.printf("[GUI] " fmt "\n", ##__VA_ARGS__)
    #define GUI_LOG_ERROR(fmt, ...) Serial.printf("[GUI ERROR] " fmt "\n", ##__VA_ARGS__)
#else
    #define GUI_LOG(fmt, ...)
    #define GUI_LOG_ERROR(fmt, ...)
#endif

// ============================================================================
// Performance Monitoring
// ============================================================================

// Enable frame timing statistics
#ifndef GUI_PERF_STATS
#define GUI_PERF_STATS 0
#endif

#if GUI_PERF_STATS
    struct PerfStats {
        uint32_t framesRendered;
        uint32_t totalRenderTimeUs;
        uint32_t maxRenderTimeUs;
        uint32_t queueOverflows;
        uint32_t avgRenderTimeUs() const {
            return framesRendered > 0 ? totalRenderTimeUs / framesRendered : 0;
        }
    };
#endif

// ============================================================================
// Production Validation / Stress Test Controls
// ============================================================================
//
// Keep disabled in release builds. Enable only for validation firmware.
#ifndef GUI_ENABLE_PRODUCTION_TESTS
#define GUI_ENABLE_PRODUCTION_TESTS 0
#endif

// Default stress-run duration (ms) for on-device validation.
#ifndef GUI_TEST_STRESS_DURATION_MS
#define GUI_TEST_STRESS_DURATION_MS 180000
#endif
constexpr uint32_t TEST_STRESS_DURATION_MS = GUI_TEST_STRESS_DURATION_MS;

// Number of reconnect cycles during stress validation.
#ifndef GUI_TEST_RECONNECT_CYCLES
#define GUI_TEST_RECONNECT_CYCLES 12
#endif
constexpr uint32_t TEST_RECONNECT_CYCLES = GUI_TEST_RECONNECT_CYCLES;

// Number of render commands pushed per burst iteration.
#ifndef GUI_TEST_BURST_OPS
#define GUI_TEST_BURST_OPS 256
#endif
constexpr uint32_t TEST_BURST_OPS = GUI_TEST_BURST_OPS;

// Memory safety thresholds used by validation report.
#ifndef GUI_TEST_HEAP_LEAK_THRESHOLD_BYTES
#define GUI_TEST_HEAP_LEAK_THRESHOLD_BYTES 4096
#endif
constexpr size_t TEST_HEAP_LEAK_THRESHOLD_BYTES = GUI_TEST_HEAP_LEAK_THRESHOLD_BYTES;

#ifndef GUI_TEST_HEAP_FRAGMENT_THRESHOLD_BYTES
#define GUI_TEST_HEAP_FRAGMENT_THRESHOLD_BYTES 8192
#endif
constexpr size_t TEST_HEAP_FRAGMENT_THRESHOLD_BYTES = GUI_TEST_HEAP_FRAGMENT_THRESHOLD_BYTES;

} // namespace Config
} // namespace GUI

#endif // GUI_CONFIG_H

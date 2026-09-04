/*
 * Low-memory scaled compositor for framebuffer-less ESP32-S3 boards.
 *
 * A 4-bpp logical canvas is retained in internal SRAM. The physical panel is
 * treated as the previously presented frame, so only hashes are needed for
 * tile diffing. This keeps the hot rendering path independent from PSRAM.
 */

#ifndef GUI_LOW_MEMORY_COMPOSITOR_H
#define GUI_LOW_MEMORY_COMPOSITOR_H

#include <Arduino.h>
#include <M5Unified.h>
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace GUI {

enum class LowMemoryRenderMode : uint8_t {
    Direct = 0,
    ScaledFull = 1,
    ScaledTiles = 2,
    Auto = 3,
};

struct LowMemoryCompositorConfig {
    LowMemoryRenderMode mode = LowMemoryRenderMode::Auto;
    uint16_t logicalWidth = 240;
    uint16_t logicalHeight = 160;
    uint8_t tileSize = 16;
    uint8_t fullFlushThreshold = 38;
    uint8_t pollIntervalMs = 16;
};

struct LowMemoryCompositorStats {
    uint32_t framesObserved = 0;
    uint32_t framesPresented = 0;
    uint32_t fullFrames = 0;
    uint32_t sparseFrames = 0;
    uint32_t droppedFrames = 0;
    uint32_t tilesCompared = 0;
    uint32_t tilesTransferred = 0;
    uint64_t wireBytes = 0;
    uint32_t lastFlushUs = 0;
    uint8_t lastDirtyPercent = 0;
};

class LowMemoryCompositor {
public:
    static LowMemoryCompositor& instance();

    bool begin(lgfx::LGFX_Device* physical,
               const LowMemoryCompositorConfig& config);
    void end();

    bool active() const { return m_active.load(std::memory_order_acquire); }
    bool workerTaskActive() const { return m_task != nullptr; }
    lgfx::LGFX_Device* canvasDevice() const;
    lgfx::LGFX_Device* physicalDevice() const { return m_physical; }
    const LowMemoryCompositorConfig& config() const { return m_config; }
    const LowMemoryCompositorStats& stats() const { return m_stats; }

    void requestPresent(bool forceFull = false);
    void forceFullRefresh() { requestPresent(true); }
    void service();

    // suspend() is non-blocking. The caller then acquires DisplayLock, which
    // waits for an already-running tile transfer to finish. Nested SD guards
    // are supported.
    void suspend();
    void resume();
    bool suspended() const {
        return m_suspendDepth.load(std::memory_order_acquire) != 0;
    }

    static const char* modeName(LowMemoryRenderMode mode);

private:
    LowMemoryCompositor() = default;
    ~LowMemoryCompositor() = default;
    LowMemoryCompositor(const LowMemoryCompositor&) = delete;
    LowMemoryCompositor& operator=(const LowMemoryCompositor&) = delete;

    static void taskEntry(void* argument);
    void taskLoop();
    bool processFrame();
    bool scanCanvas(uint32_t* hashes, uint32_t timeoutMs = 50);
    void scanCanvasUnlocked(uint32_t* hashes);
    uint32_t hashTile(const uint16_t* pixels, uint16_t width,
                      uint16_t height) const;
    bool flushFull(uint32_t generation);
    bool flushSparse(const uint32_t* expectedHashes, uint32_t generation,
                     uint16_t dirtyCount);
    bool transferTile(uint16_t tileIndex, uint32_t expectedHash);
    void configurePalette();
    void releaseBuffers();
    void notifyWorker();

    LowMemoryCompositorConfig m_config;
    LowMemoryCompositorStats m_stats;
    lgfx::LGFX_Device* m_physical = nullptr;
    LGFX_Sprite* m_canvas = nullptr;

    uint16_t m_physicalWidth = 0;
    uint16_t m_physicalHeight = 0;
    uint16_t m_tileColumns = 0;
    uint16_t m_tileRows = 0;
    uint16_t m_tileCount = 0;

    uint32_t* m_presentedHashes = nullptr;
    uint32_t* m_candidateHashes = nullptr;
    uint32_t* m_scanHashes = nullptr;
    uint16_t* m_tileBuffer = nullptr;
    uint16_t* m_logicalLine = nullptr;
    uint16_t* m_physicalLine = nullptr;

    TaskHandle_t m_task = nullptr;
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_processing{false};
    std::atomic<bool> m_forceFull{false};
    std::atomic<uint32_t> m_suspendDepth{0};
    std::atomic<uint32_t> m_requestedGeneration{0};
    uint32_t m_handledGeneration = 0;
    uint8_t m_candidateStableScans = 0;
};

inline LowMemoryCompositor& lowMemoryCompositor() {
    return LowMemoryCompositor::instance();
}

}  // namespace GUI

#endif  // GUI_LOW_MEMORY_COMPOSITOR_H

/*
 * Low-memory scaled compositor implementation.
 */

#include "gui_low_memory_compositor.h"

#include "gui_display_lock.h"
#include "gui_display_target.h"
#include "../gui_config.h"

#include <algorithm>
#include <cstring>
#include <new>
#include "esp_heap_caps.h"

namespace GUI {
namespace {

constexpr size_t kRequiredHeapReserve = 28U * 1024U;
constexpr uint32_t kFnvOffset = 2166136261UL;
constexpr uint32_t kFnvPrime = 16777619UL;

template <typename T>
T* allocateInternal(size_t count, bool clear = false) {
    if (!count || count > SIZE_MAX / sizeof(T)) return nullptr;
    const size_t bytes = count * sizeof(T);
    return static_cast<T*>(clear
        ? heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        : heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

}  // namespace

LowMemoryCompositor& LowMemoryCompositor::instance() {
    static LowMemoryCompositor compositor;
    return compositor;
}

const char* LowMemoryCompositor::modeName(LowMemoryRenderMode mode) {
    switch (mode) {
        case LowMemoryRenderMode::Direct: return "direct";
        case LowMemoryRenderMode::ScaledFull: return "scaled_full";
        case LowMemoryRenderMode::ScaledTiles: return "scaled_tiles";
        case LowMemoryRenderMode::Auto: return "auto";
        default: return "direct";
    }
}

bool LowMemoryCompositor::begin(lgfx::LGFX_Device* physical,
                                const LowMemoryCompositorConfig& requested) {
    end();
    if (!physical || requested.mode == LowMemoryRenderMode::Direct) return false;

    m_config = requested;
    if (m_config.logicalWidth < 64) m_config.logicalWidth = 240;
    if (m_config.logicalHeight < 64) m_config.logicalHeight = 160;
    if (m_config.tileSize != 8 && m_config.tileSize != 16 &&
        m_config.tileSize != 32)
        m_config.tileSize = 16;
    if (m_config.fullFlushThreshold < 1 ||
        m_config.fullFlushThreshold > 100)
        m_config.fullFlushThreshold = 38;
    if (m_config.pollIntervalMs < 4) m_config.pollIntervalMs = 4;

    m_physical = physical;
    m_physicalWidth = static_cast<uint16_t>(physical->width());
    m_physicalHeight = static_cast<uint16_t>(physical->height());
    if (!m_physicalWidth || !m_physicalHeight) {
        m_physical = nullptr;
        return false;
    }

    m_tileColumns = static_cast<uint16_t>(
        (m_config.logicalWidth + m_config.tileSize - 1) / m_config.tileSize);
    m_tileRows = static_cast<uint16_t>(
        (m_config.logicalHeight + m_config.tileSize - 1) / m_config.tileSize);
    const uint32_t tileCount =
        static_cast<uint32_t>(m_tileColumns) * m_tileRows;
    if (!tileCount || tileCount > 1024) {
        m_physical = nullptr;
        return false;
    }
    m_tileCount = static_cast<uint16_t>(tileCount);

    const size_t canvasBytes =
        (static_cast<size_t>(m_config.logicalWidth) *
         m_config.logicalHeight + 1U) / 2U;
    const size_t freeInternal = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largestInternal = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (freeInternal < canvasBytes + kRequiredHeapReserve ||
        largestInternal < canvasBytes) {
        GUI_LOG_ERROR("LowMemCompositor: canvas needs %u bytes; free=%u "
                      "largest=%u", static_cast<unsigned>(canvasBytes),
                      static_cast<unsigned>(freeInternal),
                      static_cast<unsigned>(largestInternal));
        m_physical = nullptr;
        return false;
    }

    m_canvas = new (std::nothrow) LGFX_Sprite(physical);
    if (!m_canvas) {
        m_physical = nullptr;
        return false;
    }
    m_canvas->setPsram(false);
    m_canvas->setColorDepth(4);
    if (!m_canvas->createSprite(m_config.logicalWidth,
                                m_config.logicalHeight)) {
        GUI_LOG_ERROR("LowMemCompositor: 4-bpp canvas allocation failed");
        releaseBuffers();
        return false;
    }
    configurePalette();
    m_canvas->fillScreen(0x0000);

    m_presentedHashes = allocateInternal<uint32_t>(m_tileCount, true);
    m_candidateHashes = allocateInternal<uint32_t>(m_tileCount, true);
    m_scanHashes = allocateInternal<uint32_t>(m_tileCount, true);
    m_tileBuffer = allocateInternal<uint16_t>(
        static_cast<size_t>(m_config.tileSize) * m_config.tileSize);
    m_logicalLine = allocateInternal<uint16_t>(m_config.logicalWidth);
    m_physicalLine = allocateInternal<uint16_t>(m_physicalWidth);
    if (!m_presentedHashes || !m_candidateHashes || !m_scanHashes ||
        !m_tileBuffer || !m_logicalLine || !m_physicalLine) {
        GUI_LOG_ERROR("LowMemCompositor: working-buffer allocation failed");
        releaseBuffers();
        return false;
    }

    if (!setRuntimeCanvasDisplay(m_canvas, 4)) {
        GUI_LOG_ERROR("LowMemCompositor: runtime canvas activation failed");
        releaseBuffers();
        return false;
    }

    m_stats = {};
    m_handledGeneration = 0;
    m_candidateStableScans = 0;
    m_suspendDepth.store(0, std::memory_order_release);
    m_requestedGeneration.store(1, std::memory_order_release);
    m_forceFull.store(true, std::memory_order_release);
    m_active.store(true, std::memory_order_release);

    if (xTaskCreatePinnedToCore(taskEntry, "LowMemRender", 4096, this, 2,
                                &m_task, 0) != pdPASS) {
        m_task = nullptr;
        GUI_LOG_ERROR("LowMemCompositor: worker task unavailable; using "
                      "cooperative presents");
    }

    GUI_LOG("LowMemCompositor active: mode=%s logical=%ux%u physical=%ux%u "
            "tile=%u count=%u threshold=%u%% heap=%u largest=%u",
            modeName(m_config.mode), m_config.logicalWidth,
            m_config.logicalHeight, m_physicalWidth, m_physicalHeight,
            m_config.tileSize, m_tileCount, m_config.fullFlushThreshold,
            static_cast<unsigned>(heap_caps_get_free_size(
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    notifyWorker();
    return true;
}

void LowMemoryCompositor::end() {
    const bool wasActive = m_active.exchange(false, std::memory_order_acq_rel);
    if (m_task) {
        xTaskNotifyGive(m_task);
        for (uint16_t i = 0; i < 250 && m_task; ++i) vTaskDelay(1);
        if (m_task) {
            vTaskDelete(m_task);
            m_task = nullptr;
        }
    }
    while (m_processing.load(std::memory_order_acquire)) vTaskDelay(1);
    if (wasActive || m_canvas) restoreRuntimePhysicalDisplay();
    releaseBuffers();
    m_physical = nullptr;
    m_physicalWidth = 0;
    m_physicalHeight = 0;
    m_tileColumns = 0;
    m_tileRows = 0;
    m_tileCount = 0;
    m_suspendDepth.store(0, std::memory_order_release);
}

lgfx::LGFX_Device* LowMemoryCompositor::canvasDevice() const {
    return active() ? static_cast<lgfx::LGFX_Device*>(m_canvas) : nullptr;
}

void LowMemoryCompositor::releaseBuffers() {
    if (m_canvas) {
        m_canvas->deleteSprite();
        delete m_canvas;
        m_canvas = nullptr;
    }
    heap_caps_free(m_presentedHashes);
    heap_caps_free(m_candidateHashes);
    heap_caps_free(m_scanHashes);
    heap_caps_free(m_tileBuffer);
    heap_caps_free(m_logicalLine);
    heap_caps_free(m_physicalLine);
    m_presentedHashes = nullptr;
    m_candidateHashes = nullptr;
    m_scanHashes = nullptr;
    m_tileBuffer = nullptr;
    m_logicalLine = nullptr;
    m_physicalLine = nullptr;
}

void LowMemoryCompositor::configurePalette() {
    static constexpr uint16_t palette[16] = {
        0x0000, 0xFFFF, 0x4208, 0xC618,
        0xF800, 0x07E0, 0x001F, 0x07FF,
        0xFFE0, 0xFD20, 0xF81F, 0x000F,
        0x0320, 0x7800, 0x7BE0, 0x8410,
    };
    for (size_t i = 0; i < 16; ++i) m_canvas->setPaletteColor(i, palette[i]);
}

void LowMemoryCompositor::notifyWorker() {
    TaskHandle_t task = m_task;
    if (task) xTaskNotifyGive(task);
}

void LowMemoryCompositor::requestPresent(bool forceFull) {
    if (!active()) return;
    if (forceFull) m_forceFull.store(true, std::memory_order_release);
    m_requestedGeneration.fetch_add(1, std::memory_order_acq_rel);
    if (m_task)
        notifyWorker();
    else
        service();
}

void LowMemoryCompositor::suspend() {
    if (!active()) return;
    m_suspendDepth.fetch_add(1, std::memory_order_acq_rel);
    notifyWorker();
}

void LowMemoryCompositor::resume() {
    uint32_t depth = m_suspendDepth.load(std::memory_order_acquire);
    while (depth && !m_suspendDepth.compare_exchange_weak(
                        depth, depth - 1, std::memory_order_acq_rel)) {}
    if (depth == 1) notifyWorker();
}

void LowMemoryCompositor::taskEntry(void* argument) {
    static_cast<LowMemoryCompositor*>(argument)->taskLoop();
}

void LowMemoryCompositor::taskLoop() {
    while (active()) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(m_config.pollIntervalMs));
        if (!active()) break;
        service();
    }
    m_task = nullptr;
    vTaskDelete(nullptr);
}

void LowMemoryCompositor::service() {
    if (!active() || suspended()) return;
    bool expected = false;
    if (!m_processing.compare_exchange_strong(expected, true,
                                               std::memory_order_acq_rel))
        return;
    processFrame();
    m_processing.store(false, std::memory_order_release);
}

uint32_t LowMemoryCompositor::hashTile(const uint16_t* pixels,
                                       uint16_t width,
                                       uint16_t height) const {
    uint32_t hash = kFnvOffset;
    hash = (hash ^ width) * kFnvPrime;
    hash = (hash ^ height) * kFnvPrime;
    const size_t count = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < count; ++i) {
        hash = (hash ^ static_cast<uint8_t>(pixels[i])) * kFnvPrime;
        hash = (hash ^ static_cast<uint8_t>(pixels[i] >> 8)) * kFnvPrime;
    }
    return hash ? hash : 1;
}

void LowMemoryCompositor::scanCanvasUnlocked(uint32_t* hashes) {
    for (uint16_t row = 0; row < m_tileRows; ++row) {
        const uint16_t y = row * m_config.tileSize;
        const uint16_t height = std::min<uint16_t>(
            m_config.tileSize, m_config.logicalHeight - y);
        for (uint16_t column = 0; column < m_tileColumns; ++column) {
            const uint16_t x = column * m_config.tileSize;
            const uint16_t width = std::min<uint16_t>(
                m_config.tileSize, m_config.logicalWidth - x);
            m_canvas->readRect(x, y, width, height, m_tileBuffer);
            hashes[row * m_tileColumns + column] =
                hashTile(m_tileBuffer, width, height);
        }
    }
}

bool LowMemoryCompositor::scanCanvas(uint32_t* hashes, uint32_t timeoutMs) {
    DisplayLockGuard guard(timeoutMs);
    if (!guard.locked() || suspended()) return false;
    scanCanvasUnlocked(hashes);
    return true;
}

bool LowMemoryCompositor::processFrame() {
    if (!scanCanvas(m_scanHashes)) return false;
    ++m_stats.framesObserved;
    m_stats.tilesCompared += m_tileCount;

    const uint32_t generation =
        m_requestedGeneration.load(std::memory_order_acquire);
    const bool explicitFrame = generation != m_handledGeneration;
    const bool candidateChanged =
        memcmp(m_candidateHashes, m_scanHashes,
               static_cast<size_t>(m_tileCount) * sizeof(uint32_t)) != 0;
    if (candidateChanged) {
        memcpy(m_candidateHashes, m_scanHashes,
               static_cast<size_t>(m_tileCount) * sizeof(uint32_t));
        m_candidateStableScans = 1;
        if (!explicitFrame) return true;
    } else if (!explicitFrame && m_candidateStableScans < 2) {
        ++m_candidateStableScans;
        if (m_candidateStableScans < 2) return true;
    }

    const bool forceFull = m_forceFull.exchange(false,
                                                 std::memory_order_acq_rel);
    uint16_t dirtyCount = 0;
    for (uint16_t i = 0; i < m_tileCount; ++i) {
        if (forceFull || m_candidateHashes[i] != m_presentedHashes[i])
            ++dirtyCount;
    }
    if (!dirtyCount) {
        m_handledGeneration = generation;
        return true;
    }

    const uint8_t dirtyPercent = static_cast<uint8_t>(
        (static_cast<uint32_t>(dirtyCount) * 100U + m_tileCount - 1U) /
        m_tileCount);
    m_stats.lastDirtyPercent = dirtyPercent;

    bool full = forceFull || m_config.mode == LowMemoryRenderMode::ScaledFull;
    if (m_config.mode == LowMemoryRenderMode::Auto &&
        dirtyPercent >= m_config.fullFlushThreshold)
        full = true;
    if (m_config.mode == LowMemoryRenderMode::ScaledTiles) full = false;

    const uint32_t started = micros();
    const bool success = full
        ? flushFull(generation)
        : flushSparse(m_candidateHashes, generation, dirtyCount);
    m_stats.lastFlushUs = micros() - started;
    if (!success) {
        ++m_stats.droppedFrames;
        m_forceFull.store(full, std::memory_order_release);
        notifyWorker();
        return false;
    }

    ++m_stats.framesPresented;
    if (full)
        ++m_stats.fullFrames;
    else
        ++m_stats.sparseFrames;
    m_handledGeneration = generation;
    m_candidateStableScans = 2;
    if (m_requestedGeneration.load(std::memory_order_acquire) != generation) {
        ++m_stats.droppedFrames;
        notifyWorker();
    }
    return true;
}

bool LowMemoryCompositor::flushFull(uint32_t generation) {
    if (suspended() ||
        m_requestedGeneration.load(std::memory_order_acquire) != generation)
        return false;
    DisplayLockGuard guard(250);
    if (!guard.locked() || suspended()) return false;

    m_physical->startWrite();
    m_physical->setAddrWindow(0, 0, m_physicalWidth, m_physicalHeight);
    int32_t loadedSourceY = -1;
    for (uint16_t destinationY = 0; destinationY < m_physicalHeight;
         ++destinationY) {
        const uint16_t sourceY = static_cast<uint16_t>(
            (static_cast<uint32_t>(destinationY) * m_config.logicalHeight) /
            m_physicalHeight);
        if (sourceY != loadedSourceY) {
            m_canvas->readRect(0, sourceY, m_config.logicalWidth, 1,
                               m_logicalLine);
            loadedSourceY = sourceY;
        }
        for (uint16_t destinationX = 0; destinationX < m_physicalWidth;
             ++destinationX) {
            const uint16_t sourceX = static_cast<uint16_t>(
                (static_cast<uint32_t>(destinationX) *
                 m_config.logicalWidth) / m_physicalWidth);
            m_physicalLine[destinationX] = m_logicalLine[sourceX];
        }
        m_physical->writePixels(m_physicalLine, m_physicalWidth);
    }
    m_physical->endWrite();

    scanCanvasUnlocked(m_scanHashes);
    memcpy(m_presentedHashes, m_scanHashes,
           static_cast<size_t>(m_tileCount) * sizeof(uint32_t));
    memcpy(m_candidateHashes, m_scanHashes,
           static_cast<size_t>(m_tileCount) * sizeof(uint32_t));
    m_stats.tilesTransferred += m_tileCount;
    m_stats.wireBytes +=
        static_cast<uint64_t>(m_physicalWidth) * m_physicalHeight * 3U;
    return true;
}

bool LowMemoryCompositor::transferTile(uint16_t tileIndex,
                                       uint32_t expectedHash) {
    const uint16_t tileRow = tileIndex / m_tileColumns;
    const uint16_t tileColumn = tileIndex % m_tileColumns;
    const uint16_t sourceX = tileColumn * m_config.tileSize;
    const uint16_t sourceY = tileRow * m_config.tileSize;
    const uint16_t sourceWidth = std::min<uint16_t>(
        m_config.tileSize, m_config.logicalWidth - sourceX);
    const uint16_t sourceHeight = std::min<uint16_t>(
        m_config.tileSize, m_config.logicalHeight - sourceY);
    m_canvas->readRect(sourceX, sourceY, sourceWidth, sourceHeight,
                       m_tileBuffer);
    const uint32_t actualHash =
        hashTile(m_tileBuffer, sourceWidth, sourceHeight);
    if (actualHash != expectedHash) return false;

    const uint16_t destinationX0 = static_cast<uint16_t>(
        (static_cast<uint32_t>(sourceX) * m_physicalWidth) /
        m_config.logicalWidth);
    const uint16_t destinationY0 = static_cast<uint16_t>(
        (static_cast<uint32_t>(sourceY) * m_physicalHeight) /
        m_config.logicalHeight);
    const uint16_t destinationX1 = static_cast<uint16_t>(
        (static_cast<uint32_t>(sourceX + sourceWidth) * m_physicalWidth) /
        m_config.logicalWidth);
    const uint16_t destinationY1 = static_cast<uint16_t>(
        (static_cast<uint32_t>(sourceY + sourceHeight) * m_physicalHeight) /
        m_config.logicalHeight);
    const uint16_t destinationWidth = destinationX1 - destinationX0;
    const uint16_t destinationHeight = destinationY1 - destinationY0;
    if (!destinationWidth || !destinationHeight) return true;

    m_physical->startWrite();
    m_physical->setAddrWindow(destinationX0, destinationY0,
                              destinationWidth, destinationHeight);
    for (uint16_t y = destinationY0; y < destinationY1; ++y) {
        uint16_t logicalY = static_cast<uint16_t>(
            (static_cast<uint32_t>(y) * m_config.logicalHeight) /
            m_physicalHeight);
        if (logicalY < sourceY) logicalY = sourceY;
        if (logicalY >= sourceY + sourceHeight)
            logicalY = sourceY + sourceHeight - 1;
        const uint16_t localY = logicalY - sourceY;
        for (uint16_t x = destinationX0; x < destinationX1; ++x) {
            uint16_t logicalX = static_cast<uint16_t>(
                (static_cast<uint32_t>(x) * m_config.logicalWidth) /
                m_physicalWidth);
            if (logicalX < sourceX) logicalX = sourceX;
            if (logicalX >= sourceX + sourceWidth)
                logicalX = sourceX + sourceWidth - 1;
            m_physicalLine[x - destinationX0] =
                m_tileBuffer[static_cast<size_t>(localY) * sourceWidth +
                             logicalX - sourceX];
        }
        m_physical->writePixels(m_physicalLine, destinationWidth);
    }
    m_physical->endWrite();

    m_presentedHashes[tileIndex] = actualHash;
    ++m_stats.tilesTransferred;
    m_stats.wireBytes +=
        static_cast<uint64_t>(destinationWidth) * destinationHeight * 3U;
    return true;
}

bool LowMemoryCompositor::flushSparse(const uint32_t* expectedHashes,
                                      uint32_t generation,
                                      uint16_t dirtyCount) {
    (void)dirtyCount;
    if (suspended() ||
        m_requestedGeneration.load(std::memory_order_acquire) != generation)
        return false;
    DisplayLockGuard guard(250);
    if (!guard.locked() || suspended()) return false;

    for (uint16_t i = 0; i < m_tileCount; ++i) {
        if (expectedHashes[i] == m_presentedHashes[i]) continue;
        if (m_requestedGeneration.load(std::memory_order_acquire) != generation)
            return false;
        if (!transferTile(i, expectedHashes[i])) return false;
    }
    return true;
}

}  // namespace GUI

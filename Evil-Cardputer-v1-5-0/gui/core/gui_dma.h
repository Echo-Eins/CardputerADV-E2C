/*
 * GUI DMA - Asynchronous SPI DMA transfer for display updates
 *
 * Uses ESP32 SPI DMA to transfer framebuffer data to the ST7789V display
 * without blocking the CPU. The renderer can continue processing commands
 * while DMA transfers the previous frame.
 *
 * Transfer modes:
 * - Full frame: Transfer entire framebuffer (64,800 bytes)
 * - Partial: Transfer only dirty regions (future optimization)
 *
 * Integration with M5GFX:
 * - Uses M5GFX's internal SPI bus
 * - Coordinates with M5GFX for display control commands
 * - Falls back to blocking transfer if DMA unavailable
 */

#ifndef GUI_DMA_H
#define GUI_DMA_H

#include "../gui_types.h"
#include "../gui_config.h"
#include "gui_framebuffer.h"
#include <cstdint>
#include <atomic>

// ESP32 SPI and DMA
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"

// FreeRTOS for synchronization
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace GUI {

// ============================================================================
// DMA Transfer State
// ============================================================================

enum class DmaState : uint8_t {
    Idle = 0,           // No transfer in progress
    Preparing,          // Setting up transfer
    Transferring,       // DMA active
    Completing,         // Transfer done, finalizing
    Error               // Transfer failed
};

// ============================================================================
// DMA Statistics
// ============================================================================

struct DmaStats {
    uint32_t transferCount;         // Total transfers initiated
    uint64_t bytesTransferred;      // Total bytes sent
    uint64_t totalTransferTimeUs;   // Cumulative transfer time
    uint32_t maxTransferTimeUs;     // Worst-case transfer time
    uint32_t errorCount;            // Transfer errors

    void reset() {
        transferCount = 0;
        bytesTransferred = 0;
        totalTransferTimeUs = 0;
        maxTransferTimeUs = 0;
        errorCount = 0;
    }

    uint32_t avgTransferTimeUs() const {
        return transferCount > 0
            ? static_cast<uint32_t>(totalTransferTimeUs / transferCount) : 0;
    }

    // Calculate effective bandwidth in KB/s
    uint32_t bandwidthKBps() const {
        if (totalTransferTimeUs == 0) return 0;
        return static_cast<uint32_t>(
            (bytesTransferred * 1000000) /
            (totalTransferTimeUs * 1024)
        );
    }
};

// ============================================================================
// DMA Transfer Manager
// ============================================================================

class DmaTransfer {
public:
    // Singleton access
    static DmaTransfer& instance();

    // ========================================================================
    // Lifecycle
    // ========================================================================

    // Initialize DMA subsystem
    // Must be called after M5.begin() and display initialization
    bool init();

    // Shutdown DMA
    void shutdown();

    // Check if initialized and available
    bool isInitialized() const { return m_initialized; }
    bool isAvailable() const { return m_initialized && m_dmaAvailable; }

    // ========================================================================
    // Transfer Operations
    // ========================================================================

    // Start async transfer of full framebuffer
    // Returns immediately; use waitComplete() or callback
    bool startFullTransfer(const uint16_t* buffer, uint32_t size);

    // Start async transfer of partial region
    bool startPartialTransfer(const uint16_t* buffer,
                              int16_t x, int16_t y,
                              uint16_t width, uint16_t height);

    // Wait for current transfer to complete (blocking)
    void waitComplete(uint32_t timeoutMs = 1000);

    // Check if transfer is in progress
    bool isTransferring() const {
        return m_state.load(std::memory_order_acquire) == DmaState::Transferring;
    }

    // Get current state
    DmaState getState() const { return m_state.load(std::memory_order_acquire); }

    // ========================================================================
    // Callback Interface
    // ========================================================================

    // Callback function type for transfer completion
    using TransferCallback = void (*)(bool success, void* userData);

    // Set callback for transfer completion
    void setCallback(TransferCallback callback, void* userData = nullptr);

    // ========================================================================
    // Fallback Mode
    // ========================================================================

    // Perform blocking transfer (fallback when DMA unavailable)
    void blockingTransfer(const uint16_t* buffer, uint32_t size);

    // Perform blocking partial transfer
    void blockingPartialTransfer(const uint16_t* buffer,
                                  int16_t x, int16_t y,
                                  uint16_t width, uint16_t height);

    // ========================================================================
    // Configuration
    // ========================================================================

    // Enable/disable DMA (falls back to blocking if disabled)
    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

    // ========================================================================
    // Statistics
    // ========================================================================

    DmaStats getStats() const {
        portENTER_CRITICAL_SAFE(&s_statsLock);
        DmaStats copy = m_stats;
        portEXIT_CRITICAL_SAFE(&s_statsLock);
        return copy;
    }
    void resetStats() { m_stats.reset(); }

private:
    DmaTransfer();
    ~DmaTransfer();

    // Prevent copying
    DmaTransfer(const DmaTransfer&) = delete;
    DmaTransfer& operator=(const DmaTransfer&) = delete;

    // Internal transfer completion handler
    static void onTransferComplete(void* userData);
    void handleTransferComplete(bool success, uint32_t bytesTransferred = 0);

    // Set display window for partial transfer
    void setDisplayWindow(int16_t x, int16_t y, uint16_t width, uint16_t height);

    // ========================================================================
    // State
    // ========================================================================

    std::atomic<DmaState> m_state{DmaState::Idle};

    // Transfer synchronization
    SemaphoreHandle_t m_completeSemaphore;

    // Callback
    TransferCallback m_callback;
    void* m_callbackUserData;

    // Timing for current transfer
    uint32_t m_transferStartUs;

    // Statistics (guarded by s_statsLock for cross-core reads)
    DmaStats m_stats;
    static portMUX_TYPE s_statsLock;

    // Configuration
    bool m_enabled;
    bool m_dmaAvailable;
    bool m_initialized;
};

// ============================================================================
// High-Level Display Update Interface
// ============================================================================

class DisplayUpdater {
public:
    // Singleton access
    static DisplayUpdater& instance();

    // Initialize (after Framebuffer and DmaTransfer are initialized)
    bool init();
    void shutdown();

    // ========================================================================
    // Update Methods
    // ========================================================================

    // Push framebuffer to display using best available method
    // - If DMA available: async transfer
    // - Otherwise: blocking M5GFX pushImage
    void pushFramebuffer();

    // Push and wait for completion
    void pushFramebufferSync();

    // Push partial region
    void pushRegion(const Rect& region);

    // ========================================================================
    // Configuration
    // ========================================================================

    // Set whether to use DMA or blocking transfers
    void setUseDma(bool useDma) { m_useDma = useDma; }
    bool getUseDma() const { return m_useDma; }

    // Set whether to use double buffering
    void setUseDoubleBuffer(bool useDouble) { m_useDoubleBuffer = useDouble; }
    bool getUseDoubleBuffer() const { return m_useDoubleBuffer; }

private:
    DisplayUpdater();
    ~DisplayUpdater();

    DisplayUpdater(const DisplayUpdater&) = delete;
    DisplayUpdater& operator=(const DisplayUpdater&) = delete;

    bool m_useDma;
    bool m_useDoubleBuffer;
    bool m_initialized;
};

} // namespace GUI

#endif // GUI_DMA_H

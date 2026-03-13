#line 1 "C:\\Coding\\CardputerADV-E2C\\Evil-Cardputer-v1-5-0\\gui\\core\\gui_dirty_region.h"
/*
 * GUI Dirty Region Tracker - Phase 3
 *
 * Optimizes display updates by tracking which regions of the framebuffer
 * have changed and need to be transferred to the display.
 *
 * Strategies:
 * 1. Grid-based tracking: Screen divided into tiles (e.g., 16x16 pixels)
 * 2. Rectangle merging: Combine overlapping dirty regions
 * 3. Area-based decision: Full refresh if dirty area exceeds threshold
 *
 * Integration:
 * - Framebuffer marks regions dirty on write operations
 * - Renderer queries dirty regions before display flush
 * - DMA transfers only changed regions (partial updates)
 *
 * Memory usage:
 * - 240x135 display with 16x16 tiles = 15x9 = 135 bits = ~17 bytes
 * - Plus region list: 8 regions * 8 bytes = 64 bytes
 * - Total: ~100 bytes SRAM
 */

#ifndef GUI_DIRTY_REGION_H
#define GUI_DIRTY_REGION_H

#include "../gui_types.h"
#include "../gui_config.h"
#include <cstdint>
#include <atomic>

namespace GUI {

// ============================================================================
// Configuration
// ============================================================================

// Tile size for grid-based tracking (must be power of 2)
#ifndef GUI_DIRTY_TILE_SIZE
#define GUI_DIRTY_TILE_SIZE 16
#endif

// Maximum number of dirty rectangles to track before merging
#ifndef GUI_MAX_DIRTY_RECTS
#define GUI_MAX_DIRTY_RECTS 8
#endif

// Threshold: if dirty area exceeds this percentage, do full refresh
#ifndef GUI_DIRTY_FULL_REFRESH_THRESHOLD
#define GUI_DIRTY_FULL_REFRESH_THRESHOLD 60
#endif

// Minimum rectangle size worth doing partial update (pixels)
#ifndef GUI_DIRTY_MIN_PARTIAL_SIZE
#define GUI_DIRTY_MIN_PARTIAL_SIZE 64
#endif

// ============================================================================
// Tile Grid Constants
// ============================================================================

namespace DirtyConfig {
    constexpr uint8_t TILE_SIZE = GUI_DIRTY_TILE_SIZE;
    constexpr uint8_t TILE_SHIFT = (TILE_SIZE == 8) ? 3 :
                                   (TILE_SIZE == 16) ? 4 :
                                   (TILE_SIZE == 32) ? 5 : 4;

    // Maximum grid dimensions (actual runtime grid is selected per active display).
    constexpr uint8_t GRID_MAX_WIDTH = (Config::DISPLAY_WIDTH + TILE_SIZE - 1) / TILE_SIZE;
    constexpr uint8_t GRID_MAX_HEIGHT = (Config::DISPLAY_HEIGHT + TILE_SIZE - 1) / TILE_SIZE;
    constexpr uint16_t GRID_MAX_TOTAL = GRID_MAX_WIDTH * GRID_MAX_HEIGHT;

    // Bitmap size for tile grid (in bytes)
    constexpr uint8_t BITMAP_SIZE = (GRID_MAX_TOTAL + 7) / 8;

    static_assert(GRID_MAX_WIDTH <= 64, "Grid width must fit in bitmap index type");
}

// ============================================================================
// Dirty Region Statistics
// ============================================================================

struct DirtyRegionStats {
    uint32_t markDirtyCount;        // Number of times markDirty was called
    uint32_t fullRefreshCount;      // Times we did full refresh
    uint32_t partialRefreshCount;   // Times we did partial refresh
    uint32_t totalDirtyPixels;      // Cumulative dirty pixels
    uint32_t savedPixels;           // Pixels saved by partial updates

    void reset() {
        markDirtyCount = 0;
        fullRefreshCount = 0;
        partialRefreshCount = 0;
        totalDirtyPixels = 0;
        savedPixels = 0;
    }

    // Calculate efficiency (percentage of pixels saved)
    float efficiency() const {
        uint32_t totalPixels = totalDirtyPixels + savedPixels;
        return totalPixels > 0 ? (savedPixels * 100.0f / totalPixels) : 0.0f;
    }
};

// ============================================================================
// Dirty Region Entry
// ============================================================================

struct DirtyRect {
    Rect rect;
    uint8_t priority;   // Higher = more urgent
    bool valid;

    DirtyRect() : priority(0), valid(false) {
        rect = Rect::make(0, 0, 0, 0);
    }

    void set(const Rect& r, uint8_t p = 0) {
        rect = r;
        priority = p;
        valid = true;
    }

    void clear() {
        rect = Rect::make(0, 0, 0, 0);
        valid = false;
        priority = 0;
    }

    uint32_t area() const {
        return valid ? rect.area() : 0;
    }
};

// ============================================================================
// Dirty Region Tracker Class
// ============================================================================

class DirtyRegionTracker {
public:
    // Singleton access
    static DirtyRegionTracker& instance();

    // ========================================================================
    // Lifecycle
    // ========================================================================

    // Initialize tracker
    bool init();

    // Shutdown
    void shutdown();

    // Check if initialized
    bool isInitialized() const { return m_initialized; }

    // ========================================================================
    // Dirty Marking
    // ========================================================================

    // Mark entire screen as dirty
    void markAllDirty();

    // Mark everything as clean
    void markAllClean();

    // Mark a rectangle as dirty
    void markDirty(const Rect& rect);

    // Mark a single pixel as dirty
    void markDirty(int16_t x, int16_t y);

    // Mark a point/size region as dirty
    void markDirty(int16_t x, int16_t y, uint16_t w, uint16_t h);

    // ========================================================================
    // Query
    // ========================================================================

    // Check if any region is dirty
    bool isDirty() const;

    // Check if specific rectangle overlaps dirty regions
    bool isDirty(const Rect& rect) const;

    // Check if specific tile is dirty
    bool isTileDirty(uint8_t tileX, uint8_t tileY) const;

    // Get count of dirty tiles
    uint16_t dirtyTileCount() const;

    // Get dirty area percentage (0-100)
    uint8_t dirtyPercentage() const;

    // Should do full refresh instead of partial?
    bool shouldFullRefresh() const;

    // ========================================================================
    // Region Access
    // ========================================================================

    // Get merged dirty rectangle (bounding box of all dirty regions)
    Rect getMergedDirtyRect() const;

    // Get list of optimized dirty rectangles for partial updates
    // Returns number of rectangles filled (0-count)
    uint8_t getDirtyRects(DirtyRect* rects, uint8_t maxRects) const;

    // Get single optimal dirty rect (best balance of coverage vs. size)
    Rect getOptimalDirtyRect() const;

    // ========================================================================
    // Tile Grid Access
    // ========================================================================

    // Get grid dimensions
    uint8_t gridWidth() const { return m_gridWidth; }
    uint8_t gridHeight() const { return m_gridHeight; }
    uint8_t tileSize() const { return DirtyConfig::TILE_SIZE; }
    uint16_t screenWidth() const { return m_screenWidth; }
    uint16_t screenHeight() const { return m_screenHeight; }

    // Get tile coordinates for pixel
    static uint8_t pixelToTileX(int16_t x) {
        return static_cast<uint8_t>(x >> DirtyConfig::TILE_SHIFT);
    }
    static uint8_t pixelToTileY(int16_t y) {
        return static_cast<uint8_t>(y >> DirtyConfig::TILE_SHIFT);
    }

    // Get pixel coordinates for tile
    static int16_t tileToPixelX(uint8_t tileX) {
        return static_cast<int16_t>(tileX) << DirtyConfig::TILE_SHIFT;
    }
    static int16_t tileToPixelY(uint8_t tileY) {
        return static_cast<int16_t>(tileY) << DirtyConfig::TILE_SHIFT;
    }

    // ========================================================================
    // Statistics
    // ========================================================================

    const DirtyRegionStats& getStats() const { return m_stats; }
    DirtyRegionStats& stats() { return m_stats; }
    void resetStats() { m_stats.reset(); }

    // Increment stats counters
    void incrementFullRefresh() { m_stats.fullRefreshCount++; }
    void incrementPartialRefresh() { m_stats.partialRefreshCount++; }
    void addSavedPixels(uint32_t pixels) { m_stats.savedPixels += pixels; }

    // ========================================================================
    // Configuration
    // ========================================================================

    // Set full refresh threshold (0-100 percent)
    void setFullRefreshThreshold(uint8_t percent);
    uint8_t getFullRefreshThreshold() const { return m_fullRefreshThreshold; }

    // Enable/disable dirty tracking
    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

private:
    DirtyRegionTracker();
    ~DirtyRegionTracker();

    // Prevent copying
    DirtyRegionTracker(const DirtyRegionTracker&) = delete;
    DirtyRegionTracker& operator=(const DirtyRegionTracker&) = delete;

    // ========================================================================
    // Internal Methods
    // ========================================================================

    // Set/clear tile in bitmap
    void setTile(uint8_t tileX, uint8_t tileY);
    void clearTile(uint8_t tileX, uint8_t tileY);

    // Mark tiles covered by rectangle
    void markTilesInRect(const Rect& rect);

    // Calculate grid index
    uint16_t tileIndex(uint8_t tileX, uint8_t tileY) const {
        return static_cast<uint16_t>(tileY) * m_gridWidth + tileX;
    }

    // Get bit position in bitmap
    static void getBitPosition(uint16_t index, uint8_t& byteIdx, uint8_t& bitIdx) {
        byteIdx = index >> 3;
        bitIdx = index & 7;
    }

    // Optimize dirty rectangles (merge overlapping, sort by priority)
    void optimizeRects();

    // ========================================================================
    // State
    // ========================================================================

    // Tile bitmap (1 bit per tile)
    uint8_t m_tileBitmap[DirtyConfig::BITMAP_SIZE];

    // Active screen/grid dimensions (runtime-selected display profile).
    uint8_t m_gridWidth;
    uint8_t m_gridHeight;
    uint16_t m_gridTotal;
    uint16_t m_screenWidth;
    uint16_t m_screenHeight;

    // Cached dirty rectangle list (for optimization)
    DirtyRect m_dirtyRects[GUI_MAX_DIRTY_RECTS];
    uint8_t m_dirtyRectCount;

    // Merged bounding box of all dirty regions
    Rect m_mergedRect;

    // Count of dirty tiles (cached for performance)
    std::atomic<uint16_t> m_dirtyTileCount{0};

    // Configuration
    uint8_t m_fullRefreshThreshold;
    bool m_enabled;
    bool m_initialized;

    // Statistics
    DirtyRegionStats m_stats;
};

// ============================================================================
// Global Access Function
// ============================================================================

inline DirtyRegionTracker& dirtyTracker() {
    return DirtyRegionTracker::instance();
}

} // namespace GUI

#endif // GUI_DIRTY_REGION_H

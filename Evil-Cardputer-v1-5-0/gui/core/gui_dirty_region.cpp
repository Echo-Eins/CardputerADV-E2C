/*
 * GUI Dirty Region Tracker - Phase 3 Implementation
 *
 * Grid-based dirty region tracking for optimized partial display updates.
 */

#include "gui_dirty_region.h"
#include <cstring>
#include <algorithm>

namespace GUI {

// ============================================================================
// Singleton Instance
// ============================================================================

DirtyRegionTracker& DirtyRegionTracker::instance() {
    static DirtyRegionTracker instance;
    return instance;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

DirtyRegionTracker::DirtyRegionTracker()
    : m_dirtyRectCount(0)
    , m_mergedRect(Rect::make(0, 0, 0, 0))
    , m_fullRefreshThreshold(GUI_DIRTY_FULL_REFRESH_THRESHOLD)
    , m_enabled(true)
    , m_initialized(false)
{
    memset(m_tileBitmap, 0, sizeof(m_tileBitmap));
    m_stats.reset();
}

DirtyRegionTracker::~DirtyRegionTracker() {
    shutdown();
}

// ============================================================================
// Lifecycle
// ============================================================================

bool DirtyRegionTracker::init() {
    if (m_initialized) {
        return true;
    }

    // Clear all state
    memset(m_tileBitmap, 0, sizeof(m_tileBitmap));
    m_dirtyRectCount = 0;
    m_dirtyTileCount.store(0, std::memory_order_relaxed);
    m_mergedRect = Rect::make(0, 0, 0, 0);

    for (int i = 0; i < GUI_MAX_DIRTY_RECTS; i++) {
        m_dirtyRects[i].clear();
    }

    m_stats.reset();
    m_initialized = true;

    GUI_LOG("DirtyRegionTracker initialized (%dx%d tiles, %d bytes)",
            DirtyConfig::GRID_WIDTH, DirtyConfig::GRID_HEIGHT, DirtyConfig::BITMAP_SIZE);

    return true;
}

void DirtyRegionTracker::shutdown() {
    m_initialized = false;
    markAllClean();
}

// ============================================================================
// Dirty Marking
// ============================================================================

void DirtyRegionTracker::markAllDirty() {
    if (!m_enabled) return;

    // Set all valid tile bits (GRID_TOTAL may not fill the last byte)
    memset(m_tileBitmap, 0xFF, sizeof(m_tileBitmap));
    constexpr uint8_t tailBits = DirtyConfig::GRID_TOTAL % 8;
    if constexpr (tailBits != 0) {
        m_tileBitmap[DirtyConfig::BITMAP_SIZE - 1] =
            static_cast<uint8_t>((1u << tailBits) - 1);
    }
    m_dirtyTileCount.store(DirtyConfig::GRID_TOTAL, std::memory_order_relaxed);

    // Set merged rect to full screen
    m_mergedRect = Rect::make(0, 0, Config::DISPLAY_WIDTH, Config::DISPLAY_HEIGHT);

    // Clear rect list and add full screen
    m_dirtyRectCount = 1;
    m_dirtyRects[0].set(m_mergedRect, 255);

    m_stats.markDirtyCount++;
}

void DirtyRegionTracker::markAllClean() {
    memset(m_tileBitmap, 0, sizeof(m_tileBitmap));
    m_dirtyTileCount.store(0, std::memory_order_relaxed);
    m_mergedRect = Rect::make(0, 0, 0, 0);
    m_dirtyRectCount = 0;

    for (int i = 0; i < GUI_MAX_DIRTY_RECTS; i++) {
        m_dirtyRects[i].clear();
    }
}

void DirtyRegionTracker::markDirty(const Rect& rect) {
    if (!m_enabled) return;
    if (rect.isEmpty()) return;

    // Clamp to screen bounds
    Rect clipped = rect.intersection(Rect::make(0, 0, Config::DISPLAY_WIDTH, Config::DISPLAY_HEIGHT));
    if (clipped.isEmpty()) return;

    m_stats.markDirtyCount++;

    // Mark tiles covered by rectangle
    markTilesInRect(clipped);

    // Update merged bounding box
    if (m_mergedRect.isEmpty()) {
        m_mergedRect = clipped;
    } else {
        // Union of rectangles
        int16_t x1 = std::min(m_mergedRect.x, clipped.x);
        int16_t y1 = std::min(m_mergedRect.y, clipped.y);
        int16_t x2 = std::max(m_mergedRect.right(), clipped.right());
        int16_t y2 = std::max(m_mergedRect.bottom(), clipped.bottom());
        m_mergedRect = Rect::make(x1, y1,
                                  static_cast<uint16_t>(x2 - x1),
                                  static_cast<uint16_t>(y2 - y1));
    }

    // Add to dirty rect list (or merge with existing)
    bool merged = false;

    // Try to merge with existing rect
    for (uint8_t i = 0; i < m_dirtyRectCount; i++) {
        if (m_dirtyRects[i].valid && m_dirtyRects[i].rect.intersects(clipped)) {
            // Merge by expanding existing rect
            Rect& existing = m_dirtyRects[i].rect;
            int16_t x1 = std::min(existing.x, clipped.x);
            int16_t y1 = std::min(existing.y, clipped.y);
            int16_t x2 = std::max(existing.right(), clipped.right());
            int16_t y2 = std::max(existing.bottom(), clipped.bottom());
            existing = Rect::make(x1, y1,
                                  static_cast<uint16_t>(x2 - x1),
                                  static_cast<uint16_t>(y2 - y1));
            merged = true;
            break;
        }
    }

    // If not merged, add as new rect
    if (!merged) {
        if (m_dirtyRectCount < GUI_MAX_DIRTY_RECTS) {
            m_dirtyRects[m_dirtyRectCount].set(clipped);
            m_dirtyRectCount++;
        } else {
            // List is full - merge all into first rect
            optimizeRects();
        }
    }
}

void DirtyRegionTracker::markDirty(int16_t x, int16_t y) {
    if (!m_enabled) return;
    if (x < 0 || x >= Config::DISPLAY_WIDTH || y < 0 || y >= Config::DISPLAY_HEIGHT) return;

    // Mark single tile
    uint8_t tileX = pixelToTileX(x);
    uint8_t tileY = pixelToTileY(y);

    if (!isTileDirty(tileX, tileY)) {
        setTile(tileX, tileY);
        m_dirtyTileCount.fetch_add(1, std::memory_order_relaxed);
    }

    // Update merged rect
    Rect pixelRect = Rect::make(x, y, 1, 1);
    if (m_mergedRect.isEmpty()) {
        m_mergedRect = pixelRect;
    } else {
        int16_t x1 = std::min(m_mergedRect.x, x);
        int16_t y1 = std::min(m_mergedRect.y, y);
        int16_t x2 = std::max(m_mergedRect.right(), static_cast<int16_t>(x + 1));
        int16_t y2 = std::max(m_mergedRect.bottom(), static_cast<int16_t>(y + 1));
        m_mergedRect = Rect::make(x1, y1,
                                  static_cast<uint16_t>(x2 - x1),
                                  static_cast<uint16_t>(y2 - y1));
    }

    m_stats.markDirtyCount++;
}

void DirtyRegionTracker::markDirty(int16_t x, int16_t y, uint16_t w, uint16_t h) {
    markDirty(Rect::make(x, y, w, h));
}

// ============================================================================
// Query
// ============================================================================

bool DirtyRegionTracker::isDirty() const {
    return m_dirtyTileCount.load(std::memory_order_relaxed) > 0;
}

bool DirtyRegionTracker::isDirty(const Rect& rect) const {
    if (!isDirty()) return false;

    // Check if rect intersects merged dirty region
    if (!m_mergedRect.intersects(rect)) return false;

    // Check individual tiles
    uint8_t startTileX = pixelToTileX(std::max<int16_t>(0, rect.x));
    uint8_t startTileY = pixelToTileY(std::max<int16_t>(0, rect.y));
    uint8_t endTileX = pixelToTileX(std::min<int16_t>(Config::DISPLAY_WIDTH - 1, rect.right() - 1));
    uint8_t endTileY = pixelToTileY(std::min<int16_t>(Config::DISPLAY_HEIGHT - 1, rect.bottom() - 1));

    for (uint8_t ty = startTileY; ty <= endTileY; ty++) {
        for (uint8_t tx = startTileX; tx <= endTileX; tx++) {
            if (isTileDirty(tx, ty)) {
                return true;
            }
        }
    }

    return false;
}

bool DirtyRegionTracker::isTileDirty(uint8_t tileX, uint8_t tileY) const {
    if (tileX >= DirtyConfig::GRID_WIDTH || tileY >= DirtyConfig::GRID_HEIGHT) {
        return false;
    }

    uint16_t index = tileIndex(tileX, tileY);
    uint8_t byteIdx, bitIdx;
    getBitPosition(index, byteIdx, bitIdx);

    return (m_tileBitmap[byteIdx] & (1 << bitIdx)) != 0;
}

uint16_t DirtyRegionTracker::dirtyTileCount() const {
    return m_dirtyTileCount.load(std::memory_order_relaxed);
}

uint8_t DirtyRegionTracker::dirtyPercentage() const {
    uint16_t count = dirtyTileCount();
    return static_cast<uint8_t>((count * 100) / DirtyConfig::GRID_TOTAL);
}

bool DirtyRegionTracker::shouldFullRefresh() const {
    return dirtyPercentage() >= m_fullRefreshThreshold;
}

// ============================================================================
// Region Access
// ============================================================================

Rect DirtyRegionTracker::getMergedDirtyRect() const {
    return m_mergedRect;
}

uint8_t DirtyRegionTracker::getDirtyRects(DirtyRect* rects, uint8_t maxRects) const {
    if (rects == nullptr || maxRects == 0) return 0;

    uint8_t count = 0;

    // If should do full refresh, return single full-screen rect
    if (shouldFullRefresh()) {
        rects[0].set(Rect::make(0, 0, Config::DISPLAY_WIDTH, Config::DISPLAY_HEIGHT), 255);
        return 1;
    }

    // Copy valid rects
    for (uint8_t i = 0; i < m_dirtyRectCount && count < maxRects; i++) {
        if (m_dirtyRects[i].valid) {
            rects[count] = m_dirtyRects[i];
            count++;
        }
    }

    // If no rects but tiles are dirty, return merged rect
    if (count == 0 && isDirty()) {
        rects[0].set(m_mergedRect);
        return 1;
    }

    return count;
}

Rect DirtyRegionTracker::getOptimalDirtyRect() const {
    if (!isDirty()) {
        return Rect::make(0, 0, 0, 0);
    }

    // If high dirty percentage, return full screen
    if (shouldFullRefresh()) {
        return Rect::make(0, 0, Config::DISPLAY_WIDTH, Config::DISPLAY_HEIGHT);
    }

    // Scan tiles to find tight bounding box
    int16_t minX = Config::DISPLAY_WIDTH;
    int16_t minY = Config::DISPLAY_HEIGHT;
    int16_t maxX = 0;
    int16_t maxY = 0;

    for (uint8_t ty = 0; ty < DirtyConfig::GRID_HEIGHT; ty++) {
        for (uint8_t tx = 0; tx < DirtyConfig::GRID_WIDTH; tx++) {
            if (isTileDirty(tx, ty)) {
                int16_t tilePixelX = tileToPixelX(tx);
                int16_t tilePixelY = tileToPixelY(ty);

                minX = std::min(minX, tilePixelX);
                minY = std::min(minY, tilePixelY);
                maxX = std::max(maxX, static_cast<int16_t>(tilePixelX + DirtyConfig::TILE_SIZE));
                maxY = std::max(maxY, static_cast<int16_t>(tilePixelY + DirtyConfig::TILE_SIZE));
            }
        }
    }

    // Clamp to screen bounds
    maxX = std::min(maxX, static_cast<int16_t>(Config::DISPLAY_WIDTH));
    maxY = std::min(maxY, static_cast<int16_t>(Config::DISPLAY_HEIGHT));

    if (maxX <= minX || maxY <= minY) {
        return Rect::make(0, 0, 0, 0);
    }

    return Rect::make(minX, minY,
                      static_cast<uint16_t>(maxX - minX),
                      static_cast<uint16_t>(maxY - minY));
}

// ============================================================================
// Configuration
// ============================================================================

void DirtyRegionTracker::setFullRefreshThreshold(uint8_t percent) {
    m_fullRefreshThreshold = std::min<uint8_t>(percent, 100);
}

// ============================================================================
// Internal Methods
// ============================================================================

void DirtyRegionTracker::setTile(uint8_t tileX, uint8_t tileY) {
    if (tileX >= DirtyConfig::GRID_WIDTH || tileY >= DirtyConfig::GRID_HEIGHT) return;

    uint16_t index = tileIndex(tileX, tileY);
    uint8_t byteIdx, bitIdx;
    getBitPosition(index, byteIdx, bitIdx);

    m_tileBitmap[byteIdx] |= (1 << bitIdx);
}

void DirtyRegionTracker::clearTile(uint8_t tileX, uint8_t tileY) {
    if (tileX >= DirtyConfig::GRID_WIDTH || tileY >= DirtyConfig::GRID_HEIGHT) return;

    uint16_t index = tileIndex(tileX, tileY);
    uint8_t byteIdx, bitIdx;
    getBitPosition(index, byteIdx, bitIdx);

    m_tileBitmap[byteIdx] &= ~(1 << bitIdx);
}

void DirtyRegionTracker::markTilesInRect(const Rect& rect) {
    uint8_t startTileX = pixelToTileX(rect.x);
    uint8_t startTileY = pixelToTileY(rect.y);
    uint8_t endTileX = pixelToTileX(rect.right() - 1);
    uint8_t endTileY = pixelToTileY(rect.bottom() - 1);

    // Clamp to grid bounds
    endTileX = std::min(endTileX, static_cast<uint8_t>(DirtyConfig::GRID_WIDTH - 1));
    endTileY = std::min(endTileY, static_cast<uint8_t>(DirtyConfig::GRID_HEIGHT - 1));

    uint16_t newDirtyCount = 0;

    for (uint8_t ty = startTileY; ty <= endTileY; ty++) {
        for (uint8_t tx = startTileX; tx <= endTileX; tx++) {
            if (!isTileDirty(tx, ty)) {
                setTile(tx, ty);
                newDirtyCount++;
            }
        }
    }

    if (newDirtyCount > 0) {
        m_dirtyTileCount.fetch_add(newDirtyCount, std::memory_order_relaxed);
    }
}

void DirtyRegionTracker::optimizeRects() {
    // Merge all rects into one (simplest optimization)
    if (m_dirtyRectCount <= 1) return;

    Rect merged = m_dirtyRects[0].rect;
    for (uint8_t i = 1; i < m_dirtyRectCount; i++) {
        if (m_dirtyRects[i].valid) {
            const Rect& r = m_dirtyRects[i].rect;
            int16_t x1 = std::min(merged.x, r.x);
            int16_t y1 = std::min(merged.y, r.y);
            int16_t x2 = std::max(merged.right(), r.right());
            int16_t y2 = std::max(merged.bottom(), r.bottom());
            merged = Rect::make(x1, y1,
                               static_cast<uint16_t>(x2 - x1),
                               static_cast<uint16_t>(y2 - y1));
        }
    }

    // Clear all and set single merged rect
    for (uint8_t i = 0; i < GUI_MAX_DIRTY_RECTS; i++) {
        m_dirtyRects[i].clear();
    }
    m_dirtyRects[0].set(merged);
    m_dirtyRectCount = 1;
}

} // namespace GUI

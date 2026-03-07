/*
 * GUI Render Queue - Lock-free command queue for async rendering
 *
 * This implements a single-producer, single-consumer (SPSC) lock-free ring buffer
 * for passing render commands from the main loop (Core 1) to the renderer task (Core 0).
 *
 * Design principles:
 * - Lock-free for minimal latency (no mutexes in hot path)
 * - Fixed-size commands for predictable memory layout
 * - Atomic head/tail indices for thread safety
 * - Semaphore signaling for efficient blocking on empty queue
 *
 * Memory layout:
 * - SRAM: Queue metadata + ring buffer (~8KB)
 * - Commands are copied, not referenced (safe for stack-allocated data)
 * - EXCEPTION: DrawBitmap and DrawJpeg store raw pointers to external data.
 *   The caller MUST keep the pointed-to buffer alive until the render queue
 *   has processed the command (e.g. use static/global buffers or call sync()).
 */

#ifndef GUI_RENDER_QUEUE_H
#define GUI_RENDER_QUEUE_H

#include "../gui_types.h"
#include "../gui_config.h"
#include <atomic>
#include <cstring>

// FreeRTOS includes
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace GUI {

// ============================================================================
// Render Operation Types
// ============================================================================

enum class RenderOpType : uint8_t {
    Nop = 0,            // No operation (placeholder)

    // Basic drawing primitives
    FillRect,           // Fill rectangle with solid color
    DrawRect,           // Draw rectangle outline
    DrawLine,           // Draw line
    DrawPixel,          // Draw single pixel

    // Extended drawing primitives (added for Legacy Bridge support)
    DrawCircle,         // Draw circle outline
    FillCircle,         // Fill circle with solid color
    DrawRoundRect,      // Draw rounded rectangle outline
    FillRoundRect,      // Fill rounded rectangle
    DrawTriangle,       // Draw triangle outline
    FillTriangle,       // Fill triangle

    // Text rendering
    DrawText,           // Draw text string
    DrawChar,           // Draw single character

    // Images
    DrawBitmap,         // Draw raw bitmap (RGB565)
    DrawJpeg,           // Draw JPEG from buffer

    // Clipping
    SetClip,            // Set clipping rectangle
    ClearClip,          // Remove clipping

    // Display control
    Clear,              // Clear entire screen
    FillScreen,         // Fill screen with color
    SetBrightness,      // Adjust backlight

    // Advanced operations
    Scroll,             // Scroll display content

    // Synchronization
    Sync,               // Wait for queue to drain
    EndFrame,           // Mark frame boundary

    // Count of operation types (for bounds checking)
    _Count
};

// ============================================================================
// Render Operation Flags
// ============================================================================

namespace RenderFlags {
    constexpr uint8_t None          = 0x00;
    constexpr uint8_t Urgent        = 0x01;  // Skip batching, render immediately
    constexpr uint8_t Opaque        = 0x02;  // No transparency (optimization hint)
    constexpr uint8_t NoAntiAlias   = 0x04;  // Disable anti-aliasing
    constexpr uint8_t TextWrap      = 0x08;  // Enable text wrapping
}

// ============================================================================
// Render Operation Data Structures (all POD)
// ============================================================================

// FillRect, DrawRect data (12 bytes)
struct RenderOpRect {
    Rect rect;          // 8 bytes
    Color color;        // 2 bytes
    uint8_t thickness;  // 1 byte
    uint8_t _pad;       // 1 byte
};

// DrawLine data (12 bytes)
struct RenderOpLine {
    Point p1;           // 4 bytes
    Point p2;           // 4 bytes
    Color color;        // 2 bytes
    uint8_t thickness;  // 1 byte
    uint8_t _pad;       // 1 byte
};

// DrawPixel data (8 bytes)
struct RenderOpPixel {
    Point pos;          // 4 bytes
    Color color;        // 2 bytes
    uint8_t _pad[2];    // 2 bytes padding
};

// DrawText data (28 bytes)
struct RenderOpText {
    Point pos;          // 4 bytes
    Color fg;           // 2 bytes
    Color bg;           // 2 bytes
    FontConfig font;    // 4 bytes
    uint8_t textLen;    // 1 byte
    uint8_t _pad[3];    // 3 bytes padding
    char text[12];      // 12 bytes (short text embedded)
};

// DrawChar data (16 bytes)
struct RenderOpChar {
    Point pos;          // 4 bytes
    Color fg;           // 2 bytes
    Color bg;           // 2 bytes
    FontConfig font;    // 4 bytes
    char ch;            // 1 byte
    uint8_t _pad[3];    // 3 bytes
};

// DrawBitmap data (16 bytes)
// WARNING: stores raw pointer — caller must keep data alive until processed!
struct RenderOpBitmap {
    Rect rect;              // 8 bytes
    const uint16_t* data;   // 4/8 bytes (pointer, NOT owned)
};

// DrawJpeg data (20 bytes)
// WARNING: stores raw pointer — caller must keep data alive until processed!
struct RenderOpJpeg {
    Point pos;              // 4 bytes
    const uint8_t* data;    // 4/8 bytes (pointer)
    uint32_t len;           // 4 bytes
    uint16_t maxWidth;      // 2 bytes
    uint16_t maxHeight;     // 2 bytes
};

// SetClip data (8 bytes)
struct RenderOpClip {
    Rect clipRect;          // 8 bytes
};

// Clear, FillScreen data (4 bytes)
struct RenderOpFill {
    Color color;            // 2 bytes
    uint8_t _pad[2];        // 2 bytes
};

// SetBrightness data (4 bytes)
struct RenderOpBrightness {
    uint8_t level;          // 1 byte
    uint8_t _pad[3];        // 3 bytes
};

// DrawCircle, FillCircle data (10 bytes)
struct RenderOpCircle {
    Point center;           // 4 bytes
    int16_t radius;         // 2 bytes
    Color color;            // 2 bytes
    uint8_t filled;         // 1 byte (0=outline, 1=filled)
    uint8_t _pad;           // 1 byte
};

// DrawRoundRect, FillRoundRect data (14 bytes)
struct RenderOpRoundRect {
    Rect rect;              // 8 bytes
    int16_t radius;         // 2 bytes
    Color color;            // 2 bytes
    uint8_t filled;         // 1 byte (0=outline, 1=filled)
    uint8_t _pad;           // 1 byte
};

// DrawTriangle, FillTriangle data (16 bytes)
struct RenderOpTriangle {
    Point p1;               // 4 bytes
    Point p2;               // 4 bytes
    Point p3;               // 4 bytes
    Color color;            // 2 bytes
    uint8_t filled;         // 1 byte (0=outline, 1=filled)
    uint8_t _pad;           // 1 byte
};

// Scroll data (4 bytes)
struct RenderOpScroll {
    int16_t dx;             // 2 bytes
    int16_t dy;             // 2 bytes
};

// ============================================================================
// Render Operation Structure
// ============================================================================

struct RenderOp {
    RenderOpType type;          // 1 byte
    RenderPriority priority;    // 1 byte
    DisplayTarget target;       // 1 byte
    uint8_t flags;              // 1 byte

    // Explicit default constructor needed because union contains
    // members with non-trivial constructors (Rect has a constructor)
    RenderOp() { memset(this, 0, sizeof(*this)); }

    // Data union (28 bytes to make total 32 bytes)
    union {
        RenderOpRect rect;
        RenderOpLine line;
        RenderOpPixel pixel;
        RenderOpText text;
        RenderOpChar chr;
        RenderOpBitmap bitmap;
        RenderOpJpeg jpeg;
        RenderOpClip clip;
        RenderOpFill fill;
        RenderOpBrightness brightness;
        RenderOpCircle circle;
        RenderOpRoundRect roundRect;
        RenderOpTriangle triangle;
        RenderOpScroll scroll;
        uint8_t raw[28];        // For memset
    } data;
};

// Verify structure size
// On ESP32 (32-bit): 4 + 28 = 32 bytes
// On desktop (64-bit): May be larger due to pointer alignment
#if defined(ESP32) || defined(ESP_PLATFORM)
static_assert(sizeof(RenderOp) == 32, "RenderOp must be 32 bytes on ESP32");
#else
static_assert(sizeof(RenderOp) <= 48, "RenderOp size check for desktop build");
#endif

// ============================================================================
// Lock-Free Ring Buffer Queue
// ============================================================================

class RenderQueue {
public:
    // Singleton access (initialized lazily)
    static RenderQueue& instance();

    // Initialize the queue (call once at startup)
    bool init();

    // Shutdown and cleanup
    void shutdown();

    // Check if initialized
    bool isInitialized() const { return m_initialized; }

    // ========================================================================
    // Producer Interface (Main loop / Core 1)
    // ========================================================================

    // Push a command to the queue
    // Returns true if successful, false if queue is full
    // This is lock-free and wait-free
    bool push(const RenderOp& op);

    // Push multiple commands atomically (best-effort)
    // Returns number of commands pushed
    size_t pushBatch(const RenderOp* ops, size_t count);

    // ========================================================================
    // Consumer Interface (Renderer task / Core 0)
    // ========================================================================

    // Pop a command from the queue
    // Blocks up to timeoutMs if queue is empty
    // Returns true if a command was retrieved
    bool pop(RenderOp& op, uint32_t timeoutMs = portMAX_DELAY);

    // Pop without blocking (returns false if empty)
    bool tryPop(RenderOp& op);

    // Peek at the next command without removing it
    bool peek(RenderOp& op) const;

    // ========================================================================
    // Queue Status
    // ========================================================================

    // Number of commands waiting to be processed
    size_t pending() const;

    // Check if queue is full
    bool isFull() const;

    // Check if queue is empty
    bool isEmpty() const;

    // Get queue capacity
    size_t capacity() const { return Config::QUEUE_SIZE; }

    // ========================================================================
    // Synchronization
    // ========================================================================

    // Clear all pending commands
    void clear();

    // Wait until all commands are processed
    void sync(uint32_t timeoutMs = 1000);

    // Signal that a command was processed (called by renderer)
    void signalProcessed();

    // ========================================================================
    // Statistics
    // ========================================================================

    // Get overflow count (pushes that failed due to full queue)
    uint32_t getOverflowCount() const { return m_overflowCount; }

    // Reset overflow counter
    void resetOverflowCount() { m_overflowCount = 0; }

    // High water mark (max pending commands seen)
    size_t getHighWaterMark() const { return m_highWaterMark; }

private:
    RenderQueue();
    ~RenderQueue();

    // Prevent copying
    RenderQueue(const RenderQueue&) = delete;
    RenderQueue& operator=(const RenderQueue&) = delete;

    // Ring buffer storage
    RenderOp m_buffer[Config::QUEUE_SIZE];

    // Atomic indices for lock-free operation
    std::atomic<size_t> m_head{0};
    std::atomic<size_t> m_tail{0};

    // Semaphore for blocking consumer when queue is empty
    SemaphoreHandle_t m_dataAvailable;

    // Semaphore for sync() operation
    SemaphoreHandle_t m_syncComplete;
    std::atomic<bool> m_syncPending{false};

    // Statistics
    std::atomic<uint32_t> m_overflowCount{0};
    std::atomic<size_t> m_highWaterMark{0};

    // Initialization flag
    bool m_initialized;
};

// ============================================================================
// Helper Functions for Creating Render Operations
// ============================================================================

namespace RenderOps {

    // Create FillRect operation
    inline RenderOp fillRect(int16_t x, int16_t y, uint16_t w, uint16_t h, Color color,
                             RenderPriority priority = RenderPriority::Normal) {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::FillRect;
        op.priority = priority;
        op.target = DisplayTarget::Internal;
        op.data.rect.rect = Rect::make(x, y, w, h);
        op.data.rect.color = color;
        return op;
    }

    // Create DrawRect operation
    inline RenderOp drawRect(int16_t x, int16_t y, uint16_t w, uint16_t h, Color color,
                             uint8_t thickness = 1,
                             RenderPriority priority = RenderPriority::Normal) {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::DrawRect;
        op.priority = priority;
        op.target = DisplayTarget::Internal;
        op.data.rect.rect = Rect::make(x, y, w, h);
        op.data.rect.color = color;
        op.data.rect.thickness = thickness;
        return op;
    }

    // Create DrawLine operation
    inline RenderOp drawLine(int16_t x1, int16_t y1, int16_t x2, int16_t y2, Color color,
                             uint8_t thickness = 1,
                             RenderPriority priority = RenderPriority::Normal) {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::DrawLine;
        op.priority = priority;
        op.target = DisplayTarget::Internal;
        op.data.line.p1 = Point::make(x1, y1);
        op.data.line.p2 = Point::make(x2, y2);
        op.data.line.color = color;
        op.data.line.thickness = thickness;
        return op;
    }

    // Create DrawPixel operation
    inline RenderOp drawPixel(int16_t x, int16_t y, Color color,
                              RenderPriority priority = RenderPriority::Normal) {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::DrawPixel;
        op.priority = priority;
        op.target = DisplayTarget::Internal;
        op.data.pixel.pos = Point::make(x, y);
        op.data.pixel.color = color;
        return op;
    }

    // Create DrawText operation (short text, <= 12 chars)
    inline RenderOp drawText(int16_t x, int16_t y, const char* text,
                             Color fg, Color bg = Colors::Black,
                             RenderPriority priority = RenderPriority::Normal) {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::DrawText;
        op.priority = priority;
        op.target = DisplayTarget::Internal;
        op.data.text.pos = Point::make(x, y);
        op.data.text.fg = fg;
        op.data.text.bg = bg;
        op.data.text.font = FontConfig::make();

        size_t len = strlen(text);
        // Reserve last byte for null-terminator: max 11 visible chars
        uint8_t copyLen = static_cast<uint8_t>(len > 11 ? 11 : len);
        op.data.text.textLen = copyLen;
        memcpy(op.data.text.text, text, copyLen);
        op.data.text.text[copyLen] = '\0';
        return op;
    }

    // Create Clear operation
    inline RenderOp clear(Color color = Colors::Black,
                          RenderPriority priority = RenderPriority::Normal) {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::Clear;
        op.priority = priority;
        op.target = DisplayTarget::Internal;
        op.data.fill.color = color;
        return op;
    }

    // Create FillScreen operation
    inline RenderOp fillScreen(Color color,
                               RenderPriority priority = RenderPriority::Normal) {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::FillScreen;
        op.priority = priority;
        op.target = DisplayTarget::Internal;
        op.data.fill.color = color;
        return op;
    }

    // Create SetClip operation
    inline RenderOp setClip(int16_t x, int16_t y, uint16_t w, uint16_t h) {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::SetClip;
        op.target = DisplayTarget::Internal;
        op.data.clip.clipRect = Rect::make(x, y, w, h);
        return op;
    }

    // Create ClearClip operation
    inline RenderOp clearClip() {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::ClearClip;
        op.target = DisplayTarget::Internal;
        return op;
    }

    // Create SetBrightness operation
    inline RenderOp setBrightness(uint8_t level) {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::SetBrightness;
        op.target = DisplayTarget::Internal;
        op.data.brightness.level = level;
        return op;
    }

    // Create Sync operation
    inline RenderOp sync() {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::Sync;
        op.priority = RenderPriority::Urgent;
        op.target = DisplayTarget::Internal;
        return op;
    }

    // Create EndFrame operation
    inline RenderOp endFrame() {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::EndFrame;
        op.target = DisplayTarget::Internal;
        return op;
    }

    // ========================================================================
    // Extended Drawing Primitives (for Legacy Bridge support)
    // ========================================================================

    // Create DrawCircle operation
    inline RenderOp drawCircle(int16_t x, int16_t y, int16_t r, Color color,
                               RenderPriority priority = RenderPriority::Normal) {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::DrawCircle;
        op.priority = priority;
        op.target = DisplayTarget::Internal;
        op.data.circle.center = Point::make(x, y);
        op.data.circle.radius = r;
        op.data.circle.color = color;
        op.data.circle.filled = 0;
        return op;
    }

    // Create FillCircle operation
    inline RenderOp fillCircle(int16_t x, int16_t y, int16_t r, Color color,
                               RenderPriority priority = RenderPriority::Normal) {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::FillCircle;
        op.priority = priority;
        op.target = DisplayTarget::Internal;
        op.data.circle.center = Point::make(x, y);
        op.data.circle.radius = r;
        op.data.circle.color = color;
        op.data.circle.filled = 1;
        return op;
    }

    // Create DrawRoundRect operation
    inline RenderOp drawRoundRect(int16_t x, int16_t y, uint16_t w, uint16_t h,
                                  int16_t r, Color color,
                                  RenderPriority priority = RenderPriority::Normal) {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::DrawRoundRect;
        op.priority = priority;
        op.target = DisplayTarget::Internal;
        op.data.roundRect.rect = Rect::make(x, y, w, h);
        op.data.roundRect.radius = r;
        op.data.roundRect.color = color;
        op.data.roundRect.filled = 0;
        return op;
    }

    // Create FillRoundRect operation
    inline RenderOp fillRoundRect(int16_t x, int16_t y, uint16_t w, uint16_t h,
                                  int16_t r, Color color,
                                  RenderPriority priority = RenderPriority::Normal) {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::FillRoundRect;
        op.priority = priority;
        op.target = DisplayTarget::Internal;
        op.data.roundRect.rect = Rect::make(x, y, w, h);
        op.data.roundRect.radius = r;
        op.data.roundRect.color = color;
        op.data.roundRect.filled = 1;
        return op;
    }

    // Create DrawTriangle operation
    inline RenderOp drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                 int16_t x2, int16_t y2, Color color,
                                 RenderPriority priority = RenderPriority::Normal) {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::DrawTriangle;
        op.priority = priority;
        op.target = DisplayTarget::Internal;
        op.data.triangle.p1 = Point::make(x0, y0);
        op.data.triangle.p2 = Point::make(x1, y1);
        op.data.triangle.p3 = Point::make(x2, y2);
        op.data.triangle.color = color;
        op.data.triangle.filled = 0;
        return op;
    }

    // Create FillTriangle operation
    inline RenderOp fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                 int16_t x2, int16_t y2, Color color,
                                 RenderPriority priority = RenderPriority::Normal) {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::FillTriangle;
        op.priority = priority;
        op.target = DisplayTarget::Internal;
        op.data.triangle.p1 = Point::make(x0, y0);
        op.data.triangle.p2 = Point::make(x1, y1);
        op.data.triangle.p3 = Point::make(x2, y2);
        op.data.triangle.color = color;
        op.data.triangle.filled = 1;
        return op;
    }

    // Create Scroll operation
    inline RenderOp scroll(int16_t dx, int16_t dy,
                           RenderPriority priority = RenderPriority::Normal) {
        RenderOp op;
        memset(&op, 0, sizeof(op));
        op.type = RenderOpType::Scroll;
        op.priority = priority;
        op.target = DisplayTarget::Internal;
        op.data.scroll.dx = dx;
        op.data.scroll.dy = dy;
        return op;
    }

} // namespace RenderOps

} // namespace GUI

#endif // GUI_RENDER_QUEUE_H

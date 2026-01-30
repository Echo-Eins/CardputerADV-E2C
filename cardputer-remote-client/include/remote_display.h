/**
 * @file remote_display.h
 * @brief Display module with DMA and hardware JPEG decoder
 *
 * Features:
 * - Hardware JPEG decoder (ESP32-S3 specific)
 * - DMA-based LCD transfer for zero-copy rendering
 * - Double buffering for smooth updates
 * - Fallback to software JPEG decoder
 */

#ifndef REMOTE_DISPLAY_H
#define REMOTE_DISPLAY_H

#include "remote_config.h"
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Display Configuration for M5Stack Cardputer
// =============================================================================

// ST7789V2 LCD on Cardputer
#define LCD_HOST        SPI2_HOST
#define LCD_PIXEL_CLK   40000000    // 40 MHz SPI clock
#define LCD_CMD_BITS    8
#define LCD_PARAM_BITS  8

// Cardputer LCD pins
#define LCD_PIN_SCLK    36
#define LCD_PIN_MOSI    35
#define LCD_PIN_DC      34
#define LCD_PIN_CS      37
#define LCD_PIN_RST     33
#define LCD_PIN_BL      38

// DMA configuration
#define LCD_DMA_CHANNEL 1
#define LCD_DMA_MAX_TRANSFER_SIZE (DISPLAY_WIDTH * 16 * 2)  // 16 lines per DMA transfer

// =============================================================================
// Display Context
// =============================================================================

typedef struct {
    // LCD panel handle
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;

    // Double buffer for DMA
    uint16_t *frame_buffer[2];
    uint8_t current_buffer;
    bool buffer_busy[2];

    // JPEG decoder state
    void *jpeg_decoder;
    bool hw_jpeg_available;

    // Statistics
    uint32_t frames_displayed;
    uint32_t jpeg_decode_errors;
    uint32_t last_decode_time_us;
    uint32_t last_transfer_time_us;

    // Brightness
    uint8_t brightness;
    bool backlight_on;
} display_context_t;

// =============================================================================
// Initialization
// =============================================================================

/**
 * @brief Initialize display with DMA support
 * @param ctx Display context
 * @return REMOTE_OK on success
 */
remote_error_t display_init(display_context_t *ctx);

/**
 * @brief Deinitialize display
 */
void display_deinit(display_context_t *ctx);

// =============================================================================
// Frame Rendering
// =============================================================================

/**
 * @brief Decode JPEG and display on screen
 * @param ctx Display context
 * @param jpeg_data JPEG compressed data
 * @param jpeg_len JPEG data length
 * @return REMOTE_OK on success
 *
 * Uses hardware JPEG decoder if available, falls back to software.
 * Uses DMA for LCD transfer.
 */
remote_error_t display_render_jpeg(display_context_t *ctx,
                                   const uint8_t *jpeg_data,
                                   size_t jpeg_len);

/**
 * @brief Render raw RGB565 frame (for testing)
 * @param ctx Display context
 * @param rgb565_data RGB565 pixel data (DISPLAY_WIDTH * DISPLAY_HEIGHT * 2 bytes)
 * @return REMOTE_OK on success
 */
remote_error_t display_render_rgb565(display_context_t *ctx,
                                     const uint16_t *rgb565_data);

/**
 * @brief Wait for current DMA transfer to complete
 * @param ctx Display context
 * @param timeout_ms Timeout in milliseconds
 * @return REMOTE_OK if completed, REMOTE_ERR_TIMEOUT if timed out
 */
remote_error_t display_wait_transfer(display_context_t *ctx, uint32_t timeout_ms);

// =============================================================================
// Display Control
// =============================================================================

/**
 * @brief Set backlight brightness
 * @param ctx Display context
 * @param brightness 0-255
 */
void display_set_brightness(display_context_t *ctx, uint8_t brightness);

/**
 * @brief Turn backlight on/off
 * @param ctx Display context
 * @param on true to turn on
 */
void display_set_backlight(display_context_t *ctx, bool on);

/**
 * @brief Enter light sleep mode (display off, minimal power)
 */
void display_sleep(display_context_t *ctx);

/**
 * @brief Wake from light sleep
 */
void display_wake(display_context_t *ctx);

/**
 * @brief Clear screen to black
 */
void display_clear(display_context_t *ctx);

/**
 * @brief Draw status text (for connection status, errors, etc.)
 * @param ctx Display context
 * @param text Text to display
 * @param line Line number (0-based)
 */
void display_draw_status(display_context_t *ctx, const char *text, int line);

// =============================================================================
// Statistics
// =============================================================================

/**
 * @brief Get display statistics
 */
typedef struct {
    uint32_t frames_displayed;
    uint32_t jpeg_errors;
    uint32_t avg_decode_time_us;
    uint32_t avg_transfer_time_us;
    bool hw_jpeg_enabled;
} display_stats_t;

void display_get_stats(const display_context_t *ctx, display_stats_t *stats);

// =============================================================================
// Hardware JPEG Decoder (ESP32-S3 specific)
// =============================================================================

#if CONFIG_IDF_TARGET_ESP32S3

/**
 * @brief Check if hardware JPEG decoder is available
 */
bool display_hw_jpeg_available(void);

/**
 * @brief Decode JPEG using hardware decoder
 * @param jpeg_data Input JPEG data
 * @param jpeg_len JPEG data length
 * @param rgb565_out Output RGB565 buffer (must be DISPLAY_BUFFER_SIZE)
 * @param out_width Output image width
 * @param out_height Output image height
 * @return REMOTE_OK on success
 */
remote_error_t display_hw_jpeg_decode(const uint8_t *jpeg_data,
                                      size_t jpeg_len,
                                      uint16_t *rgb565_out,
                                      uint16_t *out_width,
                                      uint16_t *out_height);

#endif // CONFIG_IDF_TARGET_ESP32S3

#ifdef __cplusplus
}
#endif

#endif // REMOTE_DISPLAY_H

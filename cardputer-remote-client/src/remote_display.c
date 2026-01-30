/**
 * @file remote_display.c
 * @brief Display module implementation with DMA and hardware JPEG
 */

#include "remote_display.h"
#include <string.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Software JPEG decoder (fallback)
#include <tjpgd.h>

// Hardware JPEG decoder (ESP32-S3 only)
#if CONFIG_IDF_TARGET_ESP32S3
#include <esp_jpeg_dec.h>
#endif

static const char *TAG = "remote_display";

// =============================================================================
// DMA Transfer Completion Callback
// =============================================================================

static SemaphoreHandle_t dma_sem = NULL;

static bool IRAM_ATTR lcd_trans_done_cb(esp_lcd_panel_io_handle_t io,
                                        esp_lcd_panel_io_event_data_t *data,
                                        void *user_ctx) {
    display_context_t *ctx = (display_context_t *)user_ctx;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Mark buffer as not busy
    ctx->buffer_busy[ctx->current_buffer] = false;

    // Signal completion
    if (dma_sem) {
        xSemaphoreGiveFromISR(dma_sem, &xHigherPriorityTaskWoken);
    }

    return xHigherPriorityTaskWoken == pdTRUE;
}

// =============================================================================
// Software JPEG Decoder (TJPGD)
// =============================================================================

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    uint16_t *output;
    uint16_t out_width;
    uint16_t out_height;
} tjpgd_io_t;

static size_t tjpgd_input_func(JDEC *jd, uint8_t *buff, size_t ndata) {
    tjpgd_io_t *io = (tjpgd_io_t *)jd->device;

    size_t remaining = io->len - io->pos;
    size_t to_read = ndata < remaining ? ndata : remaining;

    if (buff) {
        memcpy(buff, io->data + io->pos, to_read);
    }
    io->pos += to_read;

    return to_read;
}

static int tjpgd_output_func(JDEC *jd, void *bitmap, JRECT *rect) {
    tjpgd_io_t *io = (tjpgd_io_t *)jd->device;
    uint16_t *rgb565 = (uint16_t *)bitmap;

    // Copy decoded block to output buffer
    for (int y = rect->top; y <= rect->bottom && y < io->out_height; y++) {
        for (int x = rect->left; x <= rect->right && x < io->out_width; x++) {
            int src_idx = (y - rect->top) * (rect->right - rect->left + 1) + (x - rect->left);
            int dst_idx = y * io->out_width + x;
            io->output[dst_idx] = rgb565[src_idx];
        }
    }

    return 1;  // Continue
}

static remote_error_t sw_jpeg_decode(const uint8_t *jpeg_data, size_t jpeg_len,
                                      uint16_t *rgb565_out,
                                      uint16_t *out_width, uint16_t *out_height) {
    JDEC jd;
    tjpgd_io_t io = {
        .data = jpeg_data,
        .len = jpeg_len,
        .pos = 0,
        .output = rgb565_out,
        .out_width = DISPLAY_WIDTH,
        .out_height = DISPLAY_HEIGHT,
    };

    // Work buffer for TJPGD (4KB minimum)
    void *work = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL);
    if (!work) {
        ESP_LOGE(TAG, "Failed to allocate TJPGD work buffer");
        return REMOTE_ERR_NO_MEMORY;
    }

    // Prepare decoder
    JRESULT res = jd_prepare(&jd, tjpgd_input_func, work, 4096, &io);
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "TJPGD prepare failed: %d", res);
        free(work);
        return REMOTE_ERR_JPEG;
    }

    *out_width = jd.width;
    *out_height = jd.height;

    // Decompress
    res = jd_decomp(&jd, tjpgd_output_func, 0);
    free(work);

    if (res != JDR_OK) {
        ESP_LOGE(TAG, "TJPGD decompress failed: %d", res);
        return REMOTE_ERR_JPEG;
    }

    return REMOTE_OK;
}

// =============================================================================
// Hardware JPEG Decoder (ESP32-S3)
// =============================================================================

#if CONFIG_IDF_TARGET_ESP32S3

static jpeg_dec_handle_t hw_jpeg_handle = NULL;
static bool hw_jpeg_initialized = false;

bool display_hw_jpeg_available(void) {
    return hw_jpeg_initialized;
}

static remote_error_t hw_jpeg_init(void) {
    if (hw_jpeg_initialized) {
        return REMOTE_OK;
    }

    jpeg_dec_config_t config = {
        .output_format = JPEG_DEC_RGB565,
    };

    esp_err_t err = jpeg_dec_open(&config, &hw_jpeg_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Hardware JPEG decoder not available: %s", esp_err_to_name(err));
        return REMOTE_ERR_JPEG;
    }

    hw_jpeg_initialized = true;
    ESP_LOGI(TAG, "Hardware JPEG decoder initialized");
    return REMOTE_OK;
}

remote_error_t display_hw_jpeg_decode(const uint8_t *jpeg_data,
                                      size_t jpeg_len,
                                      uint16_t *rgb565_out,
                                      uint16_t *out_width,
                                      uint16_t *out_height) {
    if (!hw_jpeg_initialized) {
        return REMOTE_ERR_JPEG;
    }

    jpeg_dec_io_t io = {
        .inbuf = (uint8_t *)jpeg_data,
        .inbuf_len = jpeg_len,
        .outbuf = (uint8_t *)rgb565_out,
        .outbuf_size = DISPLAY_BUFFER_SIZE,
    };

    jpeg_dec_header_info_t header;
    esp_err_t err = jpeg_dec_parse_header(hw_jpeg_handle, &io, &header);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HW JPEG header parse failed: %s", esp_err_to_name(err));
        return REMOTE_ERR_JPEG;
    }

    *out_width = header.width;
    *out_height = header.height;

    err = jpeg_dec_process(hw_jpeg_handle, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HW JPEG decode failed: %s", esp_err_to_name(err));
        return REMOTE_ERR_JPEG;
    }

    return REMOTE_OK;
}

#else

bool display_hw_jpeg_available(void) {
    return false;
}

#endif // CONFIG_IDF_TARGET_ESP32S3

// =============================================================================
// Display Initialization
// =============================================================================

remote_error_t display_init(display_context_t *ctx) {
    esp_err_t err;

    memset(ctx, 0, sizeof(*ctx));

    // Create DMA semaphore
    dma_sem = xSemaphoreCreateBinary();
    if (!dma_sem) {
        ESP_LOGE(TAG, "Failed to create DMA semaphore");
        return REMOTE_ERR_NO_MEMORY;
    }
    xSemaphoreGive(dma_sem);  // Initially available

    // Allocate DMA-capable frame buffers
    for (int i = 0; i < 2; i++) {
        ctx->frame_buffer[i] = heap_caps_malloc(DISPLAY_BUFFER_SIZE,
                                                 MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!ctx->frame_buffer[i]) {
            ESP_LOGE(TAG, "Failed to allocate frame buffer %d", i);
            display_deinit(ctx);
            return REMOTE_ERR_NO_MEMORY;
        }
        memset(ctx->frame_buffer[i], 0, DISPLAY_BUFFER_SIZE);
    }

    // Configure backlight PWM
    ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&bl_timer);

    ledc_channel_config_t bl_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .gpio_num = LCD_PIN_BL,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&bl_channel);

    // Configure SPI bus
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_DMA_MAX_TRANSFER_SIZE,
    };

    err = spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        display_deinit(ctx);
        return REMOTE_ERR_DISPLAY;
    }

    // Configure LCD panel IO
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLK,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = lcd_trans_done_cb,
        .user_ctx = ctx,
    };

    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &ctx->io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD panel IO init failed: %s", esp_err_to_name(err));
        display_deinit(ctx);
        return REMOTE_ERR_DISPLAY;
    }

    // Configure LCD panel (ST7789)
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };

    err = esp_lcd_new_panel_st7789(ctx->io, &panel_cfg, &ctx->panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD panel init failed: %s", esp_err_to_name(err));
        display_deinit(ctx);
        return REMOTE_ERR_DISPLAY;
    }

    // Initialize panel
    esp_lcd_panel_reset(ctx->panel);
    esp_lcd_panel_init(ctx->panel);
    esp_lcd_panel_invert_color(ctx->panel, true);  // ST7789 needs inversion
    esp_lcd_panel_swap_xy(ctx->panel, true);       // Landscape mode
    esp_lcd_panel_mirror(ctx->panel, false, true);
    esp_lcd_panel_disp_on_off(ctx->panel, true);

    // Try to initialize hardware JPEG decoder
#if CONFIG_IDF_TARGET_ESP32S3
    ctx->hw_jpeg_available = (hw_jpeg_init() == REMOTE_OK);
#else
    ctx->hw_jpeg_available = false;
#endif

    // Turn on backlight
    ctx->brightness = 128;
    display_set_brightness(ctx, ctx->brightness);
    ctx->backlight_on = true;

    ESP_LOGI(TAG, "Display initialized (HW JPEG: %s)",
             ctx->hw_jpeg_available ? "yes" : "no");
    return REMOTE_OK;
}

void display_deinit(display_context_t *ctx) {
    if (ctx->panel) {
        esp_lcd_panel_disp_on_off(ctx->panel, false);
        esp_lcd_panel_del(ctx->panel);
    }

    if (ctx->io) {
        esp_lcd_panel_io_del(ctx->io);
    }

    for (int i = 0; i < 2; i++) {
        if (ctx->frame_buffer[i]) {
            heap_caps_free(ctx->frame_buffer[i]);
        }
    }

    if (dma_sem) {
        vSemaphoreDelete(dma_sem);
        dma_sem = NULL;
    }

    spi_bus_free(LCD_HOST);

#if CONFIG_IDF_TARGET_ESP32S3
    if (hw_jpeg_initialized && hw_jpeg_handle) {
        jpeg_dec_close(hw_jpeg_handle);
        hw_jpeg_initialized = false;
    }
#endif

    memset(ctx, 0, sizeof(*ctx));
}

// =============================================================================
// Frame Rendering
// =============================================================================

remote_error_t display_render_jpeg(display_context_t *ctx,
                                   const uint8_t *jpeg_data,
                                   size_t jpeg_len) {
    uint64_t decode_start = esp_timer_get_time();
    uint16_t width, height;
    remote_error_t err;

    // Wait for previous transfer to complete
    err = display_wait_transfer(ctx, 100);
    if (err != REMOTE_OK) {
        ESP_LOGW(TAG, "Previous transfer still in progress");
    }

    // Select next buffer
    uint8_t buf_idx = (ctx->current_buffer + 1) % 2;
    uint16_t *target_buffer = ctx->frame_buffer[buf_idx];

    // Decode JPEG
#if CONFIG_IDF_TARGET_ESP32S3
    if (ctx->hw_jpeg_available) {
        err = display_hw_jpeg_decode(jpeg_data, jpeg_len, target_buffer,
                                     &width, &height);
        if (err != REMOTE_OK) {
            // Fall back to software decoder
            ESP_LOGW(TAG, "HW JPEG failed, using software decoder");
            err = sw_jpeg_decode(jpeg_data, jpeg_len, target_buffer,
                                 &width, &height);
        }
    } else {
        err = sw_jpeg_decode(jpeg_data, jpeg_len, target_buffer,
                             &width, &height);
    }
#else
    err = sw_jpeg_decode(jpeg_data, jpeg_len, target_buffer, &width, &height);
#endif

    if (err != REMOTE_OK) {
        ctx->jpeg_decode_errors++;
        ESP_LOGE(TAG, "JPEG decode failed");
        return err;
    }

    ctx->last_decode_time_us = (uint32_t)(esp_timer_get_time() - decode_start);

    // Start DMA transfer
    uint64_t transfer_start = esp_timer_get_time();

    ctx->buffer_busy[buf_idx] = true;
    ctx->current_buffer = buf_idx;

    esp_err_t esp_err = esp_lcd_panel_draw_bitmap(ctx->panel,
                                                   0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                                   target_buffer);
    if (esp_err != ESP_OK) {
        ctx->buffer_busy[buf_idx] = false;
        ESP_LOGE(TAG, "LCD draw failed: %s", esp_err_to_name(esp_err));
        return REMOTE_ERR_DISPLAY;
    }

    ctx->last_transfer_time_us = (uint32_t)(esp_timer_get_time() - transfer_start);
    ctx->frames_displayed++;

    return REMOTE_OK;
}

remote_error_t display_render_rgb565(display_context_t *ctx,
                                     const uint16_t *rgb565_data) {
    remote_error_t err = display_wait_transfer(ctx, 100);
    if (err != REMOTE_OK) {
        ESP_LOGW(TAG, "Previous transfer still in progress");
    }

    uint8_t buf_idx = (ctx->current_buffer + 1) % 2;
    memcpy(ctx->frame_buffer[buf_idx], rgb565_data, DISPLAY_BUFFER_SIZE);

    ctx->buffer_busy[buf_idx] = true;
    ctx->current_buffer = buf_idx;

    esp_err_t esp_err = esp_lcd_panel_draw_bitmap(ctx->panel,
                                                   0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                                   ctx->frame_buffer[buf_idx]);
    if (esp_err != ESP_OK) {
        ctx->buffer_busy[buf_idx] = false;
        return REMOTE_ERR_DISPLAY;
    }

    ctx->frames_displayed++;
    return REMOTE_OK;
}

remote_error_t display_wait_transfer(display_context_t *ctx, uint32_t timeout_ms) {
    (void)ctx;

    if (xSemaphoreTake(dma_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        xSemaphoreGive(dma_sem);
        return REMOTE_OK;
    }

    return REMOTE_ERR_TIMEOUT;
}

// =============================================================================
// Display Control
// =============================================================================

void display_set_brightness(display_context_t *ctx, uint8_t brightness) {
    ctx->brightness = brightness;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, brightness);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void display_set_backlight(display_context_t *ctx, bool on) {
    ctx->backlight_on = on;
    display_set_brightness(ctx, on ? ctx->brightness : 0);
}

void display_sleep(display_context_t *ctx) {
    display_set_backlight(ctx, false);
    esp_lcd_panel_disp_on_off(ctx->panel, false);
}

void display_wake(display_context_t *ctx) {
    esp_lcd_panel_disp_on_off(ctx->panel, true);
    display_set_backlight(ctx, true);
}

void display_clear(display_context_t *ctx) {
    memset(ctx->frame_buffer[0], 0, DISPLAY_BUFFER_SIZE);
    display_render_rgb565(ctx, ctx->frame_buffer[0]);
}

void display_draw_status(display_context_t *ctx, const char *text, int line) {
    // Simple status rendering - just stores text for now
    // In a full implementation, this would render text to the frame buffer
    ESP_LOGI(TAG, "Status [%d]: %s", line, text);
}

// =============================================================================
// Statistics
// =============================================================================

void display_get_stats(const display_context_t *ctx, display_stats_t *stats) {
    stats->frames_displayed = ctx->frames_displayed;
    stats->jpeg_errors = ctx->jpeg_decode_errors;
    stats->avg_decode_time_us = ctx->last_decode_time_us;
    stats->avg_transfer_time_us = ctx->last_transfer_time_us;
    stats->hw_jpeg_enabled = ctx->hw_jpeg_available;
}

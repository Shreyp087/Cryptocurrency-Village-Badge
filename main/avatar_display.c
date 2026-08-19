#include "avatar_display.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/*
 * CryptoHack / HiP Badge REL_0.8.12 SE display wiring. The production
 * display flex does not behave like the published KiCad net names imply.
 * The factory firmware recovered from this badge configures GPIO1 as LCD
 * chip select and does not give the ST7789 driver a reset GPIO. GPIO4 is
 * pulsed manually once during startup, then left high; this matches the
 * production panel while keeping the shared SPI EEPROM deselected.
 *
 *   GPIO0  TFT D/C
 *   GPIO1  TFT chip select
 *   GPIO4  TFT reset / shared EEPROM chip select
 *   GPIO6  SPI clock
 *   GPIO7  SPI MOSI
 *
 * The panel fitted to this badge is an ST7789-class 240x320 controller
 * mounted in landscape orientation. The factory-proven 20 MHz clock keeps
 * button-driven full-screen changes responsive; the earlier false-color
 * bands were caused by RGB565 byte order, not signal integrity.
 */
#define LCD_PIN_DC      GPIO_NUM_0
#define LCD_PIN_CS      GPIO_NUM_1
#define LCD_PIN_RESET   GPIO_NUM_4
#define LCD_PIN_SCK     GPIO_NUM_6
#define LCD_PIN_MOSI    GPIO_NUM_7
#define LCD_SPI_HOST    SPI2_HOST
#define LCD_CLOCK_HZ    (20 * 1000 * 1000)

static const char *TAG = "avatar_display";
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_transfer_done;
static bool s_initialized;

static bool IRAM_ATTR transfer_complete(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *event_data,
    void *user_context)
{
    (void)panel_io;
    (void)event_data;
    (void)user_context;

    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_transfer_done, &task_woken);
    return task_woken == pdTRUE;
}

esp_err_t avatar_display_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /*
     * Reset the production panel explicitly before the SPI peripheral owns
     * the shared pins. GPIO4 also selects the EEPROM, but no clock edges are
     * generated during this pulse, so the EEPROM remains unaffected.
     */
    const gpio_config_t reset_config = {
        .pin_bit_mask = 1ULL << LCD_PIN_RESET,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&reset_config), TAG, "LCD reset GPIO failed");
    gpio_set_level(LCD_PIN_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_PIN_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_PIN_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    const spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_PIN_SCK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz =
            AVATAR_SCREEN_WIDTH * AVATAR_TRANSFER_ROWS * sizeof(uint16_t) + 16,
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO),
        TAG,
        "SPI bus initialization failed");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi(
            (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
            &io_config,
            &s_io),
        TAG,
        "LCD IO initialization failed");

    s_transfer_done = xSemaphoreCreateBinary();
    if (s_transfer_done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = transfer_complete,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_register_event_callbacks(s_io, &callbacks, NULL),
        TAG,
        "LCD completion callback registration failed");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        /* Assets and CPU-generated pixels are uint16_t values in ESP32-C3
         * little-endian memory. Tell the ST7789 to consume the low byte
         * first; otherwise RGB565 pixels become psychedelic color bands. */
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel),
        TAG,
        "ST7789 initialization failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "LCD reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "LCD init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "LCD invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), TAG, "LCD rotation failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, false, true), TAG, "LCD mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 0, 0), TAG, "LCD gap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "LCD power-on failed");

    s_initialized = true;
    ESP_LOGI(TAG, "ST7789 ready: %dx%d at %d MHz",
             AVATAR_SCREEN_WIDTH,
             AVATAR_SCREEN_HEIGHT,
             LCD_CLOCK_HZ / 1000000);
    return ESP_OK;
}

esp_err_t avatar_display_draw_strip(int y, int height, const uint16_t *pixels)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixels == NULL || y < 0 || height <= 0 || y + height > AVATAR_SCREEN_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t error = esp_lcd_panel_draw_bitmap(
        s_panel,
        0,
        y,
        AVATAR_SCREEN_WIDTH,
        y + height,
        pixels);
    if (error != ESP_OK) {
        return error;
    }

    if (xSemaphoreTake(s_transfer_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "timed out waiting for LCD transfer");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

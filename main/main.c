#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "avatar_display.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "trapdoor.h"

#define STRIP_ROWS AVATAR_TRANSFER_ROWS
#define BUTTON_1_PIN GPIO_NUM_10
#define BUTTON_2_PIN GPIO_NUM_9
#define BUTTON_I2C_PORT I2C_NUM_0
#define BUTTON_I2C_SDA GPIO_NUM_20
#define BUTTON_I2C_SCL GPIO_NUM_21
#define BUTTON_I2C_HZ 100000
#define PCF8574_ADDRESS 0x20
#define PCF_BUTTON_1_MASK (1U << 3)
#define PCF_BUTTON_3_MASK (1U << 1)
#define PCF_BUTTON_4_MASK (1U << 2)

/* Symbols emitted by target_add_binary_data in main/CMakeLists.txt. */
extern const uint8_t avatar_data_start[] asm("_binary_avatar_rgb565_bin_start");
extern const uint8_t avatar_data_end[] asm("_binary_avatar_rgb565_bin_end");
extern const uint8_t meme_data_start[] asm("_binary_meme_rgb565_bin_start");
extern const uint8_t meme_data_end[] asm("_binary_meme_rgb565_bin_end");

static const char *TAG = "showcase";

typedef enum {
    SCENE_AVATAR,
    SCENE_NAME,
    SCENE_MEME,
    SCENE_COUNT,
} scene_t;

typedef enum {
    BUTTON_EVENT_NONE,
    BUTTON_EVENT_LEFTMOST,
    BUTTON_EVENT_MIDDLE_LEFT,
    BUTTON_EVENT_MIDDLE_RIGHT,
    BUTTON_EVENT_RIGHTMOST,
} button_event_t;

typedef struct {
    bool button_1_down;
    bool button_2_down;
    bool button_3_down;
    bool button_4_down;
    bool pcf_ready;
    uint8_t pcf_port;
} button_state_t;

typedef struct {
    char character;
    uint8_t columns[5];
} glyph_t;

/* Compact 5x7 font: only glyphs needed by the badge messages are retained. */
static const glyph_t FONT[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'!', {0x00, 0x00, 0x5f, 0x00, 0x00}},
    {'\'', {0x00, 0x05, 0x03, 0x00, 0x00}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'/', {0x20, 0x10, 0x08, 0x04, 0x02}},
    {'0', {0x3e, 0x51, 0x49, 0x45, 0x3e}},
    {'1', {0x00, 0x42, 0x7f, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4b, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7f, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3c, 0x4a, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1e}},
    {'?', {0x02, 0x01, 0x51, 0x09, 0x06}},
    {'_', {0x40, 0x40, 0x40, 0x40, 0x40}},
    {'A', {0x7e, 0x11, 0x11, 0x11, 0x7e}},
    {'B', {0x7f, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3e, 0x41, 0x41, 0x41, 0x22}},
    {'D', {0x7f, 0x41, 0x41, 0x22, 0x1c}},
    {'E', {0x7f, 0x49, 0x49, 0x49, 0x41}},
    {'F', {0x7f, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3e, 0x41, 0x49, 0x49, 0x7a}},
    {'H', {0x7f, 0x08, 0x08, 0x08, 0x7f}},
    {'I', {0x00, 0x41, 0x7f, 0x41, 0x00}},
    {'J', {0x20, 0x40, 0x41, 0x3f, 0x01}},
    {'K', {0x7f, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7f, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7f, 0x02, 0x0c, 0x02, 0x7f}},
    {'N', {0x7f, 0x04, 0x08, 0x10, 0x7f}},
    {'O', {0x3e, 0x41, 0x41, 0x41, 0x3e}},
    {'P', {0x7f, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3e, 0x41, 0x51, 0x21, 0x5e}},
    {'R', {0x7f, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7f, 0x01, 0x01}},
    {'U', {0x3f, 0x40, 0x40, 0x40, 0x3f}},
    {'V', {0x1f, 0x20, 0x40, 0x20, 0x1f}},
    {'W', {0x3f, 0x40, 0x38, 0x40, 0x3f}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
    {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
};

#define FONT_GLYPH_COUNT (sizeof(FONT) / sizeof(FONT[0]))

static esp_err_t badge_buttons_init(button_state_t *state)
{
    const gpio_config_t direct_button_config = {
        .pin_bit_mask = (1ULL << BUTTON_1_PIN) | (1ULL << BUTTON_2_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(
        gpio_config(&direct_button_config),
        TAG,
        "direct button GPIO setup failed");

    const i2c_config_t i2c_config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BUTTON_I2C_SDA,
        .scl_io_num = BUTTON_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BUTTON_I2C_HZ,
        .clk_flags = 0,
    };
    esp_err_t error = i2c_param_config(BUTTON_I2C_PORT, &i2c_config);
    if (error == ESP_OK) {
        error = i2c_driver_install(BUTTON_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    }

    state->pcf_ready = false;
    state->pcf_port = 0xff;
    if (error == ESP_OK) {
        const uint8_t all_inputs = 0xff;
        error = i2c_master_write_to_device(
            BUTTON_I2C_PORT,
            PCF8574_ADDRESS,
            &all_inputs,
            sizeof(all_inputs),
            pdMS_TO_TICKS(20));
    }
    if (error == ESP_OK) {
        error = i2c_master_read_from_device(
            BUTTON_I2C_PORT,
            PCF8574_ADDRESS,
            &state->pcf_port,
            sizeof(state->pcf_port),
            pdMS_TO_TICKS(20));
    }
    if (error == ESP_OK) {
        state->pcf_ready = true;
        ESP_LOGI(TAG, "PCF8574 buttons ready at 0x%02x, state=0x%02x",
                 PCF8574_ADDRESS,
                 state->pcf_port);
    } else {
        ESP_LOGW(TAG, "PCF8574 unavailable (%s); buttons 1 and 2 still work",
                 esp_err_to_name(error));
    }

    vTaskDelay(pdMS_TO_TICKS(25));

    state->button_1_down = gpio_get_level(BUTTON_1_PIN) == 0 ||
        (state->pcf_ready && (state->pcf_port & PCF_BUTTON_1_MASK) == 0);
    state->button_2_down = gpio_get_level(BUTTON_2_PIN) == 0;
    state->button_3_down = state->pcf_ready &&
        (state->pcf_port & PCF_BUTTON_3_MASK) == 0;
    state->button_4_down = state->pcf_ready &&
        (state->pcf_port & PCF_BUTTON_4_MASK) == 0;
    return ESP_OK;
}

static button_event_t poll_buttons(button_state_t *state)
{
    if (state->pcf_ready) {
        uint8_t port = 0xff;
        if (i2c_master_read_from_device(
                BUTTON_I2C_PORT,
                PCF8574_ADDRESS,
                &port,
                sizeof(port),
                pdMS_TO_TICKS(5)) == ESP_OK) {
            state->pcf_port = port;
        }
    }

    const bool button_1_down = gpio_get_level(BUTTON_1_PIN) == 0 ||
        (state->pcf_ready && (state->pcf_port & PCF_BUTTON_1_MASK) == 0);
    const bool button_2_down = gpio_get_level(BUTTON_2_PIN) == 0;
    const bool button_3_down = state->pcf_ready &&
        (state->pcf_port & PCF_BUTTON_3_MASK) == 0;
    const bool button_4_down = state->pcf_ready &&
        (state->pcf_port & PCF_BUTTON_4_MASK) == 0;
    button_event_t event = BUTTON_EVENT_NONE;

    if (button_1_down && !state->button_1_down) {
        /* TACT_A is the third green button from the left. */
        event = BUTTON_EVENT_MIDDLE_RIGHT;
    } else if (button_2_down && !state->button_2_down) {
        /* TACT_B is the rightmost green button. */
        event = BUTTON_EVENT_RIGHTMOST;
    } else if (button_3_down && !state->button_3_down) {
        /* TACT_C is the leftmost green button. */
        event = BUTTON_EVENT_LEFTMOST;
    } else if (button_4_down && !state->button_4_down) {
        /* TACT_D is the second green button from the left. */
        event = BUTTON_EVENT_MIDDLE_LEFT;
    }

    state->button_1_down = button_1_down;
    state->button_2_down = button_2_down;
    state->button_3_down = button_3_down;
    state->button_4_down = button_4_down;
    return event;
}

static const char *scene_label(scene_t scene)
{
    static const char *labels[SCENE_COUNT] = {"avatar", "SHREY", "meme"};
    return labels[scene];
}

static uint16_t rgb565(unsigned red, unsigned green, unsigned blue)
{
    return (uint16_t)(((red & 0xf8) << 8) | ((green & 0xfc) << 3) | (blue >> 3));
}

static const uint8_t *glyph_for(char character)
{
    for (size_t index = 0; index < FONT_GLYPH_COUNT; ++index) {
        if (FONT[index].character == character) {
            return FONT[index].columns;
        }
    }
    return FONT[0].columns;
}

static int text_width(const char *text, int scale)
{
    const size_t length = strlen(text);
    return length == 0 ? 0 : (int)(length * 6 - 1) * scale;
}

static bool text_pixel(
    const char *text,
    size_t visible_characters,
    int x,
    int y,
    int origin_x,
    int origin_y,
    int scale)
{
    const int local_x = x - origin_x;
    const int local_y = y - origin_y;
    if (local_x < 0 || local_y < 0 || local_y >= 7 * scale) {
        return false;
    }

    const size_t character_index = (size_t)(local_x / (6 * scale));
    if (character_index >= visible_characters || text[character_index] == '\0') {
        return false;
    }

    const int character_x = local_x % (6 * scale);
    if (character_x >= 5 * scale) {
        return false;
    }

    const int column = character_x / scale;
    const int row = local_y / scale;
    return ((glyph_for(text[character_index])[column] >> row) & 1U) != 0;
}

static uint16_t name_background(int x, int y, unsigned frame)
{
    unsigned red = 2 + (unsigned)y * 5 / AVATAR_SCREEN_HEIGHT;
    unsigned green = 4 + (unsigned)x * 6 / AVATAR_SCREEN_WIDTH;
    unsigned blue = 16 + (unsigned)y * 14 / AVATAR_SCREEN_HEIGHT;

    if (((x + (int)frame) % 40) < 2 || ((y + (int)frame / 2) % 32) < 2) {
        red += 3;
        green += 8;
        blue += 18;
    }
    if (((x * 17 + y * 31 + (int)frame * 13) % 997) < 3) {
        red = 20;
        green = 180;
        blue = 220;
    }
    return rgb565(red, green, blue);
}

static void render_name_scene(uint16_t *strip)
{
    static const char eyebrow[] = "HELLO! I'M";
    static const char name[] = "SHREY";
    static const char message[] = "BUILD MODE: ON";
    const unsigned frame_number = 28;
    const int eyebrow_x = (AVATAR_SCREEN_WIDTH - text_width(eyebrow, 3)) / 2;
    const int name_x = (AVATAR_SCREEN_WIDTH - text_width(name, 7)) / 2;
    const int message_x = (AVATAR_SCREEN_WIDTH - text_width(message, 3)) / 2;
    const unsigned pulse = 210 + ((frame_number / 4) % 8) * 6;

    for (int output_y = 0; output_y < AVATAR_SCREEN_HEIGHT; output_y += STRIP_ROWS) {
        const int rows = output_y + STRIP_ROWS <= AVATAR_SCREEN_HEIGHT
            ? STRIP_ROWS
            : AVATAR_SCREEN_HEIGHT - output_y;

        for (int local_y = 0; local_y < rows; ++local_y) {
            const int y = output_y + local_y;
            for (int x = 0; x < AVATAR_SCREEN_WIDTH; ++x) {
                uint16_t color = name_background(x, y, frame_number);

                if (text_pixel(name, strlen(name), x - 3, y - 3, name_x, 82, 7)) {
                    color = rgb565(26, 8, 70);
                }
                if (text_pixel(eyebrow, strlen(eyebrow), x, y, eyebrow_x, 31, 3)) {
                    color = rgb565(255, 174, 28);
                }
                if (text_pixel(name, strlen(name), x, y, name_x, 82, 7)) {
                    const unsigned shimmer = (unsigned)(x + (int)frame_number * 7) % 95;
                    color = shimmer < 14
                        ? rgb565(255, 202, 42)
                        : rgb565(30, pulse, 255);
                }
                if (y >= 151 && y <= 154 && x >= name_x && x < name_x + text_width(name, 7)) {
                    color = rgb565(255, 174, 28);
                }
                if (text_pixel(message, strlen(message), x - 2, y - 2, message_x, 185, 3)) {
                    color = rgb565(20, 5, 60);
                }
                if (text_pixel(message, strlen(message), x, y, message_x, 185, 3)) {
                    color = rgb565(114, 255, 196);
                }

                strip[local_y * AVATAR_SCREEN_WIDTH + x] = color;
            }
        }
        ESP_ERROR_CHECK(avatar_display_draw_strip(output_y, rows, strip));
    }
}

static void render_static_bitmap(const uint16_t *bitmap, uint16_t *strip)
{
    for (int output_y = 0; output_y < AVATAR_SCREEN_HEIGHT; output_y += STRIP_ROWS) {
        const int rows = output_y + STRIP_ROWS <= AVATAR_SCREEN_HEIGHT
            ? STRIP_ROWS
            : AVATAR_SCREEN_HEIGHT - output_y;
        memcpy(
            strip,
            bitmap + output_y * AVATAR_SCREEN_WIDTH,
            (size_t)rows * AVATAR_SCREEN_WIDTH * sizeof(uint16_t));
        ESP_ERROR_CHECK(avatar_display_draw_strip(output_y, rows, strip));
    }
}

static void render_static_scene(
    scene_t scene,
    const uint16_t *avatar,
    const uint16_t *meme,
    uint16_t *strip)
{
    if (scene == SCENE_AVATAR) {
        render_static_bitmap(avatar, strip);
    } else if (scene == SCENE_NAME) {
        render_name_scene(strip);
    } else {
        render_static_bitmap(meme, strip);
    }
}

static uint16_t trapdoor_background(int x, int y)
{
    unsigned green = 5 + (unsigned)y * 10 / AVATAR_SCREEN_HEIGHT;
    if ((y % 16) == 0) {
        green += 5;
    }
    if (((x * 19 + y * 37) % 1601) < 2) {
        return rgb565(28, 90, 48);
    }
    return rgb565(2, green, 7);
}

static void render_trapdoor_scene(
    const trapdoor_snapshot_t *snapshot,
    const uint16_t *meme,
    uint16_t *strip)
{
    if (snapshot->unlocked) {
        render_static_bitmap(meme, strip);
        return;
    }

    static const char title[] = "OPERATION KAAM";
    static const char wifi[] = "WIFI: " TRAPDOOR_SSID;
    static const char password[] = "PASS: " TRAPDOOR_PASSWORD;
    static const char footer[] = "HOLD BUTTON 4: EXIT";
    char players[28];
    char authorization[32];
    char vault[28];
    snprintf(players, sizeof(players), "WIFI:%u ROLES:%u/2",
             snapshot->connected_players, snapshot->role_count);
    snprintf(vault, sizeof(vault), "STAGE: %s", trapdoor_stage_name(snapshot->stage));
    switch (snapshot->stage) {
    case TRAPDOOR_STAGE_RECON:
        strlcpy(authorization, "WEB RECON: ACTIVE", sizeof(authorization));
        break;
    case TRAPDOOR_STAGE_DEFUSAL:
        snprintf(authorization, sizeof(authorization), "DEFUSE:%u/%u T:%u",
                 snapshot->sequence_progress,
                 snapshot->sequence_length,
                 snapshot->seconds_remaining);
        break;
    case TRAPDOOR_STAGE_BLE_GHOST:
        strlcpy(authorization, "BLE: SHREY_GHOST", sizeof(authorization));
        break;
    case TRAPDOOR_STAGE_COOP_READY:
        strlcpy(authorization, "BLE SOLVED: GET ROLES", sizeof(authorization));
        break;
    case TRAPDOOR_STAGE_SYNC:
        snprintf(authorization, sizeof(authorization), "SYNC:%u/2 T:%u",
                 snapshot->sync_count, snapshot->seconds_remaining);
        break;
    case TRAPDOOR_STAGE_UNLOCKED:
        strlcpy(authorization, "DUAL AUTH: ACCEPTED", sizeof(authorization));
        break;
    default:
        strlcpy(authorization, "MISSION STATE: UNKNOWN", sizeof(authorization));
        break;
    }

    const int title_x = (AVATAR_SCREEN_WIDTH - text_width(title, 3)) / 2;
    const int wifi_x = (AVATAR_SCREEN_WIDTH - text_width(wifi, 2)) / 2;
    const int password_x = (AVATAR_SCREEN_WIDTH - text_width(password, 2)) / 2;
    const int players_x = (AVATAR_SCREEN_WIDTH - text_width(players, 2)) / 2;
    const int authorization_x =
        (AVATAR_SCREEN_WIDTH - text_width(authorization, 2)) / 2;
    const int vault_x = (AVATAR_SCREEN_WIDTH - text_width(vault, 2)) / 2;
    const int footer_x = (AVATAR_SCREEN_WIDTH - text_width(footer, 1)) / 2;

    for (int output_y = 0; output_y < AVATAR_SCREEN_HEIGHT; output_y += STRIP_ROWS) {
        const int rows = output_y + STRIP_ROWS <= AVATAR_SCREEN_HEIGHT
            ? STRIP_ROWS
            : AVATAR_SCREEN_HEIGHT - output_y;

        for (int local_y = 0; local_y < rows; ++local_y) {
            const int y = output_y + local_y;
            for (int x = 0; x < AVATAR_SCREEN_WIDTH; ++x) {
                uint16_t color = trapdoor_background(x, y);
                if (y == 52 || y == 113 || y == 215) {
                    color = rgb565(22, 110, 58);
                }
                if (text_pixel(title, strlen(title), x, y, title_x, 20, 3)) {
                    color = rgb565(255, 181, 46);
                } else if (text_pixel(wifi, strlen(wifi), x, y, wifi_x, 65, 2) ||
                           text_pixel(password, strlen(password), x, y, password_x, 89, 2)) {
                    color = rgb565(98, 255, 155);
                } else if (text_pixel(players, strlen(players), x, y, players_x, 129, 2)) {
                    color = rgb565(185, 255, 205);
                } else if (text_pixel(authorization, strlen(authorization),
                                      x, y, authorization_x, 157, 2)) {
                    color = snapshot->stage >= TRAPDOOR_STAGE_BLE_GHOST
                        ? rgb565(255, 181, 46)
                        : rgb565(98, 255, 155);
                } else if (text_pixel(vault, strlen(vault), x, y, vault_x, 185, 2)) {
                    color = snapshot->stage >= TRAPDOOR_STAGE_BLE_GHOST
                        ? rgb565(255, 181, 46)
                        : rgb565(98, 255, 155);
                } else if (text_pixel(footer, strlen(footer), x, y, footer_x, 224, 1)) {
                    color = rgb565(115, 150, 124);
                }
                strip[local_y * AVATAR_SCREEN_WIDTH + x] = color;
            }
        }
        ESP_ERROR_CHECK(avatar_display_draw_strip(output_y, rows, strip));
    }
}

static unsigned physical_button_number(button_event_t event)
{
    switch (event) {
    case BUTTON_EVENT_LEFTMOST:
        return 1;
    case BUTTON_EVENT_MIDDLE_LEFT:
        return 2;
    case BUTTON_EVENT_MIDDLE_RIGHT:
        return 3;
    case BUTTON_EVENT_RIGHTMOST:
        return 4;
    default:
        return 0;
    }
}

static bool asset_has_expected_size(
    const char *label,
    const uint8_t *start,
    const uint8_t *end)
{
    const size_t expected_size =
        AVATAR_SCREEN_WIDTH * AVATAR_SCREEN_HEIGHT * sizeof(uint16_t);
    const size_t actual_size = (size_t)(end - start);
    if (actual_size == expected_size) {
        return true;
    }

    ESP_LOGE(TAG, "%s asset is %u bytes; expected %u",
             label,
             (unsigned)actual_size,
             (unsigned)expected_size);
    return false;
}

void app_main(void)
{
    if (!asset_has_expected_size("avatar", avatar_data_start, avatar_data_end) ||
        !asset_has_expected_size("meme", meme_data_start, meme_data_end)) {
        return;
    }

    ESP_ERROR_CHECK(avatar_display_init());

    button_state_t buttons;
    ESP_ERROR_CHECK(badge_buttons_init(&buttons));

    uint16_t *strip = heap_caps_malloc(
        AVATAR_SCREEN_WIDTH * STRIP_ROWS * sizeof(uint16_t),
        MALLOC_CAP_DMA);
    if (strip == NULL) {
        ESP_LOGE(TAG, "not enough DMA memory for animation buffer");
        return;
    }

    const uint16_t *avatar = (const uint16_t *)avatar_data_start;
    const uint16_t *meme = (const uint16_t *)meme_data_start;
    ESP_LOGI(TAG, "starting SHREY Trapdoor + static gallery");
    ESP_LOGI(TAG, "Operation KAAM: web + timed buttons + BLE + dual-phone finale");
    ESP_LOGI(TAG, "hold button 4 for two seconds to toggle gallery mode");

    scene_t scene = SCENE_AVATAR;
    bool trapdoor_mode = trapdoor_start() == ESP_OK;
    bool redraw = true;
    int64_t button_4_pressed_at = 0;
    bool button_4_hold_handled = false;

    for (;;) {
        const button_event_t event = poll_buttons(&buttons);
        if (event == BUTTON_EVENT_RIGHTMOST) {
            button_4_pressed_at = esp_timer_get_time();
            button_4_hold_handled = false;
        }

        if (buttons.button_2_down && !button_4_hold_handled && button_4_pressed_at != 0 &&
            esp_timer_get_time() - button_4_pressed_at >= 2000000) {
            button_4_hold_handled = true;
            if (trapdoor_mode) {
                ESP_ERROR_CHECK_WITHOUT_ABORT(trapdoor_stop());
                trapdoor_mode = false;
                scene = SCENE_AVATAR;
            } else {
                if (trapdoor_start() == ESP_OK) {
                    trapdoor_mode = true;
                }
            }
            redraw = true;
        } else if (!buttons.button_2_down) {
            button_4_pressed_at = 0;
            button_4_hold_handled = false;
        }

        if (event != BUTTON_EVENT_NONE && trapdoor_mode) {
            trapdoor_process_button(physical_button_number(event));
            redraw = true;
        } else if (event == BUTTON_EVENT_LEFTMOST && !trapdoor_mode) {
            scene = SCENE_AVATAR;
            redraw = true;
        } else if (event == BUTTON_EVENT_MIDDLE_LEFT && !trapdoor_mode) {
            scene = SCENE_NAME;
            redraw = true;
        } else if (event == BUTTON_EVENT_MIDDLE_RIGHT && !trapdoor_mode) {
            scene = SCENE_MEME;
            redraw = true;
        } else if (event == BUTTON_EVENT_RIGHTMOST && !trapdoor_mode) {
            scene = (scene_t)((scene + 1) % SCENE_COUNT);
            redraw = true;
        }

        if (trapdoor_mode) {
            trapdoor_tick();
        }
        if (trapdoor_mode && trapdoor_take_display_dirty()) {
            redraw = true;
        }
        if (redraw) {
            if (trapdoor_mode) {
                trapdoor_snapshot_t snapshot;
                trapdoor_get_snapshot(&snapshot);
                render_trapdoor_scene(&snapshot, meme, strip);
                ESP_LOGI(TAG, "trapdoor display: stage=%s players=%u roles=%u unlocked=%d",
                         trapdoor_stage_name(snapshot.stage),
                         snapshot.connected_players,
                         snapshot.role_count,
                         snapshot.unlocked);
            } else {
                render_static_scene(scene, avatar, meme, strip);
                ESP_LOGI(TAG, "gallery scene: %s", scene_label(scene));
            }
            redraw = false;
        }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

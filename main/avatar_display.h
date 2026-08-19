#pragma once

#include <stdint.h>
#include "esp_err.h"

#define AVATAR_SCREEN_WIDTH  320
#define AVATAR_SCREEN_HEIGHT 240
#define AVATAR_TRANSFER_ROWS 32

esp_err_t avatar_display_init(void);
esp_err_t avatar_display_draw_strip(int y, int height, const uint16_t *pixels);

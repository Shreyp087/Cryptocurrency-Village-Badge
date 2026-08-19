#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t ghost_ble_init(void);
void ghost_ble_set_active(bool active);
bool ghost_ble_is_active(void);

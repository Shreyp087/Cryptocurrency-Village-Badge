#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define TRAPDOOR_SSID "SHREY_DEFCON"
#define TRAPDOOR_PASSWORD "4885-HEIST"

typedef enum {
    TRAPDOOR_STAGE_RECON,
    TRAPDOOR_STAGE_DEFUSAL,
    TRAPDOOR_STAGE_BLE_GHOST,
    TRAPDOOR_STAGE_COOP_READY,
    TRAPDOOR_STAGE_SYNC,
    TRAPDOOR_STAGE_UNLOCKED,
} trapdoor_stage_t;

typedef struct {
    bool wifi_running;
    bool physical_authorized;
    bool unlocked;
    bool ble_active;
    unsigned connected_players;
    unsigned sequence_progress;
    unsigned sequence_length;
    unsigned seconds_remaining;
    unsigned attempts;
    unsigned role_count;
    unsigned sync_count;
    trapdoor_stage_t stage;
} trapdoor_snapshot_t;

esp_err_t trapdoor_start(void);
esp_err_t trapdoor_stop(void);
bool trapdoor_is_running(void);
void trapdoor_process_button(unsigned button_number);
void trapdoor_tick(void);
void trapdoor_ble_solved(void);
void trapdoor_get_snapshot(trapdoor_snapshot_t *snapshot);
bool trapdoor_take_display_dirty(void);
const char *trapdoor_stage_name(trapdoor_stage_t stage);

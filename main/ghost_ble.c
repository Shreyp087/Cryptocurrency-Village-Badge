#include "ghost_ble.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "trapdoor.h"

#define GHOST_NAME "SHREY_GHOST"

static const char *TAG = "ghost_ble";
static bool s_initialized;
static volatile bool s_active;
static volatile bool s_synced;
static uint8_t s_own_addr_type;
static uint16_t s_status_handle;
static uint16_t s_answer_handle;
static uint16_t s_connection_handle = BLE_HS_CONN_HANDLE_NONE;

/* 7e57d001-4b41-414d-8000-534852455900 */
static const ble_uuid128_t GHOST_SERVICE_UUID = BLE_UUID128_INIT(
    0x00, 0x59, 0x45, 0x52, 0x48, 0x53, 0x00, 0x80,
    0x4d, 0x41, 0x41, 0x4b, 0x01, 0xd0, 0x57, 0x7e);
/* ...d002: clue read */
static const ble_uuid128_t GHOST_CLUE_UUID = BLE_UUID128_INIT(
    0x00, 0x59, 0x45, 0x52, 0x48, 0x53, 0x00, 0x80,
    0x4d, 0x41, 0x41, 0x4b, 0x02, 0xd0, 0x57, 0x7e);
/* ...d003: answer write */
static const ble_uuid128_t GHOST_ANSWER_UUID = BLE_UUID128_INIT(
    0x00, 0x59, 0x45, 0x52, 0x48, 0x53, 0x00, 0x80,
    0x4d, 0x41, 0x41, 0x4b, 0x03, 0xd0, 0x57, 0x7e);
/* ...d004: status read + notify */
static const ble_uuid128_t GHOST_STATUS_UUID = BLE_UUID128_INIT(
    0x00, 0x59, 0x45, 0x52, 0x48, 0x53, 0x00, 0x80,
    0x4d, 0x41, 0x41, 0x4b, 0x04, 0xd0, 0x57, 0x7e);

static int ghost_gap_event(struct ble_gap_event *event, void *argument);

static int append_text(struct os_mbuf *buffer, const char *text)
{
    return os_mbuf_append(buffer, text, strlen(text)) == 0
        ? 0
        : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static bool answer_is_kaam(struct os_mbuf *buffer)
{
    char answer[16] = {0};
    uint16_t length = 0;
    if (ble_hs_mbuf_to_flat(buffer, answer, sizeof(answer) - 1, &length) != 0) {
        return false;
    }
    answer[length] = '\0';
    while (length > 0 && isspace((unsigned char)answer[length - 1])) {
        answer[--length] = '\0';
    }
    char *start = answer;
    while (isspace((unsigned char)*start)) {
        ++start;
    }
    for (char *cursor = start; *cursor != '\0'; ++cursor) {
        *cursor = (char)toupper((unsigned char)*cursor);
    }
    return strcmp(start, "KAAM") == 0;
}

static int ghost_access(
    uint16_t connection_handle,
    uint16_t attribute_handle,
    struct ble_gatt_access_ctxt *context,
    void *argument)
{
    (void)connection_handle;
    (void)argument;

    if (!s_active) {
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    if (context->op == BLE_GATT_ACCESS_OP_READ_CHR &&
        ble_uuid_cmp(context->chr->uuid, &GHOST_CLUE_UUID.u) == 0) {
        return append_text(context->om,
            "FRAGMENT=NDDP; CIPHER=CAESAR; SHIFT=-3; WRITE PLAINTEXT TO D003");
    }
    if (context->op == BLE_GATT_ACCESS_OP_READ_CHR &&
        attribute_handle == s_status_handle) {
        return append_text(context->om,
            "MORSE ...- .- ..- .-.. - / SUBSCRIBE FOR LIVE STATUS");
    }
    if (context->op == BLE_GATT_ACCESS_OP_WRITE_CHR &&
        attribute_handle == s_answer_handle) {
        if (!answer_is_kaam(context->om)) {
            ESP_LOGW(TAG, "incorrect ghost answer written");
            return BLE_ATT_ERR_UNLIKELY;
        }
        ESP_LOGI(TAG, "ghost answer accepted");
        trapdoor_ble_solved();
        ble_gatts_chr_updated(s_status_handle);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def GHOST_SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &GHOST_SERVICE_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &GHOST_CLUE_UUID.u,
                .access_cb = ghost_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &GHOST_ANSWER_UUID.u,
                .access_cb = ghost_access,
                .flags = BLE_GATT_CHR_F_WRITE,
                .val_handle = &s_answer_handle,
            },
            {
                .uuid = &GHOST_STATUS_UUID.u,
                .access_cb = ghost_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_status_handle,
            },
            {0},
        },
    },
    {0},
};

static void ghost_advertise(void)
{
    if (!s_active || !s_synced || ble_gap_adv_active()) {
        return;
    }

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)GHOST_NAME;
    fields.name_len = strlen(GHOST_NAME);
    fields.name_is_complete = 1;
    const int fields_result = ble_gap_adv_set_fields(&fields);
    if (fields_result != 0) {
        ESP_LOGE(TAG, "advertising fields failed: %d", fields_result);
        return;
    }

    struct ble_gap_adv_params parameters = {0};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    const int result = ble_gap_adv_start(
        s_own_addr_type,
        NULL,
        BLE_HS_FOREVER,
        &parameters,
        ghost_gap_event,
        NULL);
    if (result == 0) {
        ESP_LOGI(TAG, "BLE ghost visible as %s", GHOST_NAME);
    } else {
        ESP_LOGE(TAG, "advertising start failed: %d", result);
    }
}

static int ghost_gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_connection_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "operator connected to BLE ghost");
        } else {
            ghost_advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "operator disconnected from BLE ghost");
        ghost_advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_status_handle &&
            event->subscribe.cur_notify && s_connection_handle != BLE_HS_CONN_HANDLE_NONE) {
            const char notification[] = "STATUS=GHOST_AWAKE; NEXT=READ_D002";
            struct os_mbuf *buffer = ble_hs_mbuf_from_flat(
                notification, sizeof(notification) - 1);
            if (buffer != NULL) {
                ble_gatts_notify_custom(
                    s_connection_handle, s_status_handle, buffer);
            }
        }
        return 0;
    default:
        return 0;
    }
}

static void ghost_on_reset(int reason)
{
    s_synced = false;
    ESP_LOGW(TAG, "NimBLE reset: %d", reason);
}

static void ghost_on_sync(void)
{
    int result = ble_hs_util_ensure_addr(0);
    if (result == 0) {
        result = ble_hs_id_infer_auto(0, &s_own_addr_type);
    }
    if (result != 0) {
        ESP_LOGE(TAG, "BLE identity unavailable: %d", result);
        return;
    }
    s_synced = true;
    ESP_LOGI(TAG, "BLE host synchronized; ghost is sleeping");
    ghost_advertise();
}

static void ghost_host_task(void *argument)
{
    (void)argument;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ghost_ble_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    esp_err_t error = nimble_port_init();
    if (error != ESP_OK) {
        return error;
    }

    ble_hs_cfg.reset_cb = ghost_on_reset;
    ble_hs_cfg.sync_cb = ghost_on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int result = ble_gatts_count_cfg(GHOST_SERVICES);
    if (result == 0) {
        result = ble_gatts_add_svcs(GHOST_SERVICES);
    }
    if (result == 0) {
        result = ble_svc_gap_device_name_set(GHOST_NAME);
    }
    if (result != 0) {
        ESP_LOGE(TAG, "GATT setup failed: %d", result);
        return ESP_FAIL;
    }

    s_initialized = true;
    nimble_port_freertos_init(ghost_host_task);
    return ESP_OK;
}

void ghost_ble_set_active(bool active)
{
    s_active = active;
    if (!s_synced) {
        return;
    }
    if (active) {
        ghost_advertise();
    } else {
        if (ble_gap_adv_active()) {
            ble_gap_adv_stop();
        }
        if (s_connection_handle != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(s_connection_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
}

bool ghost_ble_is_active(void)
{
    return s_active;
}

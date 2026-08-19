#include "trapdoor.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dhcpserver/dhcpserver.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ghost_ble.h"
#include "nvs_flash.h"

#define TRAPDOOR_CHANNEL 6
#define TRAPDOOR_MAX_PLAYERS 4
#define DNS_PORT 53
#define DNS_PACKET_MAX 512
#define DEFUSAL_SEQUENCE_LENGTH 6
#define DEFUSAL_SECONDS 45
#define SYNC_SECONDS 15

extern const char portal_html_start[] asm("_binary_portal_html_start");

static const char *TAG = "trapdoor";

typedef struct {
    bool wifi_running;
    bool physical_authorized;
    bool unlocked;
    bool display_dirty;
    bool recon_complete;
    unsigned connected_players;
    unsigned sequence_progress;
    unsigned attempts;
    uint8_t sequence[DEFUSAL_SEQUENCE_LENGTH];
    trapdoor_stage_t stage;
    int64_t deadline_us;
    unsigned last_display_seconds;
    uint32_t operator_ip[2];
    uint8_t sync_mask;
} game_state_t;

static SemaphoreHandle_t s_lock;
static game_state_t s_game;
static bool s_platform_initialized;
static bool s_wifi_initialized;
static esp_netif_t *s_ap_netif;
static esp_event_handler_instance_t s_wifi_event_instance;
static httpd_handle_t s_http_server;
static TaskHandle_t s_dns_task;
static volatile bool s_dns_should_run;
static volatile int s_dns_socket = -1;

static void state_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void state_unlock(void)
{
    xSemaphoreGive(s_lock);
}

const char *trapdoor_stage_name(trapdoor_stage_t stage)
{
    switch (stage) {
    case TRAPDOOR_STAGE_RECON:
        return "RECON";
    case TRAPDOOR_STAGE_DEFUSAL:
        return "DEFUSAL";
    case TRAPDOOR_STAGE_BLE_GHOST:
        return "BLE_GHOST";
    case TRAPDOOR_STAGE_COOP_READY:
        return "COOP_READY";
    case TRAPDOOR_STAGE_SYNC:
        return "SYNC";
    case TRAPDOOR_STAGE_UNLOCKED:
        return "UNLOCKED";
    default:
        return "UNKNOWN";
    }
}

static unsigned bit_count(uint8_t value)
{
    unsigned count = 0;
    while (value != 0) {
        count += value & 1U;
        value >>= 1;
    }
    return count;
}

static unsigned role_count_locked(void)
{
    return (s_game.operator_ip[0] != 0 ? 1U : 0U) +
           (s_game.operator_ip[1] != 0 ? 1U : 0U);
}

static unsigned seconds_remaining_locked(int64_t now_us)
{
    if (s_game.deadline_us <= now_us ||
        (s_game.stage != TRAPDOOR_STAGE_DEFUSAL &&
         s_game.stage != TRAPDOOR_STAGE_SYNC)) {
        return 0;
    }
    return (unsigned)((s_game.deadline_us - now_us + 999999) / 1000000);
}

static void wifi_event_handler(
    void *argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)argument;
    (void)event_base;
    (void)event_data;

    state_lock();
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        if (s_game.connected_players < TRAPDOOR_MAX_PLAYERS) {
            ++s_game.connected_players;
        }
        s_game.display_dirty = true;
        ESP_LOGI(TAG, "player joined (%u connected)", s_game.connected_players);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_game.connected_players > 0) {
            --s_game.connected_players;
        }
        s_game.display_dirty = true;
        ESP_LOGI(TAG, "player left (%u connected)", s_game.connected_players);
    }
    state_unlock();
}

static esp_err_t initialize_platform(void)
{
    if (s_platform_initialized) {
        return ESP_OK;
    }

    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    if (error != ESP_OK) {
        return error;
    }

    error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_platform_initialized = true;
    return ESP_OK;
}

static esp_err_t initialize_wifi(void)
{
    if (s_wifi_initialized) {
        return ESP_OK;
    }

    wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t error = esp_wifi_init(&initialization);
    if (error != ESP_OK) {
        return error;
    }
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        wifi_event_handler,
        NULL,
        &s_wifi_event_instance));

    wifi_config_t configuration = {0};
    strlcpy((char *)configuration.ap.ssid, TRAPDOOR_SSID, sizeof(configuration.ap.ssid));
    strlcpy((char *)configuration.ap.password, TRAPDOOR_PASSWORD, sizeof(configuration.ap.password));
    configuration.ap.ssid_len = strlen(TRAPDOOR_SSID);
    configuration.ap.channel = TRAPDOOR_CHANNEL;
    configuration.ap.authmode = WIFI_AUTH_WPA2_PSK;
    configuration.ap.max_connection = TRAPDOOR_MAX_PLAYERS;
    configuration.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &configuration));
    s_wifi_initialized = true;
    return ESP_OK;
}

static esp_err_t configure_captive_dns_offer(void)
{
    const dhcps_offer_t offer_dns = OFFER_DNS;
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = htonl(0xc0a80401U);

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(s_ap_netif));
    ESP_ERROR_CHECK(esp_netif_dhcps_option(
        s_ap_netif,
        ESP_NETIF_OP_SET,
        ESP_NETIF_DOMAIN_NAME_SERVER,
        (void *)&offer_dns,
        sizeof(offer_dns)));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(s_ap_netif));
    return ESP_OK;
}

static bool dns_question_end(const uint8_t *packet, size_t length, size_t *end)
{
    size_t offset = 12;
    while (offset < length && packet[offset] != 0) {
        const size_t label_length = packet[offset];
        if (label_length > 63 || offset + label_length + 1 >= length) {
            return false;
        }
        offset += label_length + 1;
    }
    if (offset + 5 > length) {
        return false;
    }
    *end = offset + 5;
    return true;
}

static void dns_server_task(void *argument)
{
    (void)argument;
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "DNS socket creation failed");
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_dns_socket = socket_fd;

    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(socket_fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(socket_fd);
        s_dns_socket = -1;
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "captive DNS ready on UDP/53");
    while (s_dns_should_run) {
        uint8_t packet[DNS_PACKET_MAX];
        struct sockaddr_in client;
        socklen_t client_length = sizeof(client);
        const int received = recvfrom(
            socket_fd,
            packet,
            sizeof(packet) - 16,
            0,
            (struct sockaddr *)&client,
            &client_length);
        if (received < 12) {
            continue;
        }

        size_t question_end = 0;
        if (!dns_question_end(packet, (size_t)received, &question_end)) {
            continue;
        }

        packet[2] = 0x81;
        packet[3] = 0x80;
        packet[6] = 0x00;
        packet[7] = 0x01;
        packet[8] = packet[9] = packet[10] = packet[11] = 0;

        uint8_t *answer = packet + question_end;
        const uint8_t captive_answer[] = {
            0xc0, 0x0c,
            0x00, 0x01,
            0x00, 0x01,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x04,
            192, 168, 4, 1,
        };
        memcpy(answer, captive_answer, sizeof(captive_answer));
        sendto(
            socket_fd,
            packet,
            question_end + sizeof(captive_answer),
            0,
            (const struct sockaddr *)&client,
            client_length);
    }

    close(socket_fd);
    s_dns_socket = -1;
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static uint32_t request_client_ip(httpd_req_t *request)
{
    struct sockaddr_storage peer = {0};
    socklen_t length = sizeof(peer);
    const int socket_fd = httpd_req_to_sockfd(request);
    if (socket_fd < 0 ||
        getpeername(socket_fd, (struct sockaddr *)&peer, &length) != 0 ||
        peer.ss_family != AF_INET) {
        return 0;
    }
    return ((struct sockaddr_in *)&peer)->sin_addr.s_addr;
}

static esp_err_t portal_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, portal_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t sector_handler(httpd_req_t *request)
{
    state_lock();
    s_game.recon_complete = true;
    s_game.display_dirty = true;
    state_unlock();

    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Operation-Code", "47484F5354");
    return httpd_resp_sendstr(request,
        "SECTOR 7 // RECOVERY IMAGE\n"
        "PAYLOAD: 47 48 4F 53 54\n"
        "FORMAT: HEX -> ASCII\n"
        "ACTION: return to the console and ARM the decoded word\n"
        "ACTUATORS: badge buttons are numbered 1-4, left to right\n");
}

static esp_err_t state_handler(httpd_req_t *request)
{
    trapdoor_snapshot_t snapshot;
    trapdoor_get_snapshot(&snapshot);

    char json[480];
    snprintf(
        json,
        sizeof(json),
        "{\"wifi\":%s,\"authorized\":%s,\"unlocked\":%s,"
        "\"ble\":%s,\"stage\":\"%s\",\"players\":%u,\"roles\":%u,"
        "\"progress\":%u,\"length\":%u,\"seconds\":%u,"
        "\"attempts\":%u,\"sync\":%u}",
        snapshot.wifi_running ? "true" : "false",
        snapshot.physical_authorized ? "true" : "false",
        snapshot.unlocked ? "true" : "false",
        snapshot.ble_active ? "true" : "false",
        trapdoor_stage_name(snapshot.stage),
        snapshot.connected_players,
        snapshot.role_count,
        snapshot.sequence_progress,
        snapshot.sequence_length,
        snapshot.seconds_remaining,
        snapshot.attempts,
        snapshot.sync_count);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, json);
}

static void reset_challenge(void)
{
    state_lock();
    const bool running = s_game.wifi_running;
    const unsigned players = s_game.connected_players;
    memset(&s_game, 0, sizeof(s_game));
    s_game.wifi_running = running;
    s_game.connected_players = players;
    s_game.stage = TRAPDOOR_STAGE_RECON;
    s_game.display_dirty = true;
    state_unlock();
    ghost_ble_set_active(false);
}

static int role_for_ip_locked(uint32_t client_ip)
{
    if (client_ip == 0) {
        return -1;
    }
    for (int index = 0; index < 2; ++index) {
        if (s_game.operator_ip[index] == client_ip) {
            return index;
        }
    }
    return -1;
}

static bool start_sync_if_ready_locked(void)
{
    if (s_game.stage != TRAPDOOR_STAGE_COOP_READY || role_count_locked() < 2) {
        return false;
    }
    s_game.stage = TRAPDOOR_STAGE_SYNC;
    s_game.sync_mask = 0;
    s_game.deadline_us = esp_timer_get_time() + (int64_t)SYNC_SECONDS * 1000000;
    s_game.last_display_seconds = SYNC_SECONDS;
    s_game.display_dirty = true;
    ESP_LOGI(TAG, "two-operator sync window opened for %d seconds", SYNC_SECONDS);
    return true;
}

static void arm_defusal(char *output, size_t output_size)
{
    char sequence_text[48] = {0};
    size_t offset = 0;

    state_lock();
    for (unsigned index = 0; index < DEFUSAL_SEQUENCE_LENGTH; ++index) {
        s_game.sequence[index] = (uint8_t)(esp_random() % 4U + 1U);
        offset += snprintf(
            sequence_text + offset,
            sizeof(sequence_text) - offset,
            "%s%u",
            index == 0 ? "" : "-",
            s_game.sequence[index]);
    }
    s_game.sequence_progress = 0;
    s_game.physical_authorized = false;
    s_game.stage = TRAPDOOR_STAGE_DEFUSAL;
    s_game.deadline_us = esp_timer_get_time() + (int64_t)DEFUSAL_SECONDS * 1000000;
    s_game.last_display_seconds = DEFUSAL_SECONDS;
    s_game.display_dirty = true;
    state_unlock();

    snprintf(output, output_size,
        "GHOST ARMED. FUSE: %d SECONDS\nDEFUSAL VECTOR: %s\n"
        "Press badge buttons 1-4 from LEFT to RIGHT. A wrong press resets progress.",
        DEFUSAL_SECONDS,
        sequence_text);
    ESP_LOGI(TAG, "fresh defusal vector %s", sequence_text);
}

static void role_command(uint32_t client_ip, char *output, size_t output_size)
{
    state_lock();
    int role = role_for_ip_locked(client_ip);
    if (role < 0 && client_ip != 0) {
        if (s_game.operator_ip[0] == 0) {
            role = 0;
        } else if (s_game.operator_ip[1] == 0) {
            role = 1;
        }
        if (role >= 0) {
            s_game.operator_ip[role] = client_ip;
            s_game.display_dirty = true;
        }
    }
    const unsigned count = role_count_locked();
    const bool sync_started = start_sync_if_ready_locked();
    state_unlock();

    if (role == 0) {
        snprintf(output, output_size,
            "IDENTITY: OPERATOR ALPHA\nYOUR FRAGMENT: AAPKO\n"
            "Keep it secret. Operators registered: %u/2%s",
            count,
            sync_started ? "\nSYNC WINDOW IS NOW LIVE." : "");
    } else if (role == 1) {
        snprintf(output, output_size,
            "IDENTITY: OPERATOR BRAVO\nYOUR FRAGMENT: MUJHSE\n"
            "Keep it secret. Operators registered: %u/2%s",
            count,
            sync_started ? "\nSYNC WINDOW IS NOW LIVE." : "");
    } else {
        snprintf(output, output_size,
            "ROLE DENIED: both operator slots are occupied by other devices.");
    }
}

static void sync_command(
    uint32_t client_ip,
    const char *fragment,
    char *output,
    size_t output_size)
{
    state_lock();
    const int role = role_for_ip_locked(client_ip);
    if (s_game.stage == TRAPDOOR_STAGE_COOP_READY && role_count_locked() == 2) {
        start_sync_if_ready_locked();
    }

    if (s_game.stage != TRAPDOOR_STAGE_SYNC) {
        if (s_game.stage < TRAPDOOR_STAGE_COOP_READY) {
            snprintf(output, output_size, "SYNC OFFLINE: solve the BLE ghost first.");
        } else if (role_count_locked() < 2) {
            snprintf(output, output_size,
                "SYNC NEEDS TWO PHONES. Each phone must run: role");
        } else {
            snprintf(output, output_size,
                "SYNC WINDOW CLOSED. Run: sync start");
        }
        state_unlock();
        return;
    }
    if (role < 0) {
        snprintf(output, output_size, "UNREGISTERED DEVICE. Run: role");
        state_unlock();
        return;
    }

    const char *expected = role == 0 ? "aapko" : "mujhse";
    if (strcmp(fragment, expected) != 0) {
        snprintf(output, output_size,
            "FRAGMENT REJECTED for OPERATOR %s.", role == 0 ? "ALPHA" : "BRAVO");
        state_unlock();
        return;
    }

    s_game.sync_mask |= (uint8_t)(1U << role);
    s_game.display_dirty = true;
    const unsigned count = bit_count(s_game.sync_mask);
    if (s_game.sync_mask == 0x03) {
        s_game.unlocked = true;
        s_game.stage = TRAPDOOR_STAGE_UNLOCKED;
        s_game.deadline_us = 0;
        snprintf(output, output_size,
            "DUAL AUTH ACCEPTED.\nFLAG: SHREY{AAPKO_MUJHSE_KAAM_HAI}\n"
            "The badge display has released the meme.");
        ESP_LOGI(TAG, "vault unlocked by two distinct operators");
    } else {
        snprintf(output, output_size,
            "FRAGMENT LOCKED: %u/2. Waiting for the other operator...", count);
    }
    state_unlock();
}

static void execute_command(
    const char *command,
    uint32_t client_ip,
    char *output,
    size_t output_size)
{
    trapdoor_tick();
    trapdoor_snapshot_t snapshot;
    trapdoor_get_snapshot(&snapshot);

    if (strcmp(command, "help") == 0) {
        snprintf(output, output_size,
            "COMMANDS\n  status   inspect mission state\n  role     claim a phone identity\n"
            "  logs     recover damaged records\n  scan     inspect badge interfaces\n"
            "  vault    inspect the sealed objective\n  sync ... two-operator link\n"
            "  reset    restart the operation\n  about    safety boundary");
    } else if (strcmp(command, "status") == 0) {
        snprintf(output, output_size,
            "NODE: SHREY-TRAPDOOR\nSTAGE: %s\nPLAYERS: %u WIFI / %u ROLES\n"
            "TIMER: %u\nBLE GHOST: %s\nVAULT: %s",
            trapdoor_stage_name(snapshot.stage),
            snapshot.connected_players,
            snapshot.role_count,
            snapshot.seconds_remaining,
            snapshot.ble_active ? "AWAKE" : "SLEEPING",
            snapshot.unlocked ? "OPEN" : "SEALED");
    } else if (strcmp(command, "role") == 0) {
        role_command(client_ip, output, output_size);
    } else if (strcmp(command, "logs") == 0) {
        snprintf(output, output_size,
            "[03:14:15] Recovery index damaged.\n"
            "[03:14:16] Sector pointer survived: 7\n"
            "[03:14:17] The browser source remembers what the console hides.");
    } else if (strcmp(command, "scan") == 0) {
        snprintf(output, output_size,
            "INTERFACES\n  WIFI  isolated captive node online\n"
            "  BLE   %s\n  GATT  custom service gated by physical auth\n"
            "  LCD   operator console online\n  INPUT four-button actuator online",
            snapshot.ble_active ? "SHREY_GHOST advertising" : "ghost radio sleeping");
    } else if (strcmp(command, "vault") == 0) {
        if (snapshot.unlocked) {
            snprintf(output, output_size,
                "VAULT OPEN\nFLAG: SHREY{AAPKO_MUJHSE_KAAM_HAI}\nMISSION COMPLETE.");
        } else {
            snprintf(output, output_size,
                "VAULT SEALED\nFour layers required: WEB / BUTTONS / BLE / DUAL AUTH\n"
                "CURRENT STAGE: %s", trapdoor_stage_name(snapshot.stage));
        }
    } else if (strcmp(command, "arm ghost") == 0) {
        state_lock();
        const bool recon_complete = s_game.recon_complete;
        const bool armable = s_game.stage == TRAPDOOR_STAGE_RECON ||
            s_game.stage == TRAPDOOR_STAGE_DEFUSAL;
        state_unlock();
        if (!recon_complete) {
            snprintf(output, output_size,
                "ARM REJECTED: missing Sector 7 recovery image.");
        } else if (!armable) {
            snprintf(output, output_size,
                "ARM REJECTED: mission already passed the physical layer.");
        } else {
            arm_defusal(output, output_size);
        }
    } else if (strncmp(command, "sync ", 5) == 0) {
        if (strcmp(command + 5, "start") == 0) {
            state_lock();
            const bool started = start_sync_if_ready_locked();
            const trapdoor_stage_t stage = s_game.stage;
            const unsigned roles = role_count_locked();
            state_unlock();
            if (started) {
                snprintf(output, output_size,
                    "SYNC WINDOW OPEN: %d SECONDS. Both operators submit now.", SYNC_SECONDS);
            } else if (stage < TRAPDOOR_STAGE_COOP_READY) {
                snprintf(output, output_size, "SYNC OFFLINE: solve the BLE ghost first.");
            } else if (roles < 2) {
                snprintf(output, output_size, "SYNC NEEDS TWO PHONES. Each phone must run: role");
            } else {
                snprintf(output, output_size, "SYNC ALREADY ACTIVE OR VAULT OPEN.");
            }
        } else {
            sync_command(client_ip, command + 5, output, output_size);
        }
    } else if (strncmp(command, "submit ", 7) == 0) {
        snprintf(output, output_size,
            "LEGACY BYPASS REMOVED. This vault requires two distinct phone identities.\n"
            "Run: role");
    } else if (strcmp(command, "reset") == 0) {
        reset_challenge();
        snprintf(output, output_size,
            "Operation reset. Roles, timers, BLE ghost, and vault state cleared.");
    } else if (strcmp(command, "about") == 0) {
        snprintf(output, output_size,
            "A self-contained badge CTF using only its own AP, buttons, display, and BLE service.\n"
            "No uplink, credential capture, deauthentication, jamming, or third-party impersonation.");
    } else {
        snprintf(output, output_size, "UNKNOWN COMMAND: %s\nType help.", command);
    }
}

static void json_escape(const char *input, char *output, size_t output_size)
{
    size_t write_index = 0;
    for (size_t read_index = 0; input[read_index] != '\0' && write_index + 2 < output_size;
         ++read_index) {
        const char character = input[read_index];
        if (character == '\n') {
            output[write_index++] = '\\';
            output[write_index++] = 'n';
        } else if (character == '\\' || character == '"') {
            output[write_index++] = '\\';
            output[write_index++] = character;
        } else if ((unsigned char)character >= 0x20) {
            output[write_index++] = character;
        }
    }
    output[write_index] = '\0';
}

static esp_err_t command_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len > 80) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "command too long");
    }

    char command[81];
    int received_total = 0;
    while (received_total < request->content_len) {
        const int received = httpd_req_recv(
            request,
            command + received_total,
            request->content_len - received_total);
        if (received <= 0) {
            return ESP_FAIL;
        }
        received_total += received;
    }
    command[received_total] = '\0';

    char *start = command;
    while (isspace((unsigned char)*start)) {
        ++start;
    }
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    for (char *cursor = start; *cursor != '\0'; ++cursor) {
        *cursor = (char)tolower((unsigned char)*cursor);
    }

    char output[900];
    execute_command(start, request_client_ip(request), output, sizeof(output));
    char escaped[1400];
    json_escape(output, escaped, sizeof(escaped));

    trapdoor_snapshot_t snapshot;
    trapdoor_get_snapshot(&snapshot);
    char response[1600];
    snprintf(
        response,
        sizeof(response),
        "{\"output\":\"%s\",\"unlocked\":%s,\"stage\":\"%s\"}",
        escaped,
        snapshot.unlocked ? "true" : "false",
        trapdoor_stage_name(snapshot.stage));
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, response);
}

static esp_err_t start_http_server(void)
{
    if (s_http_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t configuration = HTTPD_DEFAULT_CONFIG();
    configuration.max_uri_handlers = 7;
    configuration.uri_match_fn = httpd_uri_match_wildcard;
    configuration.stack_size = 7168;
    esp_err_t error = httpd_start(&s_http_server, &configuration);
    if (error != ESP_OK) {
        return error;
    }

    const httpd_uri_t state_uri = {
        .uri = "/api/state",
        .method = HTTP_GET,
        .handler = state_handler,
    };
    const httpd_uri_t command_uri = {
        .uri = "/api/cmd",
        .method = HTTP_POST,
        .handler = command_handler,
    };
    const httpd_uri_t sector_uri = {
        .uri = "/sector7",
        .method = HTTP_GET,
        .handler = sector_handler,
    };
    const httpd_uri_t portal_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = portal_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &state_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &command_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &sector_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &portal_uri));
    ESP_LOGI(TAG, "terminal ready at http://192.168.4.1/");
    return ESP_OK;
}

esp_err_t trapdoor_start(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_ERROR_CHECK(initialize_platform());
    ESP_ERROR_CHECK(ghost_ble_init());
    ESP_ERROR_CHECK(initialize_wifi());

    if (trapdoor_is_running()) {
        return ESP_OK;
    }

    reset_challenge();
    esp_err_t error = esp_wifi_start();
    if (error != ESP_OK) {
        return error;
    }
    ESP_ERROR_CHECK(configure_captive_dns_offer());

    state_lock();
    s_game.wifi_running = true;
    s_game.connected_players = 0;
    s_game.display_dirty = true;
    state_unlock();

    error = start_http_server();
    if (error != ESP_OK) {
        esp_wifi_stop();
        return error;
    }

    s_dns_should_run = true;
    if (xTaskCreate(dns_server_task, "trapdoor_dns", 3072, NULL, 4, &s_dns_task) != pdPASS) {
        s_dns_should_run = false;
        httpd_stop(s_http_server);
        s_http_server = NULL;
        esp_wifi_stop();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Operation KAAM ready: SSID=%s channel=%d", TRAPDOOR_SSID, TRAPDOOR_CHANNEL);
    return ESP_OK;
}

esp_err_t trapdoor_stop(void)
{
    if (!trapdoor_is_running()) {
        return ESP_OK;
    }

    ghost_ble_set_active(false);
    state_lock();
    s_game.wifi_running = false;
    s_game.connected_players = 0;
    s_game.display_dirty = true;
    state_unlock();

    if (s_http_server != NULL) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }

    s_dns_should_run = false;
    if (s_dns_socket >= 0) {
        shutdown(s_dns_socket, SHUT_RDWR);
    }
    for (int wait = 0; s_dns_task != NULL && wait < 20; ++wait) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    esp_err_t error = esp_wifi_stop();
    ESP_LOGI(TAG, "Wi-Fi and BLE challenge stopped; gallery mode active");
    return error;
}

bool trapdoor_is_running(void)
{
    if (s_lock == NULL) {
        return false;
    }
    state_lock();
    const bool running = s_game.wifi_running;
    state_unlock();
    return running;
}

void trapdoor_process_button(unsigned button_number)
{
    if (button_number < 1 || button_number > 4 || !trapdoor_is_running()) {
        return;
    }

    bool activate_ble = false;
    state_lock();
    if (s_game.stage == TRAPDOOR_STAGE_DEFUSAL) {
        if (esp_timer_get_time() >= s_game.deadline_us) {
            s_game.stage = TRAPDOOR_STAGE_RECON;
            s_game.sequence_progress = 0;
            ++s_game.attempts;
            ESP_LOGW(TAG, "defusal fuse expired");
        } else if (button_number == s_game.sequence[s_game.sequence_progress]) {
            ++s_game.sequence_progress;
            ESP_LOGI(TAG, "defusal progress %u/%d",
                     s_game.sequence_progress, DEFUSAL_SEQUENCE_LENGTH);
            if (s_game.sequence_progress == DEFUSAL_SEQUENCE_LENGTH) {
                s_game.physical_authorized = true;
                s_game.stage = TRAPDOOR_STAGE_BLE_GHOST;
                s_game.deadline_us = 0;
                activate_ble = true;
                ESP_LOGI(TAG, "physical defusal accepted; waking BLE ghost");
            }
        } else {
            ++s_game.attempts;
            s_game.sequence_progress =
                button_number == s_game.sequence[0] ? 1U : 0U;
            ESP_LOGW(TAG, "wrong actuator; defusal progress reset");
        }
        s_game.display_dirty = true;
    }
    state_unlock();

    if (activate_ble) {
        ghost_ble_set_active(true);
    }
}

void trapdoor_ble_solved(void)
{
    if (s_lock == NULL) {
        return;
    }
    state_lock();
    if (s_game.stage == TRAPDOOR_STAGE_BLE_GHOST) {
        s_game.stage = TRAPDOOR_STAGE_COOP_READY;
        s_game.display_dirty = true;
        start_sync_if_ready_locked();
        ESP_LOGI(TAG, "BLE puzzle solved; cooperative finale ready");
    }
    state_unlock();
}

void trapdoor_tick(void)
{
    if (s_lock == NULL) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    state_lock();
    if ((s_game.stage == TRAPDOOR_STAGE_DEFUSAL ||
         s_game.stage == TRAPDOOR_STAGE_SYNC) &&
        now_us >= s_game.deadline_us) {
        if (s_game.stage == TRAPDOOR_STAGE_DEFUSAL) {
            s_game.stage = TRAPDOOR_STAGE_RECON;
            s_game.sequence_progress = 0;
            s_game.physical_authorized = false;
            ++s_game.attempts;
            ESP_LOGW(TAG, "defusal expired; re-arm from web console");
        } else {
            s_game.stage = TRAPDOOR_STAGE_COOP_READY;
            s_game.sync_mask = 0;
            ESP_LOGW(TAG, "two-operator sync window expired");
        }
        s_game.deadline_us = 0;
        s_game.last_display_seconds = 0;
        s_game.display_dirty = true;
    } else {
        const unsigned seconds = seconds_remaining_locked(now_us);
        if (seconds != s_game.last_display_seconds) {
            s_game.last_display_seconds = seconds;
            s_game.display_dirty = true;
        }
    }
    state_unlock();
}

void trapdoor_get_snapshot(trapdoor_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (s_lock == NULL) {
        return;
    }

    state_lock();
    snapshot->wifi_running = s_game.wifi_running;
    snapshot->physical_authorized = s_game.physical_authorized;
    snapshot->unlocked = s_game.unlocked;
    snapshot->ble_active = ghost_ble_is_active();
    snapshot->connected_players = s_game.connected_players;
    snapshot->sequence_progress = s_game.sequence_progress;
    snapshot->sequence_length = DEFUSAL_SEQUENCE_LENGTH;
    snapshot->seconds_remaining = seconds_remaining_locked(esp_timer_get_time());
    snapshot->attempts = s_game.attempts;
    snapshot->role_count = role_count_locked();
    snapshot->sync_count = bit_count(s_game.sync_mask);
    snapshot->stage = s_game.stage;
    state_unlock();
}

bool trapdoor_take_display_dirty(void)
{
    if (s_lock == NULL) {
        return false;
    }
    state_lock();
    const bool dirty = s_game.display_dirty;
    s_game.display_dirty = false;
    state_unlock();
    return dirty;
}

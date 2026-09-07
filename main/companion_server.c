#include "companion_server.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mdns.h"

#include "audio_common.h"

#include "album_art.h"
#include "app_config.h"
#include "nowplaying.h"
#include "station_list.h"

static const char *TAG = "COMPANION";

/* Deep enough that a couple of quick /tune requests are never lost even if
 * radio_pipeline_wait() is briefly busy elsewhere - same reasoning as
 * avrcp_uart.c's/console_cli.c's command queues. Each entry carries its own
 * target index (see this file's header comment on why - unlike those two
 * files' bare command byte). */
#define COMPANION_CMD_QUEUE_LEN 4
#define COMPANION_TUNE_BODY_MAX 128
#define COMPANION_EVENT_REGISTER_RETRY_MS 200
/* ~100 attempts * 200ms = 20s - generous relative to how fast start_wifi()
 * actually creates the default event loop (a handful of milliseconds after
 * radio_task starts, itself created moments after this file's
 * companion_server_start() returns), but bounded rather than infinite so a
 * genuinely broken boot does not leave a task spinning forever. */
#define COMPANION_EVENT_REGISTER_MAX_ATTEMPTS 100

typedef struct {
    uint8_t cmd;    /* avrcp_cmd_t - always AVRCP_CMD_SELECT today, see companion_server.h */
    uint8_t index;  /* RADIO_STATIONS index this SELECT is for, already range-validated */
} companion_queue_item_t;

static bool s_started;                    /* queue/doorbell machinery ready */
static bool s_ip_setup_done;               /* mDNS + httpd brought up once, on the first IP */
static QueueHandle_t s_cmd_queue;
static audio_event_iface_handle_t s_evt;
static httpd_handle_t s_httpd;
/* The index paired with the most recently dequeued SELECT - see
 * companion_server_take_command()'s own comment. Touched only from that one
 * function (the writer) and companion_server_take_pending_station_index()
 * (the reader), which radio_pipeline_wait() always calls immediately after,
 * on the same task - no lock needed, same reasoning as station_list.c's
 * s_now_playing_idx. */
static volatile uint8_t s_last_dequeued_index;

/* ---- doorbell/queue - mirrors console_cli.c's dispatch_command() exactly,
 * except the queue item also carries the target station index (see this
 * file's header comment for why a single-slot handoff like console_cli.h's
 * CONSOLE_CMD_CAL_SET_MS is not safe here). ---- */
static void dispatch_select(uint8_t idx)
{
    companion_queue_item_t item = { .cmd = (uint8_t)AVRCP_CMD_SELECT, .index = idx };
    if (xQueueSend(s_cmd_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Command queue full (%d pending); dropping this /tune request", COMPANION_CMD_QUEUE_LEN);
        return;
    }
    if (s_evt) {
        /* Payload-free wake-up, same PERIPH source type ESP-ADF's own
         * examples and this project's other two doorbells use. */
        audio_event_iface_msg_t msg = {
            .source_type = AUDIO_ELEMENT_TYPE_PERIPH,
            .cmd = 0,
            .data = NULL,
            .data_len = 0,
            .source = NULL,
            .need_free_data = false,
        };
        audio_event_iface_sendout(s_evt, &msg);
    }
}

/* ---- HTTP handlers ---- */

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}

static esp_err_t handle_nowplaying(httpd_req_t *req)
{
    nowplaying_info_t info;
    nowplaying_get_current(&info);
    uint8_t idx = station_list_get_now_playing();

    /* Version only - immediately released, never held across the rest of
     * this JSON response. See album_art.h's doc comment on
     * album_art_get_snapshot(). */
    uint32_t art_version = 0;
    if (album_art_get_snapshot(NULL, NULL, &art_version)) {
        album_art_release_snapshot();
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, NULL, 0);
    }
    /* "tuning" is deliberately just !info.valid - see this file's header
     * comment on reusing nowplaying_reset()'s existing valid=false->true
     * transition instead of adding new event plumbing. */
    cJSON_AddBoolToObject(root, "valid", info.valid);
    cJSON_AddBoolToObject(root, "tuning", !info.valid);
    cJSON_AddStringToObject(root, "title", info.title);
    cJSON_AddStringToObject(root, "subtitle", info.subtitle);
    cJSON_AddStringToObject(root, "album", info.album);
    cJSON_AddNumberToObject(root, "art_version", art_version);
    cJSON_AddNumberToObject(root, "art_w", RADIO_COMPANION_ART_W);
    cJSON_AddNumberToObject(root, "art_h", RADIO_COMPANION_ART_H);
    cJSON_AddNumberToObject(root, "station_index", idx);
    cJSON_AddStringToObject(root, "station_name", idx < RADIO_STATION_COUNT ? RADIO_STATIONS[idx].name : "");
    cJSON_AddNumberToObject(root, "age_ms", info.age_ms);
    return send_json(req, root);
}

static esp_err_t handle_stations(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, NULL, 0);
    }
    cJSON_AddNumberToObject(root, "count", RADIO_STATION_COUNT);
    cJSON_AddNumberToObject(root, "current_index", station_list_get_now_playing());
    cJSON *arr = cJSON_AddArrayToObject(root, "stations");
    for (uint8_t i = 0; i < RADIO_STATION_COUNT; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", i);
        cJSON_AddStringToObject(item, "id", RADIO_STATIONS[i].id);
        cJSON_AddStringToObject(item, "name", RADIO_STATIONS[i].name);
        cJSON_AddItemToArray(arr, item);
    }
    return send_json(req, root);
}

static esp_err_t send_tune_error(httpd_req_t *req, const char *why)
{
    httpd_resp_set_status(req, "400 Bad Request");
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send(req, NULL, 0);
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", why);
    return send_json(req, root);
}

static esp_err_t handle_tune(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= COMPANION_TUNE_BODY_MAX) {
        return send_tune_error(req, "body missing or too large");
    }

    char buf[COMPANION_TUNE_BODY_MAX];
    int received = httpd_req_recv(req, buf, (size_t)req->content_len);
    if (received <= 0) {
        return send_tune_error(req, "could not read request body");
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    cJSON *index_item = root ? cJSON_GetObjectItemCaseSensitive(root, "index") : NULL;
    int idx = -1;
    if (cJSON_IsNumber(index_item)) {
        idx = index_item->valueint;
    }
    if (root) {
        cJSON_Delete(root);
    }

    if (idx < 0 || idx >= (int)RADIO_STATION_COUNT) {
        ESP_LOGW(TAG, "Rejecting /tune request with out-of-range or missing index (body: %s)", buf);
        return send_tune_error(req, "index missing or out of range");
    }

    ESP_LOGI(TAG, "Companion display requested station index %d", idx);
    dispatch_select((uint8_t)idx);

    cJSON *ok_root = cJSON_CreateObject();
    if (!ok_root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, NULL, 0);
    }
    cJSON_AddBoolToObject(ok_root, "ok", true);
    cJSON_AddBoolToObject(ok_root, "tuning", true);
    return send_json(req, ok_root);
}

static esp_err_t handle_art(httpd_req_t *req)
{
    const uint8_t *buf = NULL;
    size_t len = 0;
    uint32_t version = 0;
    if (!album_art_get_snapshot(&buf, &len, &version)) {
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }

    /* Stack-lifetime buffers are fine here - httpd_resp_set_hdr() does NOT
     * copy field/value (verified directly against esp_http_server's own
     * source, httpd_txrx.c: it just stores the pointers), but every one of
     * these stays in scope until httpd_resp_send() below actually builds
     * and writes the response, which happens before this function returns. */
    char ver_hdr[16];
    char w_hdr[8];
    char h_hdr[8];
    snprintf(ver_hdr, sizeof(ver_hdr), "%" PRIu32, version);
    snprintf(w_hdr, sizeof(w_hdr), "%d", RADIO_COMPANION_ART_W);
    snprintf(h_hdr, sizeof(h_hdr), "%d", RADIO_COMPANION_ART_H);

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "X-Art-Version", ver_hdr);
    httpd_resp_set_hdr(req, "X-Art-W", w_hdr);
    httpd_resp_set_hdr(req, "X-Art-H", h_hdr);
    esp_err_t err = httpd_resp_send(req, (const char *)buf, len);
    album_art_release_snapshot();
    return err;
}

static void register_routes(httpd_handle_t server)
{
    const httpd_uri_t nowplaying_uri = { .uri = "/nowplaying", .method = HTTP_GET, .handler = handle_nowplaying };
    const httpd_uri_t stations_uri   = { .uri = "/stations",   .method = HTTP_GET, .handler = handle_stations };
    const httpd_uri_t tune_uri       = { .uri = "/tune",       .method = HTTP_POST, .handler = handle_tune };
    const httpd_uri_t art_uri        = { .uri = "/art",        .method = HTTP_GET, .handler = handle_art };
    httpd_register_uri_handler(server, &nowplaying_uri);
    httpd_register_uri_handler(server, &stations_uri);
    httpd_register_uri_handler(server, &tune_uri);
    httpd_register_uri_handler(server, &art_uri);
}

/* ---- mDNS + httpd bring-up, both deferred to the first real IP - see
 * companion_server_start()'s doc comment (companion_server.h) for why. ---- */
static void bring_up_mdns_and_httpd(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed: %s (DP666 will need to be pointed at this chip's IP directly)",
                 esp_err_to_name(err));
    } else {
        mdns_hostname_set(RADIO_COMPANION_MDNS_HOSTNAME);
        mdns_instance_name_set("esp32_wifi_streamer companion API");
        err = mdns_service_add(NULL, "_http", "_tcp", RADIO_COMPANION_HTTP_PORT, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mdns_service_add failed: %s (mDNS hostname may not resolve)", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "mDNS advertising as '%s.local'", RADIO_COMPANION_MDNS_HOSTNAME);
        }
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = RADIO_COMPANION_HTTP_PORT;
    config.stack_size = RADIO_COMPANION_HTTPD_STACK;
    config.task_priority = RADIO_COMPANION_HTTPD_PRIORITY;
    /* This module's four routes are the only ones ever registered. */
    config.max_uri_handlers = 4;

    err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s (companion display API unavailable; audio playback unaffected)",
                 esp_err_to_name(err));
        return;
    }
    register_routes(s_httpd);
    ESP_LOGI(TAG, "Companion HTTP server listening on port %d", RADIO_COMPANION_HTTP_PORT);
}

static void companion_ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base != IP_EVENT || id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    if (s_ip_setup_done) {
        return; /* mDNS + httpd already brought up on an earlier connect - both survive a reconnect on their own */
    }
    s_ip_setup_done = true;
    bring_up_mdns_and_httpd();
}

/* Self-deleting retry task - see companion_server_start()'s doc comment
 * (companion_server.h) for exactly why this is needed instead of
 * registering the handler directly: the default event loop this needs does
 * not exist yet at the point companion_server_start() runs. */
static void wifi_event_register_task(void *arg)
{
    (void)arg;
    for (int attempt = 0; attempt < COMPANION_EVENT_REGISTER_MAX_ATTEMPTS; attempt++) {
        esp_err_t err = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, companion_ip_event_handler, NULL, NULL);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Registered for IP_EVENT_STA_GOT_IP; companion HTTP server will start once WiFi connects");
            vTaskDelete(NULL);
            return;
        }
        if (err != ESP_ERR_INVALID_STATE) {
            /* Some failure OTHER than "the default event loop doesn't exist
             * yet" - unexpected; retrying won't fix it, so stop rather than
             * spin forever. */
            ESP_LOGE(TAG, "Could not register companion IP event handler: %s "
                     "(companion HTTP server will never start this boot)", esp_err_to_name(err));
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(COMPANION_EVENT_REGISTER_RETRY_MS));
    }
    ESP_LOGW(TAG, "Gave up waiting for the default WiFi event loop after %d attempts; "
             "companion HTTP server will never start this boot",
             COMPANION_EVENT_REGISTER_MAX_ATTEMPTS);
    vTaskDelete(NULL);
}

esp_err_t companion_server_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_cmd_queue = xQueueCreate(COMPANION_CMD_QUEUE_LEN, sizeof(companion_queue_item_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "Could not create companion command queue");
        return ESP_ERR_NO_MEM;
    }

    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    s_evt = audio_event_iface_init(&evt_cfg);
    if (!s_evt) {
        ESP_LOGE(TAG, "Could not create companion doorbell event interface");
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_started = true;

    if (xTaskCreate(wifi_event_register_task, "companion_wifi", 3072, NULL,
                     tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create companion_wifi_wait task (companion HTTP server will never start)");
        /* Command queue/doorbell still work (harmlessly unused without the
         * HTTP server ever reaching them) - not worth unwinding s_started. */
        return ESP_ERR_NO_MEM;
    }

    /* Also starts album art decoding - a natural pairing (both exist only
     * to feed the companion display), and the same "log a warning, never
     * crash the radio" non-fatal convention. */
    esp_err_t art_err = album_art_start();
    if (art_err != ESP_OK) {
        ESP_LOGW(TAG, "album_art_start failed: %s (GET /art will keep returning 204)",
                 esp_err_to_name(art_err));
    }

    ESP_LOGI(TAG, "Companion server command/doorbell ready; mDNS + HTTP will start once WiFi connects");
    return ESP_OK;
}

audio_event_iface_handle_t companion_server_get_event_iface(void)
{
    return s_started ? s_evt : NULL;
}

avrcp_cmd_t companion_server_take_command(TickType_t wait)
{
    if (!s_started) {
        return AVRCP_CMD_NONE;
    }
    companion_queue_item_t item;
    if (xQueueReceive(s_cmd_queue, &item, wait) != pdTRUE) {
        return AVRCP_CMD_NONE;
    }
    s_last_dequeued_index = item.index;
    return (avrcp_cmd_t)item.cmd;
}

uint8_t companion_server_take_pending_station_index(void)
{
    return s_last_dequeued_index;
}

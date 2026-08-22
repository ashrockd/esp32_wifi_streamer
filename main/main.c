#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "radio_pipeline.h"
#include "tunein_control.h"

static const char *TAG = "MAIN";
static EventGroupHandle_t wifi_events;
static const int WIFI_CONNECTED_BIT = BIT0;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi station started; connecting to SSID '%s'", RADIO_WIFI_SSID);
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting in background");
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "Wi-Fi connected: IP=" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void start_wifi(void)
{
    wifi_events = xEventGroupCreate();
    configASSERT(wifi_events);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t cfg = {0};
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", RADIO_WIFI_SSID);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", RADIO_WIFI_PASSWORD);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void start_sntp(void)
{
    ESP_LOGI(TAG, "Starting SNTP for valid TuneIn listen IDs and TLS clock");
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

static bool wait_for_sntp(void)
{
    time_t now = 0;

    for (int retry = 0; retry < 20; retry++) {
        time(&now);

        if (now >= 1700000000) {
            ESP_LOGI(TAG, "SNTP time synchronized");
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGW(TAG, "SNTP synchronization timed out; continuing");
    return false;
}

static void radio_task(void *pvParameters)
{
    esp_err_t err;
    int consecutive_failures = 0;

    start_wifi();

    xEventGroupWaitBits(
        wifi_events,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY
    );

    start_sntp();
    wait_for_sntp();

    while (true) {
        ESP_LOGI(TAG, "Waiting for Wi-Fi connection");

        xEventGroupWaitBits(
            wifi_events,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdTRUE,
            portMAX_DELAY
        );

        tunein_session_t *session = calloc(1, sizeof(*session));
        if (!session) {
            ESP_LOGE(TAG, "Failed to allocate TuneIn session");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        ESP_LOGI(TAG, "Starting TuneIn control session");
        err = tunein_start_session(session);

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Starting radio pipeline");
            err = radio_pipeline_start(session);
        }

        free(session);

        if (err == ESP_OK) {
            /* audio_pipeline_run() is non-blocking - it only launches the
             * element tasks - so block here for as long as playback is
             * actually healthy, and only fall through when the pipeline
             * reports an error (e.g. the signed HLS URL expired) or the
             * defensive max session duration elapses. */
            ESP_LOGI(TAG, "Playback running; watching for pipeline errors or session expiry");
            err = radio_pipeline_wait(pdMS_TO_TICKS(RADIO_SESSION_MAX_MS));
        }

        radio_pipeline_stop();

        ESP_LOGI(
            TAG,
            "radio_task minimum free stack: %u bytes",
            (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t))
        );

        if (err == ESP_OK) {
            /* Clean stop (session refresh or explicit end) - go again right away. */
            consecutive_failures = 0;
            ESP_LOGI(TAG, "Refreshing TuneIn session");
            continue;
        }

        consecutive_failures++;
        uint32_t backoff_ms = RADIO_RETRY_BASE_MS;
        for (int i = 1; i < consecutive_failures && backoff_ms < RADIO_RETRY_MAX_MS; i++) {
            backoff_ms *= 2;
        }
        if (backoff_ms > RADIO_RETRY_MAX_MS) {
            backoff_ms = RADIO_RETRY_MAX_MS;
        }

        ESP_LOGE(
            TAG,
            "Playback session ended/failed: %s; retrying in %u ms (attempt %d)",
            esp_err_to_name(err), (unsigned)backoff_ms, consecutive_failures
        );

        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MAIN", ESP_LOG_DEBUG);
    esp_log_level_set("TUNEIN", ESP_LOG_DEBUG);
    esp_log_level_set("PIPELINE", ESP_LOG_DEBUG);
    esp_log_level_set("FMP4_BRIDGE", ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "ESP32 Wi-Fi/streamer chip booting (I2S -> esp32_bt_speaker)");
    ESP_LOGI(TAG, "Target station=%s", RADIO_TUNEIN_STATION_ID);
    ESP_LOGI(TAG, "Free heap at boot=%" PRIu32, esp_get_free_heap_size());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    BaseType_t result = xTaskCreate(
        radio_task,
        "radio_task",
        16384,
        NULL,
        5,
        NULL
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create radio task");
        abort();
    }
}

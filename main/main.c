#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "avrcp_uart.h"
#include "console_cli.h"
#include "led_viz.h"
#include "playlist_prefetch.h"
#include "radio_pipeline.h"
#include "station_list.h"
#include "tunein_control.h"

static const char *TAG = "MAIN";
static EventGroupHandle_t wifi_events;
static const int WIFI_CONNECTED_BIT = BIT0;

/* RESOURCE HEADROOM LOGGING - see RADIO_RESOURCE_LOG_INTERVAL_MS's comment in
 * app_config.h. Two independent reports:
 *   log_ram_usage()   - what actually moves at runtime; called once at boot
 *                        and then every RADIO_RESOURCE_LOG_INTERVAL_MS.
 *   log_flash_usage() - the partition table + NVS usage; static for a given
 *                        build, so logged once at boot only.
 * `context` is just a free-text tag (e.g. "boot", "periodic") so the log can
 * be grepped/compared across a run without needing timestamps lined up by
 * hand. */
static void log_ram_usage(const char *context)
{
    multi_heap_info_t info8 = {0};
    heap_caps_get_info(&info8, MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "RAM [%s]: free=%u largest_free_block=%u min_free_ever=%" PRIu32
             " allocated=%u free_blocks=%u alloc_blocks=%u total_blocks=%u",
             context,
             (unsigned)info8.total_free_bytes,
             (unsigned)info8.largest_free_block,
             esp_get_minimum_free_heap_size(),
             (unsigned)info8.total_allocated_bytes,
             (unsigned)info8.free_blocks,
             (unsigned)info8.allocated_blocks,
             (unsigned)(info8.free_blocks + info8.allocated_blocks) /* multi_heap_info_t has no total_blocks field */);

    /* Fragmentation is what has actually bitten this project before (see
     * app_config.h's RADIO_HTTP_BUFFER_BYTES history: "~49KB free was not
     * enough for even a 3KB allocation, because free heap is not the same
     * as contiguous heap") - total_free_bytes vs largest_free_block above is
     * the direct evidence of that; a growing gap between the two over
     * several periodic logs means fragmentation is building, independent of
     * whatever total_free_bytes says on its own. */
    size_t dma_free    = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t exec32_free  = heap_caps_get_free_size(MALLOC_CAP_32BIT);
    size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "RAM [%s]: DMA-capable free=%u, 32-bit-aligned free=%u, PSRAM free=%u%s",
             context, (unsigned)dma_free, (unsigned)exec32_free, (unsigned)spiram_free,
             spiram_free == 0 ? " (no PSRAM fitted/enabled)" : "");
}

static void log_flash_usage(void)
{
    uint32_t declared_size = 0, physical_size = 0;
    esp_flash_get_size(NULL, &declared_size);          /* what the image header/sdkconfig says */
    esp_flash_get_physical_size(NULL, &physical_size); /* what the chip actually reports */
    ESP_LOGI(TAG, "FLASH: declared size=%" PRIu32 " bytes (%.2f MB), physical chip size=%" PRIu32 " bytes (%.2f MB)%s",
             declared_size, declared_size / (1024.0 * 1024.0),
             physical_size, physical_size / (1024.0 * 1024.0),
             (physical_size > declared_size) ?
                 " - chip has MORE than sdkconfig declares; raising CONFIG_ESPTOOLPY_FLASHSIZE "
                 "would unlock the difference for bigger/new partitions" : "");

    uint32_t partitioned_bytes = 0;
    int partition_count = 0;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it != NULL) {
        const esp_partition_t *p = esp_partition_get(it);
        ESP_LOGI(TAG, "FLASH:   partition '%-12s' type=0x%02x subtype=0x%02x offset=0x%06" PRIx32
                 " size=%7" PRIu32 " bytes (%6.1f KB)",
                 p->label, (unsigned)p->type, (unsigned)p->subtype, p->address, p->size, p->size / 1024.0f);
        partitioned_bytes += p->size;
        partition_count++;
        it = esp_partition_next(it); /* invalidates the iterator just read above - do not release it separately */
    }
    esp_partition_iterator_release(it); /* it is NULL here; documented as safe to call regardless */

    uint32_t unpartitioned = (declared_size > partitioned_bytes) ? (declared_size - partitioned_bytes) : 0;
    ESP_LOGI(TAG, "FLASH: %d partitions totaling %" PRIu32 " bytes; unpartitioned/free flash=%" PRIu32
             " bytes (%.2f KB) - room for a bigger app partition or a new one",
             partition_count, partitioned_bytes, unpartitioned, unpartitioned / 1024.0f);

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        ESP_LOGI(TAG, "FLASH: running app partition '%s', size=%" PRIu32 " bytes (%.1f KB) - this is the ceiling "
                 "the compiled binary must fit under; actual binary-vs-partition usage is in idf.py build's own "
                 "summary output, not re-derived here", running->label, running->size, running->size / 1024.0f);
    }

    nvs_stats_t nvs_stats;
    if (nvs_get_stats(NULL, &nvs_stats) == ESP_OK) {
        ESP_LOGI(TAG, "FLASH:   NVS: used_entries=%d free_entries=%d available_entries=%d total_entries=%d namespace_count=%d",
                 (int)nvs_stats.used_entries, (int)nvs_stats.free_entries, (int)nvs_stats.available_entries,
                 (int)nvs_stats.total_entries, (int)nvs_stats.namespace_count);
    }
}

static void resource_log_timer_cb(void *arg)
{
    (void)arg;
    log_ram_usage("periodic");

    /* 2026-09-04, TEMPORARY DIAGNOSTIC (see sdkconfig.defaults' matching
     * comment on CONFIG_HEAP_POISONING_LIGHT) - two real hardware crashes
     * were both heap-corruption-surfaces-downstream signatures (a wild
     * pointer inside tlsf_free()'s coalescing, then a "block already marked
     * as free" assert), caught only once the corrupted region happened to be
     * touched again, tens of minutes and a full session away from wherever
     * the actual bad write occurred. This walks the TLSF free-list structure
     * itself on every periodic tick (~RADIO_RESOURCE_LOG_INTERVAL_MS) - independent
     * of and complementary to the poisoning canaries, which only get checked
     * when a SPECIFIC block is freed - so a structural problem is caught
     * within one tick of first existing, not just when something eventually
     * stumbles into it. print_errors=true dumps the corrupt block's details
     * directly to the log if this ever fails, which is the single most
     * useful piece of evidence for narrowing down which allocation is
     * actually at fault. Remove alongside the sdkconfig.defaults poisoning
     * setting once a fix is confirmed stable - a full heap walk every tick,
     * on every heap in the system, is not free, even if it is cheap next to
     * COMPREHENSIVE poisoning's per-operation cost. */
    if (!heap_caps_check_integrity_all(true)) {
        ESP_LOGE(TAG, "HEAP CORRUPTION DETECTED by the periodic integrity check above - "
                 "see the heap_caps_check_integrity_all() output just printed for exactly "
                 "which allocation/region is affected");
    }
}

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
    /* Which station to play, as an index into RADIO_STATIONS (station_list.h).
     * Restored from NVS so a power cycle resumes whatever the AVRCP next/
     * previous buttons last selected, rather than always snapping back to the
     * compile-time default. */
    uint8_t current_station_idx = station_list_load_index();

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

    /* One-time setup, same as start_wifi()/start_sntp() above - see
     * playlist_prefetch.h. Uses the same crt_bundle_attach the pipeline's
     * own http_reader uses (radio_pipeline.c), so a prefetch's cert
     * verification behaves identically to every other HTTPS request this
     * app makes. */
    esp_err_t prefetch_err = playlist_prefetch_init(esp_crt_bundle_attach);
    if (prefetch_err != ESP_OK) {
        ESP_LOGW(TAG, "playlist_prefetch_init failed: %s; live-window boundaries "
                 "will fall back to the reactive refresh only (no crash, just no gapless fix)",
                 esp_err_to_name(prefetch_err));
    }

    while (true) {
        ESP_LOGI(TAG, "Waiting for Wi-Fi connection");

        xEventGroupWaitBits(
            wifi_events,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdTRUE,
            portMAX_DELAY
        );

        const radio_station_t *station = &RADIO_STATIONS[current_station_idx];
        /* Published for console_cli.h's `status` command (station_list.h) -
         * covers the initial boot selection and every subsequent next/prev/
         * retry-with-same-station pass through this loop in one place. */
        station_list_set_now_playing(current_station_idx);
        tunein_session_t *session = calloc(1, sizeof(*session));
        if (!session) {
            ESP_LOGE(TAG, "Failed to allocate TuneIn session");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        ESP_LOGI(TAG, "Starting TuneIn control session for station: %s (%s)", station->name, station->id);
        err = tunein_start_session(session, station->id);

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Starting radio pipeline");
            err = radio_pipeline_start(session);
        }

        free(session);

        avrcp_cmd_t station_cmd = AVRCP_CMD_NONE;
        if (err == ESP_OK) {
            /* audio_pipeline_run() is non-blocking - it only launches the
             * element tasks - so block here for as long as playback is
             * actually healthy, and only fall through when the pipeline
             * reports an error (e.g. the signed HLS URL expired), the
             * defensive max session duration elapses, or an AVRCP next/
             * previous command arrives over UART (avrcp_uart.h). */
            ESP_LOGI(TAG, "Playback running; watching for pipeline errors, session expiry, or AVRCP next/previous");
            err = radio_pipeline_wait(pdMS_TO_TICKS(RADIO_SESSION_MAX_MS), &station_cmd);
        }

        radio_pipeline_stop();

        ESP_LOGI(
            TAG,
            "radio_task minimum free stack: %u bytes",
            (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t))
        );

        /* Checked before the error/backoff handling below: a station change
         * is a deliberate, healthy end to the session, so it must clear the
         * failure counter rather than accumulate one. station_list_next/prev
         * persist the new index to NVS themselves. */
        if (station_cmd == AVRCP_CMD_NEXT) {
            current_station_idx = station_list_next(current_station_idx);
            consecutive_failures = 0;
            continue;
        }
        if (station_cmd == AVRCP_CMD_PREV) {
            current_station_idx = station_list_prev(current_station_idx);
            consecutive_failures = 0;
            continue;
        }

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
    esp_log_level_set("PLAYLIST_PREFETCH", ESP_LOG_DEBUG);
    esp_log_level_set("STATIONS", ESP_LOG_DEBUG);
    esp_log_level_set("AVRCP_UART", ESP_LOG_DEBUG);
    esp_log_level_set("LED_VIZ", ESP_LOG_DEBUG);
    esp_log_level_set("CONSOLE", ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "ESP32 Wi-Fi/streamer chip booting (I2S -> esp32_bt_speaker)");
    ESP_LOGI(TAG, "Compiled-in default station=%s (%u stations available; the one actually started is "
             "restored from NVS - see station_list.h)",
             RADIO_TUNEIN_STATION_ID, (unsigned)RADIO_STATION_COUNT);
    ESP_LOGI(TAG, "Free heap at boot=%" PRIu32, esp_get_free_heap_size());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* On-board RGB LED visualiser (led_viz.h). Started before Wi-Fi so its
     * six-second boot colour cycle - which is also a power-on self test of
     * the LED and its data line - runs while the radio is still associating,
     * rather than adding six seconds to boot. Decorative, so a failure here
     * is logged and ignored; the radio does not depend on it. */
    err = led_viz_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED visualiser unavailable: %s (audio is unaffected)", esp_err_to_name(err));
    }

    /* AVRCP next/previous-station receiver (avrcp_uart.h) - started here,
     * independent of Wi-Fi, so the UART driver is already installed and
     * buffering by the time radio_task first reaches radio_pipeline_wait(),
     * rather than dropping presses made while the chip is still connecting.
     * A failure here is logged loudly but is deliberately not fatal: the
     * radio still plays its current station, it just cannot be switched. */
    err = avrcp_uart_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start AVRCP UART receiver: %s (next/previous station switching will not work)",
                 esp_err_to_name(err));
    }

    /* Serial console (console_cli.h) - a second, independent way to drive
     * next/prev and to read/adjust the LED threshold, from a laptop over
     * the same USB/serial link already used for flashing and logging.
     * Started here for the same reason as avrcp_uart_start() above:
     * independent of Wi-Fi, so it is ready before radio_task first reaches
     * radio_pipeline_wait() rather than missing early input. Not fatal on
     * failure - the radio still plays, it just cannot be driven from the
     * console. */
    err = console_cli_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start serial console: %s (console control will not work; "
                 "AVRCP next/previous and the LED default threshold are unaffected)",
                 esp_err_to_name(err));
    }

    /* RESOURCE HEADROOM LOGGING - see RADIO_RESOURCE_LOG_INTERVAL_MS's
     * comment in app_config.h. Flash/partition layout is static, so it's
     * only worth logging once, right here at boot; RAM is logged once now
     * too, then periodically once the radio task/pipeline are actually
     * running and allocating, via a periodic esp_timer below. */
    log_flash_usage();
    log_ram_usage("boot");

    esp_timer_handle_t resource_log_timer = NULL;
    const esp_timer_create_args_t resource_log_timer_args = {
        .callback = resource_log_timer_cb,
        .name = "res_log",
    };
    err = esp_timer_create(&resource_log_timer_args, &resource_log_timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(resource_log_timer, RADIO_RESOURCE_LOG_INTERVAL_MS * 1000ULL);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not start periodic RAM usage logging: %s (boot-time report above still stands)",
                 esp_err_to_name(err));
    }

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

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
#include "icy_meta.h"
#include "latency_cal.h"
#include "led_viz.h"
#include "nowplaying.h"
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
static size_t log_ram_usage(const char *context)
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
    return dma_free;
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

/* PLAYBACK STATUS LOGGING - piggybacks on the same periodic tick as the RAM
 * report above (RADIO_RESOURCE_LOG_INTERVAL_MS) so a run's serial log has
 * one place to find "what was playing and how healthy" lined up against
 * "what RAM looked like" at the same moments, without needing timestamps
 * cross-referenced by hand. Station and LED state are cheap, current-value
 * reads (station_list.h/led_viz.h); the measured bit rate is the one field
 * that has to be derived here rather than just read, since nothing on this
 * pipeline's path carries real bitrate metadata (see radio_pipeline.h's
 * radio_pipeline_get_stream_stats() comment) - it's computed from the delta
 * in http_reader's own byte counter across two ticks of this exact timer.
 * Track title/artist/album/artwork (nowplaying.h) is likewise a cheap,
 * non-blocking read - of whatever fmp4_bridge.c last sniffed straight out
 * of the CMAF stream it already parses for playback, no separate fetch of
 * its own - see nowplaying.h's own file comment for the whole mechanism. */
static void log_playback_status(void)
{
    uint8_t idx = station_list_get_now_playing();
    const char *station_name = (idx < RADIO_STATION_COUNT) ? RADIO_STATIONS[idx].name : "none yet";
    const char *station_id   = (idx < RADIO_STATION_COUNT) ? RADIO_STATIONS[idx].id : "-";

    radio_pipeline_stream_stats_t stats;
    radio_pipeline_get_stream_stats(&stats);

    ESP_LOGI(TAG, "STATUS: station='%s' (%s) [%u/%u] | stream=%s, %d Hz, %d ch",
             station_name, station_id,
             (unsigned)(idx < RADIO_STATION_COUNT ? idx + 1 : 0), (unsigned)RADIO_STATION_COUNT,
             stats.format_name, stats.sample_rate_hz, stats.channels);
    ESP_LOGI(TAG, "STATUS: LED latency comp=%" PRId32 "ms, peak threshold=%.1fdBFS",
             led_viz_get_latency_ms(), led_viz_get_threshold_db());

    /* Measured HTTP throughput, not a nominal per-station figure - see
     * radio_pipeline.h's radio_pipeline_stream_stats_t comment for why
     * that's the only bit-rate figure available on this pipeline's path at
     * all. Static state persists across calls (this function only ever runs
     * off resource_log_timer_cb's one periodic esp_timer), so each reading
     * only has to diff against the previous one. A byte count that has gone
     * DOWN since last time (a full pipeline rebuild handed out a fresh
     * http_reader - see radio_pipeline.h) or an inactive pipeline invalidates
     * the delta rather than producing a bogus negative/inflated rate; the
     * very first reading has nothing to diff against yet either, so all
     * three cases are treated the same way: skip this tick, just remember
     * where things stand for the next one. */
    static int64_t prev_bytes = -1;
    static int64_t prev_time_us;
    int64_t now_us = esp_timer_get_time();
    if (stats.active && prev_bytes >= 0 && stats.http_bytes_total >= prev_bytes && now_us > prev_time_us) {
        int64_t delta_bytes = stats.http_bytes_total - prev_bytes;
        int64_t elapsed_us  = now_us - prev_time_us;
        float kbps = ((float)delta_bytes * 8.0f) / ((float)elapsed_us / 1000000.0f) / 1000.0f;
        ESP_LOGI(TAG, "STATUS: HTTP throughput=%.1f kbps (measured over last %" PRId64 "ms; "
                 "actual network bytes incl. HLS/CMAF container overhead, not a nominal codec rate)",
                 kbps, elapsed_us / 1000);
    } else {
        ESP_LOGI(TAG, "STATUS: HTTP throughput=n/a (no prior sample yet, or pipeline just (re)started)");
    }
    prev_bytes = stats.active ? stats.http_bytes_total : -1;
    prev_time_us = now_us;

    /* Currently playing track - see nowplaying.h. Non-blocking: this always
     * returns immediately with whatever the last completed background poll
     * produced, never the result of a fetch happening right now. */
    nowplaying_info_t track;
    nowplaying_get_current(&track);
    if (track.valid) {
        ESP_LOGI(TAG, "STATUS: now playing '%s' - '%s' (album '%s') [as of %" PRIu32 "ms ago]",
                 track.title, track.subtitle, track.album, track.age_ms);
        ESP_LOGI(TAG, "STATUS: album art = %s",
                 track.art_url[0] ? track.art_url : "n/a (no artwork in this tag)");
    } else {
        ESP_LOGI(TAG, "STATUS: now playing = n/a (no track metadata received yet for this station)");
    }
}

static void resource_log_timer_cb(void *arg)
{
    (void)arg;
    log_playback_status();
    size_t dma_free = log_ram_usage("periodic");

    /* Self-healing reboot on sustained critical DMA-capable/internal RAM
     * pressure - see RADIO_DMA_FREE_CRITICAL_BYTES's comment in
     * app_config.h for the hardware evidence and full reasoning. Streak-
     * based (not a reboot on the first low reading) so a real, brief,
     * expected dip - e.g. the main stream and an opportunistic playlist
     * prefetch both holding a TLS connection open at once - gets a chance
     * to clear on its own first. */
    static int s_dma_low_streak;
    if (dma_free < RADIO_DMA_FREE_CRITICAL_BYTES) {
        s_dma_low_streak++;
        ESP_LOGW(TAG, "DMA-capable free=%u bytes is critically low (< %d) - streak %d/%d "
                 "before a self-healing reboot",
                 (unsigned)dma_free, RADIO_DMA_FREE_CRITICAL_BYTES,
                 s_dma_low_streak, RADIO_DMA_FREE_CRITICAL_STREAK);
        if (s_dma_low_streak >= RADIO_DMA_FREE_CRITICAL_STREAK) {
            ESP_LOGE(TAG, "DMA-capable RAM critically low for %d consecutive checks (~%" PRIu32
                     "s) with no recovery - rebooting to reclaim it before TLS/Wi-Fi fail outright",
                     s_dma_low_streak,
                     (uint32_t)(s_dma_low_streak * (RADIO_RESOURCE_LOG_INTERVAL_MS / 1000)));
            esp_restart();
        }
    } else {
        s_dma_low_streak = 0;
    }

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

    /* Tracks whether the loop below is about to (re)start THE SAME station
     * or a genuinely different one, so nowplaying_reset() below only fires
     * on a real switch - see nowplaying.h's own comment on why a routine
     * same-station session refresh must NOT clear the currently-displayed
     * track. Seeded to RADIO_STATION_COUNT - an impossible real index
     * (every real one is < that) - so the very first loop iteration always
     * counts as "changed" too, which is harmless (nothing to reset yet). */
    uint8_t previous_station_idx = (uint8_t)RADIO_STATION_COUNT;

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
        if (current_station_idx != previous_station_idx) {
            /* A GENUINE station change (or the very first loop pass) - see
             * nowplaying.h's own comment for why this must not fire on a
             * routine same-station session refresh: fmp4_bridge.c keeps
             * sniffing the SAME station's tags across one either way, so
             * the last known track is still correct and clearing it would
             * just flicker the status log for no reason. */
            nowplaying_reset();
            previous_station_idx = current_station_idx;
        }
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
        bool calibrate_requested = false;
        if (err == ESP_OK) {
            /* audio_pipeline_run() is non-blocking - it only launches the
             * element tasks - so block here for as long as playback is
             * actually healthy, and only fall through when the pipeline
             * reports an error (e.g. the signed HLS URL expired), the
             * defensive max session duration elapses, an AVRCP/console next/
             * previous command arrives (avrcp_uart.h, console_cli.h), or a
             * console `cal` command requests latency calibration
             * (latency_cal.h). */
            ESP_LOGI(TAG, "Playback running; watching for pipeline errors, session expiry, next/previous, or calibration");
            err = radio_pipeline_wait(pdMS_TO_TICKS(RADIO_SESSION_MAX_MS), &station_cmd, &calibrate_requested);
        }

        /* 2026-09-04: NOT called unconditionally here any more. It used to
         * be - tearing the whole pipeline down (including decoder_el) the
         * instant radio_pipeline_wait() returned, for EVERY reason it could
         * return (station change, clean refresh, real failure alike) -
         * which defeated radio_pipeline_start()'s own in-place-reuse logic
         * before it ever got a chance to run: by the time the next loop
         * iteration called it, `pipeline` was already NULL, so it always
         * took the full-rebuild path (see radio_pipeline_start()'s comment
         * and [[esp32-wifi-streamer-aac-heap-crash]]). Only calibration
         * needs the pipeline gone before the next step, because
         * latency_cal_run() builds its own separate I2S pipeline right
         * after this and the two cannot coexist on the same I2S peripheral.
         * Every other path below loops back around to tunein_start_session()
         * + radio_pipeline_start() for the next/retried station, which now
         * decides for itself whether the existing pipeline (still alive,
         * still playing the OLD station in the background while that
         * resolve runs) can be reused in place or needs a full rebuild -
         * and falls back to calling radio_pipeline_stop() itself if not. */
        if (calibrate_requested) {
            radio_pipeline_stop();
        }

        ESP_LOGI(
            TAG,
            "radio_task minimum free stack: %u bytes",
            (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t))
        );

        /* Checked first, before even the station-change handling below: like
         * a station change, entering calibration is a deliberate, healthy
         * end to the session, not a failure. latency_cal_run() owns the
         * entire interactive flow (see latency_cal.h) and blocks this task
         * until it's done (accepted or cancelled) - current_station_idx is
         * untouched, so falling through to `continue` resumes the exact same
         * station afterward, indistinguishable from an ordinary refresh. */
        if (calibrate_requested) {
            latency_cal_run();
            consecutive_failures = 0;
            continue;
        }

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

        /* Auto-advance through the station list on a real failure, not just
         * the same one again - see RADIO_STATION_FAILOVER_DELAY_MS's comment
         * in app_config.h. A bad/expired TuneIn entry or a dead stream URL
         * should not stall playback behind minutes of backoff on that one
         * station when the other RADIO_STATION_COUNT-1 stations are one hop
         * away and probably fine. Only once every station has had a turn in
         * this same failure streak without one working does this fall back
         * to the pre-existing same-station exponential backoff below - at
         * that point the problem is almost certainly broader than any one
         * station (Wi-Fi, TuneIn itself, DNS), and cycling stations faster
         * just means hammering a broken network harder, not finding a
         * working one sooner. */
        if (consecutive_failures <= (int)RADIO_STATION_COUNT) {
            current_station_idx = station_list_next(current_station_idx);
            ESP_LOGW(
                TAG,
                "Playback session ended/failed: %s; trying next station '%s' in %u ms "
                "(failure %d of %u before falling back to same-station backoff)",
                esp_err_to_name(err), RADIO_STATIONS[current_station_idx].name,
                (unsigned)RADIO_STATION_FAILOVER_DELAY_MS, consecutive_failures,
                (unsigned)RADIO_STATION_COUNT
            );
            vTaskDelay(pdMS_TO_TICKS(RADIO_STATION_FAILOVER_DELAY_MS));
            continue;
        }

        /* consecutive_failures has already counted RADIO_STATION_COUNT
         * failover hops by this point - offset it so the backoff below still
         * starts at RADIO_RETRY_BASE_MS on the first same-station retry
         * instead of an already-huge exponent inherited from the hops. */
        int backoff_attempt = consecutive_failures - (int)RADIO_STATION_COUNT;
        uint32_t backoff_ms = RADIO_RETRY_BASE_MS;
        for (int i = 1; i < backoff_attempt && backoff_ms < RADIO_RETRY_MAX_MS; i++) {
            backoff_ms *= 2;
        }
        if (backoff_ms > RADIO_RETRY_MAX_MS) {
            backoff_ms = RADIO_RETRY_MAX_MS;
        }

        ESP_LOGE(
            TAG,
            "Every station failed (%d consecutive failures) - likely a broader problem "
            "(Wi-Fi/TuneIn/DNS), not one bad station; retrying '%s' in %u ms (attempt %d)",
            consecutive_failures, RADIO_STATIONS[current_station_idx].name,
            (unsigned)backoff_ms, backoff_attempt
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
    esp_log_level_set("LATENCY_CAL", ESP_LOG_DEBUG);
    esp_log_level_set("NOWPLAYING", ESP_LOG_DEBUG);

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

    /* Now-playing (title/artist/album/artwork) result cache (nowplaying.h) -
     * started here for the same "independent of Wi-Fi, ready before it's
     * needed" reason as led_viz_start() above. Just creates a mutex - no
     * network I/O of its own any more (see nowplaying.h's header comment):
     * fmp4_bridge.c feeds it directly from the CMAF stream it already
     * parses for playback, once radio_task/the pipeline are up. Not fatal
     * on failure: the periodic status log just keeps reporting no track
     * data (nowplaying_get_current() degrades to out->valid == false when
     * nowplaying_init() was never called), audio playback itself is
     * completely unaffected either way. */
    err = nowplaying_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nowplaying_init failed: %s; the status log will not show track info",
                 esp_err_to_name(err));
    }

    /* ICY inline metadata scratch (icy_meta.h) - The Vibe of Vegas's own
     * now-playing source, a separate mechanism from nowplaying_init() above
     * (that one is fed from the CMAF stations' ID3-in-emsg tags). Same
     * "allocate once, up front" reasoning, and equally non-fatal on failure:
     * radio_pipeline.c's tap still strips the metadata bytes correctly
     * either way (see icy_meta.c), it just has nowhere to stash the parsed
     * title/artist. */
    err = icy_meta_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "icy_meta_init failed: %s; The Vibe of Vegas will still play, "
                 "just without a title/artist display", esp_err_to_name(err));
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

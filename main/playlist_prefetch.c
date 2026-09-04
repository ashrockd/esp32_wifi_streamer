#include "playlist_prefetch.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "PLAYLIST_PREFETCH";

/* Real bodies observed on this stream run 5.5-6.2KB; this leaves headroom
 * without inviting an unbounded read.
 *
 * 2026-08-22: this used to be preallocated ONCE in playlist_prefetch_init()
 * and kept for the app's lifetime, reasoning by analogy with fmp4_bridge's
 * own preallocated buffers ("don't malloc/free a mid-size block right when
 * the heap is already under pressure"). That analogy doesn't actually hold
 * for this buffer: fmp4_bridge allocates on every ~16s fragment (high
 * frequency, and right in the middle of active decode), while this is
 * needed at most twice per ~150s window (see radio_pipeline.c's
 * PLAYLIST_PREFETCH_MAX_ATTEMPTS_PER_WINDOW) and NOT during the pipeline's
 * own critical startup window. Worse, making it permanent put its ~8KB on
 * the scale at exactly the moment that turned out to be the tightest one on
 * this chip - pipeline start (esp_aac_dec_open() + the first segment's TLS
 * handshake) - and a hardware test confirmed it: RSA-verify OOM crash on
 * the very first segment open, before this module's task had even run once
 * (see app_config.h's RADIO_HTTP_BUFFER_BYTES history for the full story).
 * Now allocated fresh inside playlist_prefetch_task() and freed before it
 * exits - zero permanent cost, at the price of one more malloc/free per
 * attempt (which was never the hot path this project has fought heap churn
 * on - that's fmp4_bridge's per-fragment buffers, still preallocated). */
#define PREFETCH_BODY_BUF_BYTES 8192

/* 2026-08-22, MEASURED: a hardware log's high-water-mark line (logged just
 * before this task deletes itself) showed 2732-3100 bytes actually used out
 * of the 5120 originally guessed here. Cut to 3584 (~500 bytes margin over
 * the observed peak) - on a heap this tight, the ~1.5KB this frees per
 * prefetch attempt is worth having back, and this task does noticeably less
 * than ADF's own 6144-byte http_stream element task (HTTP_STREAM_TASK_STACK)
 * it was originally sized against - no ADF audio_element plumbing on this
 * one's stack. Revisit only if a fresh high-water-mark log ever gets close
 * to this value. */
#define PREFETCH_TASK_STACK_BYTES 3584
#define PREFETCH_TASK_PRIORITY    3
#define PREFETCH_TASK_TIMEOUT_MS  8000

static esp_err_t (*s_crt_bundle_attach)(void *conf) = NULL;
static QueueHandle_t s_result_queue = NULL;
static volatile bool s_in_flight = false;

esp_err_t playlist_prefetch_init(esp_err_t (*crt_bundle_attach)(void *conf))
{
    if (s_result_queue != NULL) {
        return ESP_OK; /* already initialized */
    }

    /* Depth 1: only one prefetch is ever in flight at a time (see
     * s_in_flight), so nothing can produce a second result before the first
     * is drained. The read buffer itself is no longer allocated here - see
     * PREFETCH_BODY_BUF_BYTES's comment. */
    s_result_queue = xQueueCreate(1, sizeof(playlist_prefetch_result_t *));
    if (!s_result_queue) {
        ESP_LOGE(TAG, "OOM creating prefetch result queue");
        return ESP_ERR_NO_MEM;
    }

    s_crt_bundle_attach = crt_bundle_attach;
    ESP_LOGI(TAG, "Initialized (queue only - read buffer is allocated per-attempt now), free heap=%" PRIu32
             ", largest block=%u",
             esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return ESP_OK;
}

/* Minimal, hand-rolled m3u8 media-playlist scan - deliberately not reusing
 * ESP-ADF's own hls_playlist_* parser (esp-adf/components/audio_stream/lib/
 * hls/include/hls_playlist.h): that header is a PRIVATE include of the
 * audio_stream component, and exposing it publicly would be one more shared-
 * file surgery on top of the http_stream.c/.h LOCAL PATCH this feature
 * already needs. Same technique this project already uses in
 * tools/tunein_inspect.py's parse_variant() and testing-tools/python-script-
 * albumart-title/tunein_nowplaying.py's parse_m3u8_segments(): the URI on
 * the line right after every #EXTINF tag is a media segment. Relative URIs
 * are handed through as-is - http_playlist_insert() (called via
 * http_stream_playlist_insert_tracks()) already resolves those against the
 * live element's own playlist->host_uri, so this function does not need a
 * URL-join of its own. */
static int parse_media_playlist(char *text, char **out_uris, int max_uris)
{
    int count = 0;
    char *saveptr = NULL;
    char *line = strtok_r(text, "\r\n", &saveptr);
    bool want_uri = false;

    while (line != NULL && count < max_uris) {
        /* strtok_r already trims the line terminator; only leading/trailing
         * whitespace within a line could remain, which none of these tags
         * or URIs are expected to carry from a well-formed CDN response. */
        if (want_uri) {
            if (line[0] != '\0' && line[0] != '#') {
                char *copy = strdup(line);
                if (copy) {
                    out_uris[count++] = copy;
                }
            }
            want_uri = false;
        } else if (strncmp(line, "#EXTINF", 7) == 0) {
            want_uri = true;
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }
    return count;
}

static void playlist_prefetch_task(void *arg)
{
    char *url = (char *)arg;
    playlist_prefetch_result_t *result = calloc(1, sizeof(*result));
    if (!result) {
        ESP_LOGE(TAG, "OOM allocating result struct; abandoning this prefetch");
        free(url);
        s_in_flight = false;
        vTaskDelete(NULL);
        return;
    }

    /* Allocated fresh per attempt, freed before this task exits - see
     * PREFETCH_BODY_BUF_BYTES's comment for why this isn't preallocated. */
    uint8_t *body_buf = heap_caps_malloc(PREFETCH_BODY_BUF_BYTES, MALLOC_CAP_8BIT);
    if (!body_buf) {
        ESP_LOGW(TAG, "OOM allocating %d-byte read buffer for this attempt (free heap=%" PRIu32
                 "); falling back to the reactive refresh at the boundary",
                 PREFETCH_BODY_BUF_BYTES, esp_get_free_heap_size());
        if (xQueueSend(s_result_queue, &result, 0) != pdTRUE) {
            free(result);
        }
        free(url);
        s_in_flight = false;
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = PREFETCH_TASK_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        /* One request, torn down immediately after - same reasoning as
         * tunein_control.c's http_get(): keep-alive would just hold a
         * second socket/TLS session open for no reason once this is done. */
        .keep_alive_enable = false,
        .crt_bundle_attach = s_crt_bundle_attach,
    };

    esp_err_t err = ESP_FAIL;
    int status = 0;
    int total_read = 0;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed (OOM?); free heap=%" PRIu32,
                 esp_get_free_heap_size());
        goto done;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Prefetch open failed: %s; falling back to the reactive refresh at the boundary",
                 esp_err_to_name(err));
        goto cleanup_client;
    }

    esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "Prefetch got HTTP %d; falling back to the reactive refresh at the boundary", status);
        err = ESP_FAIL;
        goto cleanup_client;
    }

    while (total_read < PREFETCH_BODY_BUF_BYTES - 1) {
        int r = esp_http_client_read(client, (char *)body_buf + total_read,
                                      PREFETCH_BODY_BUF_BYTES - 1 - total_read);
        if (r <= 0) {
            break;
        }
        total_read += r;
    }
    body_buf[total_read] = '\0';
    err = ESP_OK;

cleanup_client:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

done:
    if (err == ESP_OK && total_read > 0) {
        result->count = parse_media_playlist((char *)body_buf, result->uris, PLAYLIST_PREFETCH_MAX_TRACKS);
        ESP_LOGI(TAG, "Prefetch OK: %d bytes, %d segment URI(s) parsed ahead of the boundary",
                 total_read, result->count);
    } else {
        ESP_LOGW(TAG, "Prefetch produced nothing usable; the existing reactive refresh will handle this boundary");
    }

    if (xQueueSend(s_result_queue, &result, 0) != pdTRUE) {
        /* Queue depth is 1 and s_in_flight should have prevented a second
         * concurrent send - this would only happen if a previous result was
         * never polled. Don't leak it. */
        ESP_LOGW(TAG, "Result queue full; discarding this prefetch's result");
        for (int i = 0; i < result->count; i++) {
            free(result->uris[i]);
        }
        free(result);
    }

    ESP_LOGD(TAG, "Prefetch task stack high-water mark: %u bytes",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));

    heap_caps_free(body_buf);
    free(url);
    s_in_flight = false;
    vTaskDelete(NULL);
}

esp_err_t playlist_prefetch_start(const char *playlist_url)
{
    if (!s_result_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_in_flight) {
        return ESP_ERR_INVALID_STATE;
    }
    char *url_copy = strdup(playlist_url);
    if (!url_copy) {
        return ESP_ERR_NO_MEM;
    }

    s_in_flight = true; /* set before create: avoids a second trigger landing between here and the task's own first line */
    /* Stack depth is in bytes on ESP-IDF's FreeRTOS port (StackType_t is
     * uint8_t here) - same convention main.c's own xTaskCreate(radio_task,
     * ..., 16384, ...) call already relies on, so passed straight through
     * with no unit conversion. */
    BaseType_t created = xTaskCreate(playlist_prefetch_task, "plist_pf",
                                      PREFETCH_TASK_STACK_BYTES,
                                      url_copy, PREFETCH_TASK_PRIORITY, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed (free heap=%" PRIu32 ")", esp_get_free_heap_size());
        free(url_copy);
        s_in_flight = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool playlist_prefetch_is_in_flight(void)
{
    return s_in_flight;
}

bool playlist_prefetch_poll(playlist_prefetch_result_t **out_result)
{
    if (!s_result_queue || !out_result) {
        return false;
    }
    playlist_prefetch_result_t *result = NULL;
    if (xQueueReceive(s_result_queue, &result, 0) == pdTRUE) {
        *out_result = result;
        return true;
    }
    return false;
}

void playlist_prefetch_result_free(playlist_prefetch_result_t *result)
{
    if (!result) {
        return;
    }
    for (int i = 0; i < result->count; i++) {
        free(result->uris[i]);
    }
    free(result);
}

/* Deliberately does not try to cancel an in-flight task (there is no safe
 * FreeRTOS-level way to interrupt one mid esp_http_client_read() without
 * risking a leaked TLS context) - it simply drains whatever is sitting in
 * the queue so a stale result can never be handed to a NEW session's
 * http_reader after radio_pipeline_stop()/_start(). A task that is still
 * genuinely running when this is called will finish on its own a few
 * seconds later, post its (by-then-irrelevant) result, and s_in_flight will
 * clear itself - the only cost is that this module won't start a new
 * prefetch until that happens, which is a small, self-correcting delay, not
 * a stuck state. */
void playlist_prefetch_reset(void)
{
    if (!s_result_queue) {
        return;
    }
    playlist_prefetch_result_t *result = NULL;
    if (xQueueReceive(s_result_queue, &result, 0) == pdTRUE) {
        playlist_prefetch_result_free(result);
    }
}

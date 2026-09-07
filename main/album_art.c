#include "album_art.h"

#include <inttypes.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "jpeg_decoder.h"

#include "app_config.h"
#include "nowplaying.h"

static const char *TAG = "COMPANION";

#define ALBUM_ART_BUF_BYTES ((size_t)RADIO_COMPANION_ART_W * (size_t)RADIO_COMPANION_ART_H * 2)

/* Front/back double buffer + the mutex protecting the front one - see
 * album_art.h's header comment. Allocated once in album_art_start(), never
 * freed. s_back is only ever touched by album_art_task; s_front/s_version
 * are the only state a reader (album_art_get_snapshot()) ever sees. */
static SemaphoreHandle_t s_mutex;
static uint8_t *s_front;
static uint8_t *s_back;
static uint32_t s_version;

/* Bounded JPEG download scratch (RADIO_COMPANION_ART_MAX_JPEG_BYTES) -
 * allocated once, reused for every download, same "claim it while the heap
 * is clean" reasoning as nowplaying.c's own scratch buffers. */
static uint8_t *s_jpeg_buf;
static size_t s_jpeg_len;

/* Last art_url album_art_task attempted (successfully or not) - see
 * album_art_task()'s own comment on why "attempted" rather than "succeeded"
 * is what gates a retry, to avoid hammering a permanently-broken URL every
 * RADIO_COMPANION_ART_POLL_INTERVAL_MS. */
static char s_last_url[NOWPLAYING_ART_URL_MAX];

static bool s_started;

/* Same fallback-to-internal-RAM pattern as nowplaying.c's psram_alloc() -
 * duplicated locally rather than shared, matching that file's own reasoning
 * (each module keeps its own small copy rather than coupling to another
 * module's internal helper). */
static void *psram_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        ESP_LOGW(TAG, "PSRAM allocation of %u bytes failed; falling back to internal RAM",
                 (unsigned)size);
        p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return p;
}

/* HTTP GET into the bounded s_jpeg_buf scratch buffer - same event-handler-
 * appends-to-a-buffer shape as tunein_control.c's http_get()/body_append(),
 * bounded against RADIO_COMPANION_ART_MAX_JPEG_BYTES instead of growing
 * unbounded (album art is a fixed, generously-sized bound; a TuneIn JSON
 * profile response is not, which is why that file reallocs instead). */
static esp_err_t art_http_event(esp_http_client_event_t *event)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL || event->data_len <= 0) {
        return ESP_OK;
    }
    if (s_jpeg_len + (size_t)event->data_len > RADIO_COMPANION_ART_MAX_JPEG_BYTES) {
        ESP_LOGW(TAG, "Album art response exceeds %u bytes; aborting this download",
                 (unsigned)RADIO_COMPANION_ART_MAX_JPEG_BYTES);
        return ESP_FAIL;
    }
    memcpy(s_jpeg_buf + s_jpeg_len, event->data, (size_t)event->data_len);
    s_jpeg_len += (size_t)event->data_len;
    return ESP_OK;
}

static esp_err_t download_jpeg(const char *url)
{
    s_jpeg_len = 0;

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = art_http_event,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .keep_alive_enable = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && (status < 200 || status >= 300)) {
        ESP_LOGW(TAG, "Album art GET returned HTTP %d for %s", status, url);
        return ESP_FAIL;
    }
    return err;
}

/* Nearest-neighbor resize of one RGB565 image into another - see
 * album_art.h's header comment on why this, and not a box filter, for a
 * first cut: simple, correct for both up- and down-scaling (fetch_and_
 * decode() below almost always downscales, but this makes no assumption
 * either way), and entirely adequate for a ~140px on-screen thumbnail. */
static void resize_nearest_rgb565(const uint16_t *src, int src_w, int src_h,
                                   uint16_t *dst, int dst_w, int dst_h)
{
    for (int y = 0; y < dst_h; y++) {
        int sy = (y * src_h) / dst_h;
        if (sy >= src_h) {
            sy = src_h - 1;
        }
        const uint16_t *src_row = src + (size_t)sy * (size_t)src_w;
        uint16_t *dst_row = dst + (size_t)y * (size_t)dst_w;
        for (int x = 0; x < dst_w; x++) {
            int sx = (x * src_w) / dst_w;
            if (sx >= src_w) {
                sx = src_w - 1;
            }
            dst_row[x] = src_row[sx];
        }
    }
}

/* Downloads `url`'s JPEG, decodes it, resizes it into `s_back`, and swaps it
 * into `s_front` under the mutex. Every failure is logged and returned as an
 * error - the caller (album_art_task) just keeps the previous front buffer
 * (if any) and tries again on the next art_url change; a missing/broken
 * thumbnail is never a reason to affect playback. */
static esp_err_t fetch_and_decode(const char *url)
{
    if (!s_jpeg_buf || !s_back) {
        return ESP_ERR_INVALID_STATE; /* album_art_start() never called, or its PSRAM alloc failed */
    }

    esp_err_t err = download_jpeg(url);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Album art download failed for %s: %s", url, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Downloaded %u bytes of album art from %s", (unsigned)s_jpeg_len, url);

    esp_jpeg_image_cfg_t info_cfg = {
        .indata = s_jpeg_buf,
        .indata_size = (uint32_t)s_jpeg_len,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t info_out = {0};
    err = esp_jpeg_get_image_info(&info_cfg, &info_out);
    if (err != ESP_OK || info_out.width == 0 || info_out.height == 0) {
        ESP_LOGW(TAG, "esp_jpeg_get_image_info failed for %s: %s", url, esp_err_to_name(err));
        return ESP_FAIL;
    }

    /* Pick the coarsest of esp_jpeg's fixed power-of-two scales that still
     * leaves the decoded image at least as large as the target box in BOTH
     * dimensions - keeps the transient decode buffer/CPU work as small as
     * possible while still only ever downscaling in resize_nearest_rgb565()
     * above (matches the plan's design: real Apple/TuneIn art is observed
     * ~390-640px square against a 140x140 target, so this almost always
     * lands on 1/2 or 1/4, never upscaling). Falls back to no scale at all
     * if the source is already smaller than the target box in either
     * dimension. */
    esp_jpeg_image_scale_t scale = JPEG_IMAGE_SCALE_0;
    uint16_t dec_w = info_out.width;
    uint16_t dec_h = info_out.height;
    if (info_out.width / 8 >= RADIO_COMPANION_ART_W && info_out.height / 8 >= RADIO_COMPANION_ART_H) {
        scale = JPEG_IMAGE_SCALE_1_8;
        dec_w = info_out.width / 8;
        dec_h = info_out.height / 8;
    } else if (info_out.width / 4 >= RADIO_COMPANION_ART_W && info_out.height / 4 >= RADIO_COMPANION_ART_H) {
        scale = JPEG_IMAGE_SCALE_1_4;
        dec_w = info_out.width / 4;
        dec_h = info_out.height / 4;
    } else if (info_out.width / 2 >= RADIO_COMPANION_ART_W && info_out.height / 2 >= RADIO_COMPANION_ART_H) {
        scale = JPEG_IMAGE_SCALE_1_2;
        dec_w = info_out.width / 2;
        dec_h = info_out.height / 2;
    }

    size_t dec_buf_size = (size_t)dec_w * (size_t)dec_h * 2;
    /* Transient - claimed only for the duration of one decode, freed right
     * after, same "don't hold a permanent claim for an occasional operation"
     * reasoning as playlist_prefetch.c's own read buffer (see its comment in
     * app_config.h's RADIO_HTTP_BUFFER_BYTES history). */
    uint8_t *dec_buf = psram_alloc(dec_buf_size);
    if (!dec_buf) {
        ESP_LOGW(TAG, "OOM allocating %u-byte decode buffer for album art", (unsigned)dec_buf_size);
        return ESP_ERR_NO_MEM;
    }

    esp_jpeg_image_cfg_t dec_cfg = {
        .indata = s_jpeg_buf,
        .indata_size = (uint32_t)s_jpeg_len,
        .outbuf = dec_buf,
        .outbuf_size = (uint32_t)dec_buf_size,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = scale,
    };
    esp_jpeg_image_output_t dec_out = {0};
    err = esp_jpeg_decode(&dec_cfg, &dec_out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_jpeg_decode failed for %s: %s", url, esp_err_to_name(err));
        heap_caps_free(dec_buf);
        return ESP_FAIL;
    }

    resize_nearest_rgb565((const uint16_t *)dec_buf, dec_out.width, dec_out.height,
                           (uint16_t *)s_back, RADIO_COMPANION_ART_W, RADIO_COMPANION_ART_H);
    heap_caps_free(dec_buf);

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        uint8_t *tmp = s_front;
        s_front = s_back;
        s_back = tmp;
        s_version++;
        xSemaphoreGive(s_mutex);
    }
    ESP_LOGI(TAG, "Album art decoded (%ux%u source, scale 1/%d) and published (version %" PRIu32 ")",
             (unsigned)info_out.width, (unsigned)info_out.height,
             scale == JPEG_IMAGE_SCALE_0 ? 1 : (scale == JPEG_IMAGE_SCALE_1_2 ? 2 : (scale == JPEG_IMAGE_SCALE_1_4 ? 4 : 8)),
             s_version);
    return ESP_OK;
}

static void album_art_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(RADIO_COMPANION_ART_POLL_INTERVAL_MS));

        nowplaying_info_t info;
        nowplaying_get_current(&info);
        if (!info.valid || info.art_url[0] == '\0') {
            continue;
        }
        if (strcmp(info.art_url, s_last_url) == 0) {
            continue; /* already attempted this exact URL - nothing new to do */
        }
        /* Marked as attempted BEFORE the fetch, regardless of outcome - a
         * failure (dead link, transient network blip) is retried only when
         * the track/art_url itself changes again, not every poll interval,
         * so a permanently-broken URL cannot turn into a tight retry loop
         * hammering the CDN every RADIO_COMPANION_ART_POLL_INTERVAL_MS. */
        strncpy(s_last_url, info.art_url, sizeof(s_last_url) - 1);
        s_last_url[sizeof(s_last_url) - 1] = '\0';

        fetch_and_decode(info.art_url);
    }
}

esp_err_t album_art_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "OOM creating album-art mutex");
        return ESP_ERR_NO_MEM;
    }

    s_front = psram_alloc(ALBUM_ART_BUF_BYTES);
    s_back = psram_alloc(ALBUM_ART_BUF_BYTES);
    s_jpeg_buf = psram_alloc(RADIO_COMPANION_ART_MAX_JPEG_BYTES);
    if (!s_front || !s_back || !s_jpeg_buf) {
        ESP_LOGE(TAG, "OOM allocating album-art buffers - /art will stay empty (204); "
                 "nothing else is affected");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(album_art_task, "album_art", RADIO_COMPANION_ART_TASK_STACK,
                                 NULL, RADIO_COMPANION_ART_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Could not create album_art_task");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "Album art pipeline started (%dx%d RGB565, PSRAM-backed)",
             RADIO_COMPANION_ART_W, RADIO_COMPANION_ART_H);
    return ESP_OK;
}

bool album_art_get_snapshot(const uint8_t **out_buf, size_t *out_len, uint32_t *out_version)
{
    if (!s_started || !s_mutex || s_version == 0) {
        if (out_version) {
            *out_version = 0;
        }
        return false;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        if (out_version) {
            *out_version = 0;
        }
        return false;
    }
    if (out_buf) {
        *out_buf = s_front;
    }
    if (out_len) {
        *out_len = ALBUM_ART_BUF_BYTES;
    }
    if (out_version) {
        *out_version = s_version;
    }
    /* Mutex stays held - the caller (a true return means exactly this) is
     * responsible for calling album_art_release_snapshot() exactly once,
     * even if it only asked for out_version (companion_server.c's
     * /nowplaying handler does this and releases immediately, never holding
     * it across the JSON response) - see album_art.h's doc comment. */
    return true;
}

void album_art_release_snapshot(void)
{
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

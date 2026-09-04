#include "tunein_control.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "mbedtls/md5.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"

#include "app_config.h"

static const char *TAG = "TUNEIN";

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} http_body_t;

static void body_free(http_body_t *body)
{
    free(body->data);
    memset(body, 0, sizeof(*body));
}

static esp_err_t body_append(http_body_t *body, const char *data, size_t length)
{
    if (body->length + length + 1 > body->capacity) {
        /* The TuneIn profile response alone runs ~55-62KB; start bigger than
         * the old 4096 to cut down on realloc/copy churn getting there. */
        size_t new_capacity = body->capacity ? body->capacity : 8192;
        while (new_capacity < body->length + length + 1) {
            new_capacity *= 2;
        }

        char *new_data = realloc(body->data, new_capacity);
        if (!new_data) {
            return ESP_ERR_NO_MEM;
        }

        body->data = new_data;
        body->capacity = new_capacity;
    }

    memcpy(body->data + body->length, data, length);
    body->length += length;
    body->data[body->length] = '\0';
    return ESP_OK;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    if (event == NULL) return ESP_ERR_INVALID_ARG;

    http_body_t *body = event->user_data;

    switch (event->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (event->header_key != NULL && event->header_value != NULL) {
            ESP_LOGI(TAG, "HTTP HEADER: %s: %s",
                     event->header_key, event->header_value);
        }
        break;

    case HTTP_EVENT_ON_DATA:
        if (body != NULL && event->data != NULL && event->data_len > 0) {
            return body_append(body, event->data, event->data_len);
        }
        break;

    default:
        break;
    }
    return ESP_OK;
}

static esp_err_t http_get(const char *url, http_body_t *body, int *status)
{
    ESP_LOGI(TAG, "HTTP GET starting; free heap=%" PRIu32,
             esp_get_free_heap_size());
    tunein_log_url("HTTP request", url, false);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event,
        .user_data = body,
        .timeout_ms = 20000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        /* Each call here opens one client, performs one request, and tears
         * the client down immediately below - keep-alive buys nothing and
         * just holds a socket/TLS session open for no reason. */
        .keep_alive_enable = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    /* Present as an ordinary desktop-browser request rather than an
     * identifiable embedded client - both TuneIn hosts sit behind
     * Cloudflare, and a distinctive "ESP32-TuneIn-Radio/0.1" User-Agent on
     * every request is exactly the kind of signal bot-management heuristics
     * key off of. Deliberately NOT claiming gzip/br/deflate support in
     * Accept-Encoding even though a real browser would - this client has no
     * decompression, and a compressed response would silently break
     * body_append()'s parsing. */
    esp_http_client_set_header(
        client, "User-Agent",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");
    esp_http_client_set_header(
        client, "Accept",
        "application/json, application/vnd.apple.mpegurl, text/plain, */*");
    esp_http_client_set_header(client, "Accept-Language", "en-US,en;q=0.9");
    esp_http_client_set_header(client, "Sec-Fetch-Dest", "empty");
    esp_http_client_set_header(client, "Sec-Fetch-Mode", "cors");
    esp_http_client_set_header(client, "Sec-Fetch-Site", "same-site");

    esp_err_t err = esp_http_client_perform(client);
    *status = esp_http_client_get_status_code(client);

    ESP_LOGI(TAG, "HTTP GET finished: esp_err=%s http=%d bytes=%u",
             esp_err_to_name(err), *status, (unsigned)body->length);

    esp_http_client_cleanup(client);
    return err;
}

static bool is_url_safe_char(unsigned char c)
{
    return isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

static bool url_encode(const char *input, char *output, size_t output_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;

    for (; *input; ++input) {
        unsigned char c = (unsigned char)*input;

        if (is_url_safe_char(c)) {
            if (used + 1 >= output_size) return false;
            output[used++] = (char)c;
        } else {
            if (used + 3 >= output_size) return false;
            output[used++] = '%';
            output[used++] = hex[c >> 4];
            output[used++] = hex[c & 15];
        }
    }

    output[used] = '\0';
    return true;
}

static bool url_decode_until(const char *input, char *output, size_t output_size)
{
    size_t used = 0;

    while (*input && *input != '&' && *input != '"' && *input != '\\') {
        if (used + 1 >= output_size) return false;

        if (input[0] == '%' &&
            isxdigit((unsigned char)input[1]) &&
            isxdigit((unsigned char)input[2])) {
            char digits[3] = {input[1], input[2], 0};
            output[used++] = (char)strtol(digits, NULL, 16);
            input += 3;
        } else {
            output[used++] = *input++;
        }
    }

    output[used] = '\0';
    return used > 0;
}

static bool extract_item_token(const char *json, char *token, size_t token_size)
{
    const char *marker = "itemToken=";
    const char *found = strstr(json, marker);

    if (!found) {
        marker = "itemToken%3D";
        found = strstr(json, marker);
    }

    if (!found) return false;

    return url_decode_until(found + strlen(marker), token, token_size);
}

/*
 * Finds the first "https://..." URL in json that contains `marker` (e.g.
 * ".m3u8", ".mp3") and copies it out whole, query string included.
 *
 * Each candidate URL is bounded FIRST - from "https://" up to the closing
 * quote/backslash/whitespace that ends it in the JSON - and the marker is
 * then looked for only inside that span. The bounding matters now that more
 * than one marker is searched for (see tunein_start_session()'s direct-
 * stream fallback): searching the unbounded remainder of the document would
 * happily match a marker belonging to a LATER url and return everything
 * from here to there as one giant "URL". For the original single-marker
 * .m3u8 case the result is byte-for-byte what it always was, since that
 * marker is inside the first URL either way.
 */
static bool extract_url_with_marker(const char *json, const char *marker, char *url, size_t url_size)
{
    const char *start = strstr(json, "https://");

    while (start) {
        const char *end = start;
        while (*end && *end != '"' && *end != '\\' && *end != ' ' &&
               *end != '\t' && *end != '\r' && *end != '\n' && *end != '<') {
            end++;
        }

        size_t length = (size_t)(end - start);
        if (length + 1 <= url_size) {
            /* Copy first, then search the copy: the span is not NUL-
             * terminated in place, so strstr() over the original would run
             * straight past `end` into the rest of the document. */
            memcpy(url, start, length);
            url[length] = '\0';
            if (strstr(url, marker)) {
                return true;
            }
        }

        start = strstr(start + 8, "https://");
    }

    url[0] = '\0';
    return false;
}

static bool extract_hls_url(const char *json, char *url, size_t url_size)
{
    return extract_url_with_marker(json, ".m3u8", url, url_size);
}

static void make_serial(char *serial, size_t serial_size)
{
    uint8_t mac[6];

    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));

    snprintf(serial, serial_size, "esp32-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void tunein_log_url(const char *label, const char *url, bool include_query)
{
    if (include_query) {
        ESP_LOGI(TAG, "%s: %s", label, url);
        return;
    }

    const char *query = strchr(url, '?');

    if (query) {
        ESP_LOGI(TAG, "%s: %.*s?<signed/query parameters redacted>",
                 label, (int)(query - url), url);
    } else {
        ESP_LOGI(TAG, "%s: %s", label, url);
    }
}

/* Debug-only: dumps an entire HTTP response body (the profile fetch alone
 * runs ~55KB) to serial. Left disabled by default - looping fwrite()/fflush()
 * over that much data with no yield point runs long enough on real hardware
 * to starve the idle task and trip the task watchdog (observed: CPU1 IDLE
 * watchdog trigger with a backtrace landing right here, immediately followed
 * by the next TLS handshake failing). Only flip this on for short manual
 * debugging sessions, never leave it on for unattended/production runs. */
#ifndef TUNEIN_DEBUG_DUMP_RESPONSES
#define TUNEIN_DEBUG_DUMP_RESPONSES 0
#endif

static void dump_response_to_serial(const char *label, const http_body_t *body)
{
#if TUNEIN_DEBUG_DUMP_RESPONSES
    if (label == NULL || body == NULL || body->data == NULL) {
        ESP_LOGE(TAG, "No response body to dump");
        return;
    }

    ESP_LOGI(TAG, "========== %s RESPONSE BEGIN (%u bytes) ==========",
             label, (unsigned)body->length);

    const size_t chunk_size = 512;
    for (size_t offset = 0; offset < body->length; offset += chunk_size) {
        size_t n = body->length - offset;
        if (n > chunk_size) n = chunk_size;

        printf("\n[%s offset=%u length=%u]\n",
               label, (unsigned)offset, (unsigned)n);
        fwrite(body->data + offset, 1, n, stdout);
        fflush(stdout);

        /* Yield every chunk so this can never starve the watchdog even when
         * deliberately enabled for debugging. */
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    printf("\n========== %s RESPONSE END ==========\n", label);
    fflush(stdout);
#else
    (void)label;
    (void)body;
#endif
}

static bool http_response_usable(
    const char *label,
    esp_err_t err,
    int status,
    const http_body_t *body)
{
    if (status != 200 || !body->data || body->length == 0) {
        ESP_LOGE(TAG,
                 "%s failed: err=%s HTTP=%d bytes=%u",
                 label,
                 esp_err_to_name(err),
                 status,
                 (unsigned)body->length);
        return false;
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "%s ended with %s, but HTTP 200 and %u bytes were received; processing body",
                 label,
                 esp_err_to_name(err),
                 (unsigned)body->length);
    }

    return true;
}

/* Copies one line (no trailing \r\n) starting at *cursor into out, and
 * advances *cursor past it. Returns false once there is nothing left. */
static bool next_line(const char **cursor, char *out, size_t out_size)
{
    const char *p = *cursor;
    if (!p || !*p) {
        return false;
    }

    const char *eol = strchr(p, '\n');
    size_t length = eol ? (size_t)(eol - p) : strlen(p);

    while (length > 0 && p[length - 1] == '\r') {
        length--;
    }

    if (length >= out_size) {
        length = out_size - 1;
    }

    memcpy(out, p, length);
    out[length] = '\0';
    *cursor = eol ? eol + 1 : p + strlen(p);
    return true;
}

/*
 * Picks the AAC-LC ("mp4a.40.2") rendition out of an HLS master playlist.
 * fmp4_bridge only understands plain AAC-LC (no SBR/HE-AAC), so this is a
 * deliberate choice, not just "closest to some preferred bitrate": it must
 * agree with what fmp4_bridge was primed for by the init segment.
 */
static bool select_hls_variant(const char *master_text, char *out_url, size_t out_size)
{
    /* #EXT-X-STREAM-INF attribute lines are short (BANDWIDTH/CODECS/etc.),
     * but the URI line right after each one is a full TuneIn/Apple CDN URL
     * with a signed query string - the same kind of URL session->
     * hls_master_url/hls_variant_url are sized 1536 bytes for. Sizing
     * uri_line/fallback any smaller than that risks next_line() silently
     * truncating a real variant URL before it ever reaches those fields,
     * producing a garbled URL whose eventual HTTP failure wouldn't look
     * anything like "buffer too small". */
    char line[768];
    char fallback[1536] = {0};
    bool have_fallback = false;
    const char *cursor = master_text;

    while (next_line(&cursor, line, sizeof(line))) {
        if (strncmp(line, "#EXT-X-STREAM-INF:", 18) != 0) {
            continue;
        }

        bool is_aac_lc = strstr(line, "mp4a.40.2") != NULL;
        char uri_line[1536];

        if (!next_line(&cursor, uri_line, sizeof(uri_line)) ||
            uri_line[0] == '#' || uri_line[0] == '\0') {
            continue;
        }

        if (is_aac_lc) {
            snprintf(out_url, out_size, "%s", uri_line);
            return true;
        }

        if (!have_fallback) {
            snprintf(fallback, sizeof(fallback), "%s", uri_line);
            have_fallback = true;
        }
    }

    if (have_fallback) {
        ESP_LOGW(TAG, "No mp4a.40.2 (AAC-LC) HLS variant found; falling back to the first variant listed");
        snprintf(out_url, out_size, "%s", fallback);
        return true;
    }

    return false;
}

/* Finds the CMAF init segment URI out of an EXT-X-MAP tag in a media
 * playlist. ADF's bundled HLS parser recognizes but drops this tag, so it
 * has to be fetched by hand. */
static bool extract_map_uri(const char *media_playlist_text, char *out_url, size_t out_size)
{
    const char *marker = "#EXT-X-MAP:URI=\"";
    const char *found = strstr(media_playlist_text, marker);

    if (!found) return false;

    const char *start = found + strlen(marker);
    const char *end = strchr(start, '"');

    if (!end) return false;

    size_t length = (size_t)(end - start);
    if (length + 1 > out_size) return false;

    memcpy(out_url, start, length);
    out_url[length] = '\0';
    return true;
}

/*
 * Resolves session->hls_master_url down to a specific AAC-LC media playlist
 * plus its CMAF init segment bytes. hls_variant_url is what actually gets
 * handed to ADF's http_stream (not the master) so the variant it streams is
 * guaranteed to be the one fmp4_bridge was primed for.
 */
static esp_err_t resolve_hls_variant_and_init(tunein_session_t *session)
{
    esp_err_t err;
    int status = 0;
    http_body_t master = {0};
    http_body_t variant = {0};
    http_body_t init_seg = {0};
    char init_url[1536];

    err = http_get(session->hls_master_url, &master, &status);
    if (!http_response_usable("HLS master playlist request", err, status, &master)) {
        err = (err == ESP_OK) ? ESP_FAIL : err;
        goto cleanup;
    }

    if (!select_hls_variant(master.data, session->hls_variant_url, sizeof(session->hls_variant_url))) {
        ESP_LOGE(TAG, "No HLS variant found in master playlist");
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    if (strncmp(session->hls_variant_url, "http", 4) != 0) {
        ESP_LOGE(TAG, "HLS variant URI is not absolute (relative playlist URLs are not supported): %s",
                 session->hls_variant_url);
        err = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }
    tunein_log_url("HLS variant playlist", session->hls_variant_url, false);
    body_free(&master);

    status = 0;
    err = http_get(session->hls_variant_url, &variant, &status);
    if (!http_response_usable("HLS variant playlist request", err, status, &variant)) {
        err = (err == ESP_OK) ? ESP_FAIL : err;
        goto cleanup;
    }

    if (!extract_map_uri(variant.data, init_url, sizeof(init_url))) {
        /* No EXT-X-MAP: the segments are not CMAF/fMP4, so there is no
         * AudioSpecificConfig to prime fmp4_bridge with and no reason to
         * involve it. Bare ADTS AAC and MPEG-TS segments are both
         * self-describing, so they go straight into esp_decoder, which
         * sniffs the codec itself - see TUNEIN_FORMAT_HLS_GENERIC in
         * tunein_control.h and the chain radio_pipeline_start() builds for
         * it. This used to be a hard ESP_ERR_NOT_FOUND. */
        ESP_LOGW(TAG, "No EXT-X-MAP in the HLS variant playlist - segments are not CMAF/fMP4; "
                 "using ESP-ADF's auto-detecting decoder instead of the fMP4 bridge");
        session->format = TUNEIN_FORMAT_HLS_GENERIC;
        session->init_segment_len = 0;
        body_free(&variant);
        session->resolved_at_us = esp_timer_get_time();
        err = ESP_OK;
        goto cleanup;
    }

    session->format = TUNEIN_FORMAT_HLS_CMAF_AAC;
    tunein_log_url("CMAF init segment", init_url, false);
    body_free(&variant);

    status = 0;
    err = http_get(init_url, &init_seg, &status);
    if (!http_response_usable("CMAF init segment request", err, status, &init_seg)) {
        err = (err == ESP_OK) ? ESP_FAIL : err;
        goto cleanup;
    }
    if (init_seg.length > sizeof(session->init_segment)) {
        ESP_LOGE(TAG, "CMAF init segment too large (%u bytes, max %u)",
                 (unsigned)init_seg.length, (unsigned)sizeof(session->init_segment));
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    memcpy(session->init_segment, init_seg.data, init_seg.length);
    session->init_segment_len = init_seg.length;
    session->resolved_at_us = esp_timer_get_time();
    err = ESP_OK;

cleanup:
    body_free(&master);
    body_free(&variant);
    body_free(&init_seg);
    return err;
}

static void wait_for_sntp_sync(void)
{
    time_t now = 0;
    struct tm timeinfo = {0};

    for (int retry = 0; retry < 20; retry++) {
        time(&now);
        localtime_r(&now, &timeinfo);

        if (timeinfo.tm_year >= (2024 - 1900)) {
            ESP_LOGI(TAG, "SNTP synchronized successfully");
            break;
        }

        ESP_LOGI(TAG, "Waiting for SNTP synchronization...");
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (timeinfo.tm_year < (2024 - 1900)) {
        ESP_LOGW(TAG, "SNTP synchronization failed or timed out");
    }
}

static void test_dns(void)
{
    struct addrinfo hints = {0};
    struct addrinfo *result = NULL;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(
        "api.tunein.com",
        NULL,
        &hints,
        &result
    );

    if (err != 0) {
        ESP_LOGE(TAG,
                 "DNS lookup failed: %d",
                 err);
        return;
    }

    ESP_LOGI(TAG,
             "DNS lookup successful");

    freeaddrinfo(result);
}

/*
 * Fallback for a Tune.ashx response with no .m3u8 in it at all: TuneIn
 * handed back a direct, continuous media stream rather than an HLS
 * playlist. Looks for a URL ending in a container this build can actually
 * decode and records it as TUNEIN_FORMAT_DIRECT_GENERIC.
 *
 * The markers are tried most-specific first, and the list is deliberately
 * limited to what ESP-ADF's own esp_decoder ships a decoder for (see
 * radio_pipeline.c's build_generic_decoder()) - there is no point matching
 * a container nothing downstream can open. ".mp4" and ".m4a" are the same
 * decoder (ESP_CODEC_TYPE_M4A); both spellings appear in the wild.
 *
 * Deliberately NOT matched: .pls and .asx playlist wrappers. http_stream's
 * own playlist parser could follow those, but they are a redirect layer
 * rather than a stream, and this project has no station that needs one -
 * matching them would mean claiming success here for a URL that still
 * might not resolve to anything playable.
 */
static bool resolve_direct_stream(tunein_session_t *session, const char *tune_json)
{
    static const char *const markers[] = {
        ".aac", ".m4a", ".mp4", ".mp3", ".flac", ".opus", ".ogg", ".wav",
    };

    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
        if (extract_url_with_marker(tune_json, markers[i],
                                    session->hls_variant_url,
                                    sizeof(session->hls_variant_url))) {
            session->format = TUNEIN_FORMAT_DIRECT_GENERIC;
            session->init_segment_len = 0;
            ESP_LOGW(TAG, "Tune.ashx returned no HLS playlist; falling back to a direct '%s' stream "
                     "(codec auto-detected by ESP-ADF's esp_decoder)", markers[i]);
            tunein_log_url("TuneIn session resolved direct stream", session->hls_variant_url, false);
            return true;
        }
    }

    return false;
}

esp_err_t tunein_start_session(tunein_session_t *session, const char *station_id)
{
    /* NULL keeps the pre-multi-station behavior (and any caller that has no
     * station list of its own) working unchanged. */
    if (!station_id) {
        station_id = RADIO_TUNEIN_STATION_ID;
    }

    esp_err_t err = ESP_FAIL;
    int status = 0;
    http_body_t profile = {0};
    http_body_t tune = {0};

    wait_for_sntp_sync();
    test_dns();

    char *url_buffer = malloc(4096);
    char *encoded_token = malloc(768);

    if (!url_buffer || !encoded_token) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    memset(session, 0, sizeof(*session));
    make_serial(session->serial, sizeof(session->serial));
    snprintf(session->station_id, sizeof(session->station_id), "%s", station_id);

    uint64_t unix_or_uptime = (uint64_t)time(NULL);
    if (unix_or_uptime < 1700000000) {
        unix_or_uptime = (uint64_t)(esp_timer_get_time() / 1000000);
        ESP_LOGW(TAG, "SNTP time unavailable; using uptime as listenId");
    }

    snprintf(session->listen_id, sizeof(session->listen_id),
             "%" PRIu64, unix_or_uptime);

    ESP_LOGI(TAG,
             "Starting TuneIn session: station=%s serial=%s listenId=%s",
             station_id,
             session->serial,
             session->listen_id);

    int written = snprintf(
        url_buffer,
        4096,
        "https://api.tunein.com/profiles/%s/contents?"
        "itemUrlScheme=secure&serial=%s&partnerId=%s&version=%s&formats=%s",
        station_id,
        session->serial,
        RADIO_TUNEIN_PARTNER_ID,
        RADIO_TUNEIN_VERSION,
        RADIO_TUNEIN_FORMATS
    );

    if (written < 0 || written >= 4096) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    err = http_get(url_buffer, &profile, &status);

    if (!http_response_usable("Profile request", err, status, &profile)) {
        err = (err == ESP_OK) ? ESP_FAIL : err;
        goto cleanup;
    }

    dump_response_to_serial("PROFILE", &profile);

	ESP_LOGI(TAG, "Profile response received: %u bytes; itemToken present: %s",
             (unsigned)profile.length,
             strstr(profile.data, "itemToken") ? "YES" : "NO");

    if (!extract_item_token(profile.data,
                            session->item_token,
                            sizeof(session->item_token))) {
        ESP_LOGE(TAG,
                 "Profile response did not contain itemToken; bytes=%u",
                 (unsigned)profile.length);
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    ESP_LOGI(TAG,
             "Profile complete: itemToken extracted (redacted), bytes=%u",
             (unsigned)profile.length);

    body_free(&profile);

    if (!url_encode(session->item_token, encoded_token, 768)) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    written = snprintf(
        url_buffer,
        4096,
        "https://opml.radiotime.com/Tune.ashx?"
        "id=%s&itemToken=%s&listenId=%s&itemUrlScheme=secure&serial=%s"
        "&partnerId=%s&version=%s&formats=%s&render=json",
        station_id,
        encoded_token,
        session->listen_id,
        session->serial,
        RADIO_TUNEIN_PARTNER_ID,
        RADIO_TUNEIN_VERSION,
        RADIO_TUNEIN_FORMATS
    );

    if (written < 0 || written >= 4096) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    status = 0;
    err = http_get(url_buffer, &tune, &status);

    if (!http_response_usable("Tune.ashx request", err, status, &tune)) {
        err = (err == ESP_OK) ? ESP_FAIL : err;
        goto cleanup;
    }

    dump_response_to_serial("TUNE", &tune);
	ESP_LOGI(TAG, "Tune.ashx response received: %u bytes",
             (unsigned)tune.length);

    if (extract_hls_url(tune.data,
                        session->hls_master_url,
                        sizeof(session->hls_master_url))) {
        tunein_log_url(
            "TuneIn session resolved HLS master",
            session->hls_master_url,
            false
        );

        err = resolve_hls_variant_and_init(session);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to resolve HLS variant/init segment: %s", esp_err_to_name(err));
            goto cleanup;
        }
    } else if (resolve_direct_stream(session, tune.data)) {
        /* Not HLS at all - Tune.ashx pointed straight at one continuous
         * media stream. Nothing more to resolve: the URL goes to
         * http_stream as-is and esp_decoder works out the codec. */
        session->resolved_at_us = esp_timer_get_time();
        err = ESP_OK;
    } else {
        ESP_LOGE(TAG,
                 "Tune.ashx returned no playable stream URL (no .m3u8 and no known direct "
                 "container extension); bytes=%u",
                 (unsigned)tune.length);
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

cleanup:
    body_free(&profile);
    body_free(&tune);
    free(encoded_token);
    free(url_buffer);
    return err;
}

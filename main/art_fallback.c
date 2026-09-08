#include "art_fallback.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"

static const char *TAG = "ART_FALLBACK";

/* Bounded scratch buffer every tier's HTTP response is downloaded into
 * before cJSON parses it, reused sequentially across all three tiers - this
 * module has exactly one caller (album_art_task, see art_fallback.h's
 * threading comment), so a lookup is never in flight on two tiers at once
 * to fight over it. Sized for MusicBrainz's own worst case: its
 * inc=releases response embeds each recording's full artist-credit object,
 * aliases and all (verified directly against a real query during PoC
 * testing - a single alias-heavy artist's recording+releases JSON ran well
 * into five figures of bytes) - iTunes/Deezer responses at limit=1 are a
 * small fraction of that. PSRAM-backed, same psram_alloc() pattern
 * nowplaying.c/album_art.c already use - this board has 8MB of PSRAM sitting
 * mostly idle relative to what audio playback needs (see app_config.h's
 * PSRAM migration comment), so a generous bound here costs nothing
 * meaningful. A response that still overflows this (an extreme alias count)
 * just fails that one lookup gracefully - see scratch_event()'s comment. */
#define ART_FALLBACK_JSON_BUF_BYTES   (64 * 1024)

/* MusicBrainz asks non-commercial clients to stay at <= 1 request/second -
 * enforced as a floor on the gap between the START of one MB HTTP call and
 * the next (a slow prior request may already have used up the whole
 * interval on its own), same reasoning as the PoC's own _mb_rate_limit().
 * Cover Art Archive is a different service entirely (backed by the Internet
 * Archive) and is NOT subject to this - only calls to musicbrainz.org
 * itself are throttled. */
#define ART_FALLBACK_MB_MIN_INTERVAL_US   (1100 * 1000)
static int64_t s_mb_last_call_us;

/* limit=5, not the PoC's limit=10 - halves MusicBrainz's own worst-case
 * response size (see ART_FALLBACK_JSON_BUF_BYTES above) in exchange for a
 * slightly narrower net than the PoC casts; acceptable because this is the
 * LAST tier of a last-resort fallback path in the first place (see
 * art_fallback.h) - PoC testing only ever needed the wider net (5 -> 10)
 * once, for one unusually bootleg-heavy artist (Radiohead); most stations'
 * tracks are far less live/bootleg-recorded than that. */
#define ART_FALLBACK_MB_LIMIT   5
/* Caps the total number of Cover Art Archive existence checks (one HTTPS
 * HEAD each) across every candidate release this tier considers - bounds
 * the worst-case extra round trips on top of the one MusicBrainz search
 * request itself, independent of how many releases MB actually returned. */
#define ART_FALLBACK_MB_MAX_CAA_CHECKS   5

/* MusicBrainz explicitly requires an identifying User-Agent with contact
 * info for non-browser clients; a generic UA gets more aggressively
 * rate-limited or blocked outright - same requirement the PoC's
 * MB_USER_AGENT documents. */
#define ART_FALLBACK_MB_USER_AGENT \
    "esp32_wifi_streamer-art-fallback/0.1 ( https://github.com/ashrockd/esp32_wifi_streamer )"
#define ART_FALLBACK_USER_AGENT \
    "esp32_wifi_streamer-art-fallback/0.1"

static char *s_scratch;
static size_t s_scratch_len;

/* Same fallback-to-internal-RAM pattern as nowplaying.c's/album_art.c's own
 * psram_alloc() - duplicated locally rather than shared, matching those
 * files' own "each module keeps its own small copy" convention (see
 * album_art.c's comment on this). */
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

esp_err_t art_fallback_init(void)
{
    if (s_scratch) {
        return ESP_OK; /* idempotent, same as album_art_start()/nowplaying_init() */
    }
    s_scratch = psram_alloc(ART_FALLBACK_JSON_BUF_BYTES);
    if (!s_scratch) {
        ESP_LOGW(TAG, "OOM allocating %u-byte lookup scratch buffer; 3rd-party artwork "
                 "lookup will be unavailable (embedded artwork is completely unaffected)",
                 (unsigned)ART_FALLBACK_JSON_BUF_BYTES);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* --------------------------------------------------------------------
 * URL encoding - duplicated locally from tunein_control.c's
 * is_url_safe_char()/url_encode() rather than shared (that pair is static
 * to that file; this project's convention is a small local copy per module
 * over a new shared header for a two-function helper - see album_art.c's
 * psram_alloc() comment on the same choice).
 * -------------------------------------------------------------------- */
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

/* --------------------------------------------------------------------
 * HTTP GET (into the shared scratch buffer) / HEAD (status only, no body)
 * -------------------------------------------------------------------- */

static esp_err_t scratch_event(esp_http_client_event_t *event)
{
    if (!event) return ESP_ERR_INVALID_ARG;
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->data || event->data_len <= 0) {
        return ESP_OK;
    }
    if (s_scratch_len + (size_t)event->data_len + 1 > ART_FALLBACK_JSON_BUF_BYTES) {
        ESP_LOGW(TAG, "Response exceeds %u-byte scratch buffer; aborting this lookup",
                 (unsigned)ART_FALLBACK_JSON_BUF_BYTES);
        return ESP_FAIL; /* aborts esp_http_client_perform(); caller treats as a miss */
    }
    memcpy(s_scratch + s_scratch_len, event->data, (size_t)event->data_len);
    s_scratch_len += (size_t)event->data_len;
    s_scratch[s_scratch_len] = '\0';
    return ESP_OK;
}

static esp_err_t fallback_http_get(const char *url, const char *user_agent, int *out_status)
{
    s_scratch_len = 0;
    if (s_scratch) {
        s_scratch[0] = '\0';
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = scratch_event,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .keep_alive_enable = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "User-Agent", user_agent);
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err;
}

/* HEAD only - Cover Art Archive existence check (MusicBrainz tier). No
 * event_handler/body buffer needed at all: a HEAD response carries no body,
 * only the status code this is checking. */
static esp_err_t fallback_http_head(const char *url, const char *user_agent, int *out_status)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_HEAD,
        .timeout_ms = 10000,
        .keep_alive_enable = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "User-Agent", user_agent);

    esp_err_t err = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err;
}

/* --------------------------------------------------------------------
 * 1. iTunes Search API - primary
 * -------------------------------------------------------------------- */

static bool itunes_lookup(const char *title, const char *artist, char *out_url, size_t out_url_size)
{
    char term[300];
    if (artist && artist[0]) {
        snprintf(term, sizeof(term), "%s %s", artist, title);
    } else {
        snprintf(term, sizeof(term), "%s", title);
    }

    char encoded[900];
    if (!url_encode(term, encoded, sizeof(encoded))) {
        return false;
    }

    char url[1024];
    snprintf(url, sizeof(url),
              "https://itunes.apple.com/search?term=%s&media=music&entity=song&limit=1",
              encoded);

    int status = 0;
    if (fallback_http_get(url, ART_FALLBACK_USER_AGENT, &status) != ESP_OK || status != 200) {
        return false;
    }

    cJSON *root = cJSON_Parse(s_scratch);
    if (!root) {
        return false;
    }
    bool found = false;
    cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    cJSON *first = cJSON_GetArrayItem(results, 0);
    cJSON *art = first ? cJSON_GetObjectItemCaseSensitive(first, "artworkUrl100") : NULL;
    if (cJSON_IsString(art) && art->valuestring[0]) {
        /* Request exactly RADIO_COMPANION_ART_W x _H (240x240), not iTunes'
         * default 100x100 - see this file's header comment and
         * album_art.c's fetch_and_decode() for why an exact match matters.
         * "100x100bb" and "240x240bb" are both 9 bytes - a fixed-width
         * in-place substring replace, no buffer growth/shift needed. */
        char resized[ART_FALLBACK_URL_MAX];
        snprintf(resized, sizeof(resized), "%s", art->valuestring);
        char size_tag[16];
        snprintf(size_tag, sizeof(size_tag), "%dx%dbb", RADIO_COMPANION_ART_W, RADIO_COMPANION_ART_H);
        char *hit = strstr(resized, "100x100bb");
        if (hit && strlen(size_tag) == strlen("100x100bb")) {
            memcpy(hit, size_tag, strlen(size_tag));
        }
        snprintf(out_url, out_url_size, "%s", resized);
        found = true;
    }
    cJSON_Delete(root);
    return found;
}

/* --------------------------------------------------------------------
 * 2. MusicBrainz (recording search) + Cover Art Archive
 * -------------------------------------------------------------------- */

static bool musicbrainz_lookup(const char *title, const char *artist, char *out_url, size_t out_url_size)
{
    char query[300];
    if (artist && artist[0]) {
        snprintf(query, sizeof(query), "recording:\"%s\" AND artist:\"%s\"", title, artist);
    } else {
        snprintf(query, sizeof(query), "recording:\"%s\"", title);
    }

    char encoded[900];
    if (!url_encode(query, encoded, sizeof(encoded))) {
        return false;
    }

    char url[1024];
    snprintf(url, sizeof(url),
              "https://musicbrainz.org/ws/2/recording/?query=%s&fmt=json&limit=%d&inc=releases",
              encoded, ART_FALLBACK_MB_LIMIT);

    /* Rate limit floor before the request STARTS, not a blanket sleep every
     * call - see ART_FALLBACK_MB_MIN_INTERVAL_US's own comment. */
    int64_t now_us = esp_timer_get_time();
    int64_t elapsed_us = now_us - s_mb_last_call_us;
    if (elapsed_us < ART_FALLBACK_MB_MIN_INTERVAL_US) {
        vTaskDelay(pdMS_TO_TICKS((ART_FALLBACK_MB_MIN_INTERVAL_US - elapsed_us) / 1000));
    }
    s_mb_last_call_us = esp_timer_get_time();

    int status = 0;
    if (fallback_http_get(url, ART_FALLBACK_MB_USER_AGENT, &status) != ESP_OK || status != 200) {
        return false;
    }

    cJSON *root = cJSON_Parse(s_scratch);
    if (!root) {
        return false;
    }

    /* Two passes: every "Official" release first (an actual retail/
     * streaming album), then anything else, on the SAME bounded check
     * budget (ART_FALLBACK_MB_MAX_CAA_CHECKS total, not per pass) - see
     * this file's header comment / musicbrainz_lookup's own reasoning for
     * why officialness is checked before spending a Cover Art Archive round
     * trip: a same-titled live/bootleg release essentially never has art,
     * confirmed directly during PoC testing. Recordings stay in
     * MusicBrainz's own best-match-score order within each pass. */
    bool found = false;
    int checks = 0;
    cJSON *recordings = cJSON_GetObjectItemCaseSensitive(root, "recordings");
    for (int pass = 0; pass < 2 && !found; pass++) {
        cJSON *recording = NULL;
        cJSON_ArrayForEach(recording, recordings) {
            cJSON *releases = cJSON_GetObjectItemCaseSensitive(recording, "releases");
            cJSON *release = NULL;
            cJSON_ArrayForEach(release, releases) {
                cJSON *status_field = cJSON_GetObjectItemCaseSensitive(release, "status");
                bool is_official = cJSON_IsString(status_field) &&
                                    strcmp(status_field->valuestring, "Official") == 0;
                if ((pass == 0) != is_official) {
                    continue; /* pass 0: official only; pass 1: everything not already tried */
                }

                cJSON *id = cJSON_GetObjectItemCaseSensitive(release, "id");
                if (!cJSON_IsString(id) || !id->valuestring[0]) {
                    continue;
                }
                if (checks >= ART_FALLBACK_MB_MAX_CAA_CHECKS) {
                    goto done;
                }
                checks++;

                /* Fixed thumbnail sizes only (250/500/1200) - none is
                 * exactly 240, so this always goes through
                 * fetch_and_decode()'s normal resize path, same as
                 * Deezer - see this file's header comment. front-250 is
                 * the closest available size without downloading more
                 * bytes than a 240px box needs. */
                char caa_url[200];
                snprintf(caa_url, sizeof(caa_url),
                          "https://coverartarchive.org/release/%s/front-250", id->valuestring);
                int caa_status = 0;
                if (fallback_http_head(caa_url, ART_FALLBACK_MB_USER_AGENT, &caa_status) == ESP_OK &&
                    caa_status >= 200 && caa_status < 300) {
                    snprintf(out_url, out_url_size, "%s", caa_url);
                    found = true;
                    goto done;
                }
                /* A 404 here (no art for this specific release) is
                 * expected and not logged - see this file's header
                 * comment; only the final "no fallback match at all"
                 * outcome (art_fallback_resolve()) is worth a log line. */
            }
        }
    }
done:
    cJSON_Delete(root);
    return found;
}

/* --------------------------------------------------------------------
 * 3. Deezer - last (see this file's header comment for why)
 * -------------------------------------------------------------------- */

static bool deezer_lookup(const char *title, const char *artist, char *out_url, size_t out_url_size)
{
    char term[300];
    if (artist && artist[0]) {
        snprintf(term, sizeof(term), "%s %s", artist, title);
    } else {
        snprintf(term, sizeof(term), "%s", title);
    }

    char encoded[900];
    if (!url_encode(term, encoded, sizeof(encoded))) {
        return false;
    }

    char url[1024];
    snprintf(url, sizeof(url), "https://api.deezer.com/search?q=%s&limit=1", encoded);

    int status = 0;
    if (fallback_http_get(url, ART_FALLBACK_USER_AGENT, &status) != ESP_OK || status != 200) {
        return false;
    }

    cJSON *root = cJSON_Parse(s_scratch);
    if (!root) {
        return false;
    }
    bool found = false;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *first = cJSON_GetArrayItem(data, 0);
    cJSON *album = first ? cJSON_GetObjectItemCaseSensitive(first, "album") : NULL;
    /* cover_medium (250x250) is the closest fixed Deezer size to
     * RADIO_COMPANION_ART_W/_H (240x240) without going all the way up to
     * cover_big (500x500) for no benefit - see this file's header comment;
     * neither is an exact match, so this always goes through
     * fetch_and_decode()'s normal resize path. */
    cJSON *art = album ? cJSON_GetObjectItemCaseSensitive(album, "cover_medium") : NULL;
    if (!cJSON_IsString(art) || !art->valuestring[0]) {
        art = album ? cJSON_GetObjectItemCaseSensitive(album, "cover_big") : NULL;
    }
    if (cJSON_IsString(art) && art->valuestring[0]) {
        snprintf(out_url, out_url_size, "%s", art->valuestring);
        found = true;
    }
    cJSON_Delete(root);
    return found;
}

/* --------------------------------------------------------------------
 * Put it all together
 * -------------------------------------------------------------------- */

bool art_fallback_resolve(const char *title, const char *artist, char *out_url, size_t out_url_size)
{
    if (!title || !title[0]) {
        return false;
    }
    if (!s_scratch) {
        return false; /* art_fallback_init() never called, or its PSRAM alloc failed - see its own comment */
    }
    if (!artist) {
        artist = "";
    }

    if (itunes_lookup(title, artist, out_url, out_url_size)) {
        ESP_LOGI(TAG, "Fallback artwork resolved via iTunes for '%s' - '%s'", artist, title);
        return true;
    }
    if (musicbrainz_lookup(title, artist, out_url, out_url_size)) {
        ESP_LOGI(TAG, "Fallback artwork resolved via MusicBrainz+CoverArtArchive for '%s' - '%s'", artist, title);
        return true;
    }
    if (deezer_lookup(title, artist, out_url, out_url_size)) {
        ESP_LOGI(TAG, "Fallback artwork resolved via Deezer for '%s' - '%s'", artist, title);
        return true;
    }

    ESP_LOGI(TAG, "No fallback artwork found for '%s' - '%s' (tried iTunes, MusicBrainz+CAA, Deezer)",
             artist, title);
    return false;
}

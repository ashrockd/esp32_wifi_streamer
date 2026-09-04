#include "nowplaying.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "NOWPLAYING";

/* One buffer, reused sequentially for both fetches this task makes (the
 * playlist body, then the segment's ID3 prefix) - same "allocate fresh per
 * attempt, free before the task exits" reasoning as playlist_prefetch.c's
 * PREFETCH_BODY_BUF_BYTES (see its own comment for the OOM-at-pipeline-
 * startup history that motivated NOT preallocating this permanently).
 *
 * Sizing: real playlist bodies observed on this stream run ~5.7KB (tools/
 * tunin test/variant.m3u8) - comfortably under this. For the segment prefix,
 * a real captured segment (tools/tunin test/seg.mp4) showed the full ID3v2
 * tag (header + PRIV + 2x WXXX + TALB/TPE1/TIT2) runs ~4.1KB and repeats
 * roughly every 4.2KB throughout the segment - 16KB comfortably contains
 * the first repeat in full even if the segment's leading boxes (styp/emsg
 * framing before the first tag) push its start back a bit, and gives 2-3x
 * redundancy against a truncated first copy. */
#define NOWPLAYING_FETCH_BUF_BYTES  (16 * 1024)
#define NOWPLAYING_TASK_STACK_BYTES 4096
#define NOWPLAYING_TASK_PRIORITY    3
#define NOWPLAYING_HTTP_TIMEOUT_MS  8000

static esp_err_t (*s_crt_bundle_attach)(void *conf) = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static volatile bool s_in_flight = false;

/* Shared result state, protected by s_mutex - written only by the poll
 * task's very last step before it deletes itself, read only by
 * nowplaying_get_current(). age_ms in nowplaying_info_t is NOT stored here;
 * it is derived from s_updated_tick at read time. */
static nowplaying_info_t s_current;
static TickType_t s_updated_tick;

esp_err_t nowplaying_init(esp_err_t (*crt_bundle_attach)(void *conf))
{
    if (s_mutex != NULL) {
        return ESP_OK; /* already initialized */
    }
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "OOM creating result mutex");
        return ESP_ERR_NO_MEM;
    }
    s_crt_bundle_attach = crt_bundle_attach;
    ESP_LOGI(TAG, "Initialized");
    return ESP_OK;
}

/* ---- ID3v2 parsing - see nowplaying.h's PORTING NOTE for why the header
 * validation below is NOT optional the way it was in the python original.
 * ---- */

static inline uint32_t synchsafe(const uint8_t *b)
{
    return ((uint32_t)b[0] << 21) | ((uint32_t)b[1] << 14) | ((uint32_t)b[2] << 7) | b[3];
}

/* Rejects both plain garbage AND the specific false-positive this project's
 * own CMAF stream produces: an fMP4 'emsg' box's scheme_id_uri contains the
 * literal substring "ID3" (e.g. ".../emsg/ID3") ahead of the real tag, which
 * a naive "find the bytes ID3, treat the next 10 as a header" scan (the
 * python original) walks straight into. A real ID3v2 header's major version
 * is always 3 or 4 on every station observed (2.2's different, 6-byte frame
 * header layout is not handled by the frame walker below at all, so major
 * version 2 is deliberately excluded here too rather than half-parsed), its
 * two reserved low flag bits are always 0, and its 4 size bytes are
 * synchsafe (top bit clear on each, by construction) - the false-positive
 * match above fails the major-version check outright (its "major" byte
 * lands on 0x00, part of the scheme URI's own NUL terminator). */
static bool id3_header_valid(const uint8_t *h)
{
    if (memcmp(h, "ID3", 3) != 0) {
        return false;
    }
    uint8_t major = h[3];
    if (major != 3 && major != 4) {
        return false;
    }
    if (h[5] & 0x0F) {
        return false; /* undefined/reserved flag bits must be 0 */
    }
    for (int i = 6; i < 10; i++) {
        if (h[i] & 0x80) {
            return false; /* not synchsafe */
        }
    }
    return true;
}

static int utf8_encode_cp(uint32_t cp, char *out, size_t remaining)
{
    if (cp <= 0x7F) {
        if (remaining < 1) return 0;
        out[0] = (char)cp;
        return 1;
    } else if (cp <= 0x7FF) {
        if (remaining < 2) return 0;
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp <= 0xFFFF) {
        if (remaining < 3) return 0;
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        if (remaining < 4) return 0;
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

/* ISO-8859-1 (ID3 text encoding 0) -> UTF-8. Every Latin-1 byte is already
 * that exact Unicode code point, so this is a straight expand-to-UTF-8, no
 * table needed. */
static void latin1_to_utf8(const uint8_t *in, size_t in_len, char *out, size_t out_cap)
{
    size_t used = 0;
    for (size_t i = 0; i < in_len && used + 1 < out_cap; i++) {
        int n = utf8_encode_cp(in[i], out + used, out_cap - used - 1);
        if (n == 0) break;
        used += (size_t)n;
    }
    out[used] = '\0';
}

/* UTF-16 (ID3 text encodings 1 [with BOM] and 2 [big-endian, no BOM]) ->
 * UTF-8, including surrogate-pair handling for characters outside the BMP -
 * unlikely to ever matter for a song title, but cheap to get right. A
 * missing/unexpected BOM on encoding 1 falls back to `default_big_endian`,
 * same as any real-world tolerant decoder. */
static void utf16_to_utf8(const uint8_t *in, size_t in_len, bool default_big_endian,
                           char *out, size_t out_cap)
{
    bool big_endian = default_big_endian;
    size_t i = 0;
    if (in_len >= 2) {
        if (in[0] == 0xFE && in[1] == 0xFF) { big_endian = true; i = 2; }
        else if (in[0] == 0xFF && in[1] == 0xFE) { big_endian = false; i = 2; }
    }
    size_t used = 0;
    while (i + 2 <= in_len && used + 1 < out_cap) {
        uint16_t unit = big_endian ? ((uint16_t)in[i] << 8 | in[i + 1])
                                    : ((uint16_t)in[i + 1] << 8 | in[i]);
        i += 2;
        uint32_t cp;
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 2 <= in_len) {
            uint16_t lo = big_endian ? ((uint16_t)in[i] << 8 | in[i + 1])
                                      : ((uint16_t)in[i + 1] << 8 | in[i]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + (((uint32_t)(unit - 0xD800)) << 10) + (lo - 0xDC00);
                i += 2;
            } else {
                cp = unit; /* unpaired high surrogate - pass through, best effort */
            }
        } else {
            cp = unit;
        }
        int n = utf8_encode_cp(cp, out + used, out_cap - used - 1);
        if (n == 0) break;
        used += (size_t)n;
    }
    out[used] = '\0';
}

/* Decodes a text-information frame's payload (TIT2/TPE1/TALB/...): first
 * byte is the ID3 text-encoding marker, the rest is the (possibly NUL-
 * terminated) string in that encoding. A frame's own trailing NUL (encodings
 * 0/3 carry one byte, 1/2 two - all optional per spec) needs no separate
 * trim: every branch below always writes ITS OWN final '\0' at the end of
 * whatever it copied/converted, so a frame-supplied terminator earlier in
 * the bytes just becomes an embedded '\0' that any C string read (strlen(),
 * "%s", ...) already stops at first - same visible result either way,
 * matching tunein_nowplaying.py's explicit rstrip("\x00") without needing
 * one here. */
static void decode_text_frame(const uint8_t *raw, uint32_t raw_len, char *out, size_t out_cap)
{
    out[0] = '\0';
    if (raw_len == 0) {
        return;
    }
    uint8_t enc = raw[0];
    const uint8_t *body = raw + 1;
    uint32_t body_len = raw_len - 1;
    switch (enc) {
    case 0: latin1_to_utf8(body, body_len, out, out_cap); break;
    case 1: utf16_to_utf8(body, body_len, true /* BOM decides */, out, out_cap); break;
    case 2: utf16_to_utf8(body, body_len, true, out, out_cap); break;
    case 3:
    default: {
        size_t n = body_len < out_cap - 1 ? body_len : out_cap - 1;
        memcpy(out, body, n);
        out[n] = '\0';
        break;
    }
    }
}

/* WXXX (user-defined URL link frame): <encoding><description>\0[\0]<url>.
 * The description is text-encoded like any other text frame; the URL itself
 * is always ISO-8859-1 per spec (URLs are ASCII-safe), so it is copied
 * through directly rather than re-decoded. */
static void decode_wxxx_frame(const uint8_t *raw, uint32_t raw_len,
                               char *desc_out, size_t desc_cap,
                               char *url_out, size_t url_cap)
{
    desc_out[0] = '\0';
    url_out[0] = '\0';
    if (raw_len == 0) {
        return;
    }
    uint8_t enc = raw[0];
    const uint8_t *body = raw + 1;
    uint32_t body_len = raw_len - 1;
    uint32_t sep_len = (enc == 1 || enc == 2) ? 2 : 1;

    uint32_t sep_at = body_len; /* not found -> whole thing is description, empty url */
    for (uint32_t i = 0; i + sep_len <= body_len; i++) {
        bool match = true;
        for (uint32_t j = 0; j < sep_len; j++) {
            if (body[i + j] != 0x00) { match = false; break; }
        }
        if (match) { sep_at = i; break; }
    }
    /* Description is text-encoded like any other text frame - reuse the
     * decoder over raw[0..1+sep_at) so it sees the same leading encoding
     * byte it expects. */
    decode_text_frame(raw, 1 + sep_at, desc_out, desc_cap);

    if (sep_at < body_len) {
        const uint8_t *url_bytes = body + sep_at + sep_len;
        uint32_t url_len = body_len - sep_at - sep_len;
        size_t n = url_len < url_cap - 1 ? url_len : url_cap - 1;
        memcpy(url_out, url_bytes, n);
        url_out[n] = '\0';
    }
}

/* Frame layout for ID3v2.3/2.4 (the only two id3_header_valid() accepts):
 * 4-byte frame id, 4-byte size (synchsafe on 2.4, plain big-endian on 2.3),
 * 2-byte flags, then that many bytes of payload. Walks every frame in one
 * tag, filling `out` from whichever TIT2/TPE1/TALB/WXXX it finds - matches
 * tunein_nowplaying.py's _track_from_frames(). Returns true only if a title
 * or artist was actually found (an empty/PRIV-only tag, which every
 * fragment on this stream ALSO carries as a lightweight timestamp ping - see
 * seg.mp4 - is not "no track", it is just not one of the copies that
 * happens to carry the full metadata; the caller keeps scanning). */
static bool track_from_id3_tag(const uint8_t *data, uint32_t tag_start, uint32_t major,
                                uint32_t tag_end, uint32_t data_len, nowplaying_info_t *out)
{
    char artwork_390[NOWPLAYING_ART_URL_MAX] = {0};
    char artwork_640[NOWPLAYING_ART_URL_MAX] = {0};
    char title[NOWPLAYING_TITLE_MAX] = {0};
    char artist[NOWPLAYING_SUBTITLE_MAX] = {0};
    char album[NOWPLAYING_ALBUM_MAX] = {0};
    bool have_title_or_artist = false;

    uint32_t limit = tag_end < data_len ? tag_end : data_len;
    uint32_t fp = tag_start + 10;
    while (fp + 10 <= limit) {
        if (memcmp(data + fp, "\x00\x00\x00\x00", 4) == 0) {
            break; /* padding - nothing meaningful follows */
        }
        char fid[5];
        memcpy(fid, data + fp, 4);
        fid[4] = '\0';
        uint32_t fsize = (major >= 4) ? synchsafe(data + fp + 4)
                                       : ((uint32_t)data[fp + 4] << 24 | (uint32_t)data[fp + 5] << 16 |
                                          (uint32_t)data[fp + 6] << 8 | data[fp + 7]);
        uint32_t fstart = fp + 10;
        uint32_t fend = fstart + fsize;
        if (fsize == 0 || fend > data_len) {
            break;
        }
        if (strcmp(fid, "TIT2") == 0) {
            decode_text_frame(data + fstart, fsize, title, sizeof(title));
            have_title_or_artist = have_title_or_artist || title[0] != '\0';
        } else if (strcmp(fid, "TPE1") == 0) {
            decode_text_frame(data + fstart, fsize, artist, sizeof(artist));
            have_title_or_artist = have_title_or_artist || artist[0] != '\0';
        } else if (strcmp(fid, "TALB") == 0) {
            decode_text_frame(data + fstart, fsize, album, sizeof(album));
        } else if (strcmp(fid, "WXXX") == 0) {
            char desc[32];
            char url[NOWPLAYING_ART_URL_MAX];
            decode_wxxx_frame(data + fstart, fsize, desc, sizeof(desc), url, sizeof(url));
            if (strcmp(desc, "artworkURL_390x") == 0) {
                strncpy(artwork_390, url, sizeof(artwork_390) - 1);
            } else if (strcmp(desc, "artworkURL_640x") == 0) {
                strncpy(artwork_640, url, sizeof(artwork_640) - 1);
            }
        }
        fp = fend;
    }

    if (!have_title_or_artist) {
        return false;
    }

    strncpy(out->title, title, sizeof(out->title) - 1);
    out->title[sizeof(out->title) - 1] = '\0';
    /* Same precedence as tunein_nowplaying.py's display_fields(): artist,
     * falling back to album, for the subtitle line. */
    const char *subtitle_src = artist[0] != '\0' ? artist : album;
    strncpy(out->subtitle, subtitle_src, sizeof(out->subtitle) - 1);
    out->subtitle[sizeof(out->subtitle) - 1] = '\0';
    strncpy(out->album, album, sizeof(out->album) - 1);
    out->album[sizeof(out->album) - 1] = '\0';
    /* Same precedence as tunein_nowplaying.py's display_fields(): the
     * smaller "_390x" artwork is what the TuneIn web player's own
     * #playerArtwork actually renders, so it is preferred here too when
     * present, falling back to the larger "_640x" one. */
    const char *art_src = artwork_390[0] != '\0' ? artwork_390 : artwork_640;
    strncpy(out->art_url, art_src, sizeof(out->art_url) - 1);
    out->art_url[sizeof(out->art_url) - 1] = '\0';
    return true;
}

/* Scans `data` for every valid ID3v2 tag and keeps the LAST one that yields
 * a title/artist (matching tunein_nowplaying.py's own
 * `for frames in reversed(parse_id3_tags(seg_data))` - taking the tag
 * closest to the end of what was fetched, i.e. the most recent). On this
 * stream every tag within one segment's prefix is an identical repeat of
 * the same track (see seg.mp4), so which one wins rarely matters in
 * practice - this only matters on a genuine mid-buffer track change. */
static bool find_latest_track(const uint8_t *data, uint32_t data_len, nowplaying_info_t *out)
{
    bool found_any = false;
    uint32_t pos = 0;
    while (pos + 10 <= data_len) {
        /* memmem() is not in ESP-IDF's newlib by default - a plain manual
         * scan for the 3-byte magic is simpler than pulling it in for this. */
        uint32_t idx = pos;
        bool hit = false;
        for (; idx + 10 <= data_len; idx++) {
            if (data[idx] == 'I' && data[idx + 1] == 'D' && data[idx + 2] == '3') {
                hit = true;
                break;
            }
        }
        if (!hit) {
            break;
        }
        if (!id3_header_valid(data + idx)) {
            pos = idx + 3; /* false match (see id3_header_valid()'s comment) - keep scanning past it */
            continue;
        }
        uint32_t major = data[idx + 3];
        uint32_t size = synchsafe(data + idx + 6);
        uint32_t tag_end = idx + 10 + size;
        nowplaying_info_t candidate = {0};
        if (track_from_id3_tag(data, idx, major, tag_end, data_len, &candidate)) {
            *out = candidate;
            found_any = true;
        }
        pos = tag_end > idx ? tag_end : idx + 3;
    }
    return found_any;
}

/* ---- m3u8 media-playlist scan - deliberately not reusing ESP-ADF's own
 * private hls_playlist parser, same reasoning and same technique as
 * playlist_prefetch.c's parse_media_playlist() (see its own comment) - just
 * keeping the LAST segment URI found instead of every one, since that is
 * the one currently airing. Segment URIs on this stream are always
 * absolute (see tools/tunin test/variant.m3u8), so no base-URL join is
 * needed the way tunein_nowplaying.py's urllib.parse.urljoin() does. ---- */
static char *parse_last_segment_uri(char *text)
{
    char *saveptr = NULL;
    char *line = strtok_r(text, "\r\n", &saveptr);
    bool want_uri = false;
    char *last = NULL;

    while (line != NULL) {
        if (want_uri) {
            if (line[0] != '\0' && line[0] != '#') {
                char *copy = strdup(line);
                if (copy) {
                    free(last);
                    last = copy;
                }
            }
            want_uri = false;
        } else if (strncmp(line, "#EXTINF", 7) == 0) {
            want_uri = true;
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }
    return last;
}

/* One GET, optionally Range-limited, into `buf` (capacity `buf_cap`,
 * NUL-terminated on return so it can double as a C string for the playlist
 * case). Same open/fetch_headers/bounded-read-loop shape as
 * playlist_prefetch_task() - see that function's comment for why this isn't
 * esp_http_client_perform() with an event handler (tunein_control.c's own
 * http_get() style): a fixed-size destination buffer needs a bounded read
 * loop, not an unbounded event-driven append. */
static esp_err_t fetch_url(const char *url, int range_bytes, uint8_t *buf, size_t buf_cap, int *out_len)
{
    *out_len = 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = NOWPLAYING_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        .keep_alive_enable = false,
        .crt_bundle_attach = s_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed (OOM?)");
        return ESP_ERR_NO_MEM;
    }
    if (range_bytes > 0) {
        char range_hdr[32];
        snprintf(range_hdr, sizeof(range_hdr), "bytes=0-%d", range_bytes - 1);
        esp_http_client_set_header(client, "Range", range_hdr);
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    /* 206 = Partial Content, the expected reply to our Range request; 200 =
     * the server ignored Range and is sending the whole body anyway (still
     * fine - the bounded read loop below just takes the first buf_cap-1
     * bytes of it, which is all this module ever needed in the first
     * place). Anything else (403/404/expired signed URL, ...) is a real
     * failure. */
    if (status != 200 && status != 206) {
        ESP_LOGW(TAG, "GET %s -> HTTP %d", url, status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int total = 0;
    while ((size_t)total < buf_cap - 1) {
        int r = esp_http_client_read(client, (char *)buf + total, (int)(buf_cap - 1 - (size_t)total));
        if (r <= 0) {
            break;
        }
        total += r;
    }
    buf[total] = '\0';
    *out_len = total;

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}

static void nowplaying_task(void *arg)
{
    char *playlist_url = (char *)arg;
    uint8_t *buf = heap_caps_malloc(NOWPLAYING_FETCH_BUF_BYTES, MALLOC_CAP_8BIT);
    char *segment_url = NULL;
    nowplaying_info_t track = {0};
    bool have_track = false;

    if (!buf) {
        ESP_LOGW(TAG, "OOM allocating %d-byte fetch buffer; skipping this poll",
                 NOWPLAYING_FETCH_BUF_BYTES);
        goto done;
    }

    {
        int playlist_len = 0;
        esp_err_t err = fetch_url(playlist_url, 0, buf, NOWPLAYING_FETCH_BUF_BYTES, &playlist_len);
        if (err != ESP_OK || playlist_len == 0) {
            ESP_LOGW(TAG, "Could not fetch now-playing playlist (%s); skipping this poll",
                     esp_err_to_name(err));
            goto done;
        }
        segment_url = parse_last_segment_uri((char *)buf);
        if (!segment_url) {
            ESP_LOGW(TAG, "No segment URI found in now-playing playlist; skipping this poll");
            goto done;
        }
    }

    {
        int seg_len = 0;
        esp_err_t err = fetch_url(segment_url, NOWPLAYING_FETCH_BUF_BYTES - 1, buf,
                                   NOWPLAYING_FETCH_BUF_BYTES, &seg_len);
        if (err != ESP_OK || seg_len == 0) {
            ESP_LOGW(TAG, "Could not fetch segment prefix for now-playing (%s); skipping this poll",
                     esp_err_to_name(err));
            goto done;
        }
        have_track = find_latest_track(buf, (uint32_t)seg_len, &track);
        if (!have_track) {
            ESP_LOGD(TAG, "No ID3 track metadata found in this segment's prefix; keeping last known track");
        }
    }

done:
    if (have_track) {
        track.valid = true;
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            s_current = track;
            s_updated_tick = xTaskGetTickCount();
            xSemaphoreGive(s_mutex);
        }
        ESP_LOGI(TAG, "Now playing: '%s' - '%s' (album '%s')%s",
                 track.title, track.subtitle, track.album,
                 track.art_url[0] ? "" : " [no artwork in this tag]");
    }

    ESP_LOGD(TAG, "nowplaying task stack high-water mark: %u bytes",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));

    free(segment_url);
    if (buf) {
        heap_caps_free(buf);
    }
    free(playlist_url);
    s_in_flight = false;
    vTaskDelete(NULL);
}

esp_err_t nowplaying_poll_start(const char *hls_media_playlist_url)
{
    if (!s_mutex || !hls_media_playlist_url) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_in_flight) {
        return ESP_ERR_INVALID_STATE;
    }
    char *url_copy = strdup(hls_media_playlist_url);
    if (!url_copy) {
        return ESP_ERR_NO_MEM;
    }

    s_in_flight = true; /* set before create - same reasoning as playlist_prefetch_start() */
    BaseType_t created = xTaskCreate(nowplaying_task, "nowplaying",
                                      NOWPLAYING_TASK_STACK_BYTES,
                                      url_copy, NOWPLAYING_TASK_PRIORITY, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed (free heap=%" PRIu32 ")", esp_get_free_heap_size());
        free(url_copy);
        s_in_flight = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool nowplaying_is_in_flight(void)
{
    return s_in_flight;
}

void nowplaying_get_current(nowplaying_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_mutex) {
        return;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        *out = s_current;
        TickType_t now = xTaskGetTickCount();
        out->age_ms = out->valid ? (uint32_t)((now - s_updated_tick) * portTICK_PERIOD_MS) : 0;
        xSemaphoreGive(s_mutex);
    }
}

void nowplaying_reset(void)
{
    if (!s_mutex) {
        return;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        memset(&s_current, 0, sizeof(s_current));
        s_updated_tick = xTaskGetTickCount();
        xSemaphoreGive(s_mutex);
    }
}

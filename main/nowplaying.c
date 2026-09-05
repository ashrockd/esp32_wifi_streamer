#include "nowplaying.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "NOWPLAYING";

static SemaphoreHandle_t s_mutex = NULL;

/* Published result + all parsing scratch below are PSRAM-backed pointers,
 * not plain static arrays - this board has 8MB of PSRAM (ESP32-S3 N16R8),
 * so several KB of text scratch has no business sitting in the same tight
 * internal/DMA-capable RAM pool this project has crashed over before (see
 * RADIO_DMA_FREE_CRITICAL_BYTES in app_config.h). Allocated ONCE, up front,
 * in nowplaying_init() - same "claim it while the heap is clean" convention
 * fmp4_bridge_init()/playlist_prefetch.c already use for their own buffers
 * - and never freed (this module lives for the process's whole lifetime,
 * same as those). Every function below checks for NULL before touching any
 * of these (allocation failure should never happen on this board with 8MB
 * free at boot, but degrades to a safe no-op rather than crashing if it
 * somehow ever did).
 *
 * s_current/s_updated_tick: the PUBLISHED result, protected by s_mutex -
 * written only by nowplaying_ingest_id3_tag() (fmp4_bridge.c's element
 * task), read only by nowplaying_get_current() (main.c's periodic status
 * log, a different task). age_ms in nowplaying_info_t is NOT stored here;
 * it is derived from s_updated_tick at read time.
 *
 * Everything else (s_scratch*, s_ingest_result): parsing scratch, touched
 * ONLY from nowplaying_ingest_id3_tag()'s own call chain - see this file's
 * header comment on the threading model for why plain (non-mutex'd) shared
 * state is safe here: a ~700-byte nowplaying_info_t plus a ~350-byte WXXX
 * decode scratch would eat a large fraction of fmp4_bridge's own small
 * element task stack as ordinary locals, so this state has to live
 * somewhere other than that call chain's stack regardless of which RAM
 * pool it's in - PSRAM is simply the strictly better choice of the two
 * once it's being pulled off the stack anyway. s_ingest_result is the
 * final per-tag result find_latest_track()/track_from_id3_tag() build up
 * before it gets copied into s_current (under the mutex) - kept separate
 * from s_current itself so a reader can never observe a partially-built
 * candidate. */
static nowplaying_info_t *s_current;
static TickType_t s_updated_tick;

static nowplaying_info_t *s_scratch;
static char *s_scratch_artist;
static char *s_scratch_artwork_390;
static char *s_scratch_artwork_640;
static char *s_scratch_wxxx_desc;
static char *s_scratch_wxxx_url;
static nowplaying_info_t *s_ingest_result;

/* ICY path's own result-staging scratch - separate from s_ingest_result
 * above on purpose, see nowplaying_ingest_icy_title()'s doc comment
 * (nowplaying.h) for why: different call chain/task than the ID3 path,
 * mutually exclusive in practice today but kept independent rather than
 * coupling the two ingest paths to a shared buffer. */
static nowplaying_info_t *s_icy_result;

#define NOWPLAYING_WXXX_DESC_MAX 32

static void *psram_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        /* Should never happen on this board (8MB PSRAM, this is a few KB
         * total) - falls back to internal RAM rather than leaving this
         * module non-functional over what would be a much bigger problem
         * elsewhere if it ever actually triggered. */
        ESP_LOGW(TAG, "PSRAM allocation of %u bytes failed; falling back to internal RAM",
                 (unsigned)size);
        p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return p;
}

esp_err_t nowplaying_init(void)
{
    if (s_mutex != NULL) {
        return ESP_OK; /* already initialized */
    }
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "OOM creating result mutex");
        return ESP_ERR_NO_MEM;
    }

    s_current = psram_alloc(sizeof(*s_current));
    s_scratch = psram_alloc(sizeof(*s_scratch));
    s_scratch_artist = psram_alloc(NOWPLAYING_SUBTITLE_MAX);
    s_scratch_artwork_390 = psram_alloc(NOWPLAYING_ART_URL_MAX);
    s_scratch_artwork_640 = psram_alloc(NOWPLAYING_ART_URL_MAX);
    s_scratch_wxxx_desc = psram_alloc(NOWPLAYING_WXXX_DESC_MAX);
    s_scratch_wxxx_url = psram_alloc(NOWPLAYING_ART_URL_MAX);
    s_ingest_result = psram_alloc(sizeof(*s_ingest_result));
    s_icy_result = psram_alloc(sizeof(*s_icy_result));

    if (!s_current || !s_scratch || !s_scratch_artist || !s_scratch_artwork_390 ||
        !s_scratch_artwork_640 || !s_scratch_wxxx_desc || !s_scratch_wxxx_url || !s_ingest_result ||
        !s_icy_result) {
        ESP_LOGE(TAG, "OOM allocating now-playing buffers - now-playing display will stay empty "
                 "(nothing else is affected)");
        return ESP_ERR_NO_MEM;
    }
    memset(s_current, 0, sizeof(*s_current));
    ESP_LOGI(TAG, "Initialized (PSRAM-backed)");
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
 * tag, filling the PSRAM-backed scratch fields (see their own comment for
 * why not the stack) from whichever TIT2/TPE1/TALB/WXXX it finds - matches
 * tunein_nowplaying.py's _track_from_frames(). Returns true only if a title
 * or artist was actually found (an empty/PRIV-only tag, which every
 * fragment on this stream ALSO carries as a lightweight timestamp ping - see
 * seg.mp4 - is not "no track", it is just not one of the copies that
 * happens to carry the full metadata; the caller keeps scanning). */
static bool track_from_id3_tag(const uint8_t *data, uint32_t tag_start, uint32_t major,
                                uint32_t tag_end, uint32_t data_len, nowplaying_info_t *out)
{
    s_scratch->title[0] = '\0';
    s_scratch_artist[0] = '\0';
    s_scratch->album[0] = '\0';
    s_scratch_artwork_390[0] = '\0';
    s_scratch_artwork_640[0] = '\0';
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
            decode_text_frame(data + fstart, fsize, s_scratch->title, sizeof(s_scratch->title));
            have_title_or_artist = have_title_or_artist || s_scratch->title[0] != '\0';
        } else if (strcmp(fid, "TPE1") == 0) {
            decode_text_frame(data + fstart, fsize, s_scratch_artist, NOWPLAYING_SUBTITLE_MAX);
            have_title_or_artist = have_title_or_artist || s_scratch_artist[0] != '\0';
        } else if (strcmp(fid, "TALB") == 0) {
            decode_text_frame(data + fstart, fsize, s_scratch->album, sizeof(s_scratch->album));
        } else if (strcmp(fid, "WXXX") == 0) {
            decode_wxxx_frame(data + fstart, fsize, s_scratch_wxxx_desc, NOWPLAYING_WXXX_DESC_MAX,
                               s_scratch_wxxx_url, NOWPLAYING_ART_URL_MAX);
            if (strcmp(s_scratch_wxxx_desc, "artworkURL_390x") == 0) {
                strncpy(s_scratch_artwork_390, s_scratch_wxxx_url, NOWPLAYING_ART_URL_MAX - 1);
            } else if (strcmp(s_scratch_wxxx_desc, "artworkURL_640x") == 0) {
                strncpy(s_scratch_artwork_640, s_scratch_wxxx_url, NOWPLAYING_ART_URL_MAX - 1);
            }
        }
        fp = fend;
    }

    if (!have_title_or_artist) {
        return false;
    }

    strncpy(out->title, s_scratch->title, sizeof(out->title) - 1);
    out->title[sizeof(out->title) - 1] = '\0';
    /* Same precedence as tunein_nowplaying.py's display_fields(): artist,
     * falling back to album, for the subtitle line. */
    const char *subtitle_src = s_scratch_artist[0] != '\0' ? s_scratch_artist : s_scratch->album;
    strncpy(out->subtitle, subtitle_src, sizeof(out->subtitle) - 1);
    out->subtitle[sizeof(out->subtitle) - 1] = '\0';
    strncpy(out->album, s_scratch->album, sizeof(out->album) - 1);
    out->album[sizeof(out->album) - 1] = '\0';
    /* Same precedence as tunein_nowplaying.py's display_fields(): the
     * smaller "_390x" artwork is what the TuneIn web player's own
     * #playerArtwork actually renders, so it is preferred here too when
     * present, falling back to the larger "_640x" one. */
    const char *art_src = s_scratch_artwork_390[0] != '\0' ? s_scratch_artwork_390 : s_scratch_artwork_640;
    strncpy(out->art_url, art_src, sizeof(out->art_url) - 1);
    out->art_url[sizeof(out->art_url) - 1] = '\0';
    return true;
}

/* Scans `data` for every valid ID3v2 tag and keeps the LAST one that yields
 * a title/artist (matching tunein_nowplaying.py's own
 * `for frames in reversed(parse_id3_tags(seg_data))` - taking the tag
 * closest to the end of what was fetched, i.e. the most recent). An emsg
 * box on this stream carries exactly one tag in practice, so this rarely
 * has more than one candidate to choose from at all - kept general anyway
 * since nothing about it assumes otherwise. */
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
        if (track_from_id3_tag(data, idx, major, tag_end, data_len, out)) {
            found_any = true;
        }
        pos = tag_end > idx ? tag_end : idx + 3;
    }
    return found_any;
}

void nowplaying_ingest_id3_tag(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return;
    }
    if (!s_scratch || !s_scratch_artist || !s_scratch_artwork_390 || !s_scratch_artwork_640 ||
        !s_scratch_wxxx_desc || !s_scratch_wxxx_url || !s_ingest_result) {
        return; /* nowplaying_init() never called, or its PSRAM allocation failed */
    }
    memset(s_ingest_result, 0, sizeof(*s_ingest_result));
    if (!find_latest_track(data, (uint32_t)len, s_ingest_result)) {
        ESP_LOGD(TAG, "emsg box carried no title/artist (a timestamp ping, most likely) - "
                 "keeping last known track");
        return;
    }
    s_ingest_result->valid = true;

    if (!s_mutex || !s_current) {
        return;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        *s_current = *s_ingest_result;
        s_updated_tick = xTaskGetTickCount();
        xSemaphoreGive(s_mutex);
    }
    ESP_LOGI(TAG, "Now playing: '%s' - '%s' (album '%s')%s",
             s_ingest_result->title, s_ingest_result->subtitle, s_ingest_result->album,
             s_ingest_result->art_url[0] ? "" : " [no artwork in this tag]");
}

/* Trims trailing ASCII spaces in place - cosmetic only, for 181.fm titles
 * observed with a trailing space before the closing quote (e.g.
 * "Turn Me On (Radio Edit) "). */
static void rtrim_spaces(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && s[len - 1] == ' ') {
        s[--len] = '\0';
    }
}

void nowplaying_ingest_icy_title(const char *stream_title, const char *art_url)
{
    if (!stream_title || stream_title[0] == '\0') {
        return; /* empty metadata block - a timing ping, not a real update (see this file's header) */
    }
    if (!s_icy_result) {
        return; /* nowplaying_init() never called, or its PSRAM allocation failed */
    }
    memset(s_icy_result, 0, sizeof(*s_icy_result));

    /* Same delimiter fallback order as 181.fm's own web player JS
     * (site.4.6.15.js's process_song()) - see this function's declaration
     * comment (nowplaying.h) for the full reasoning. */
    static const char *const delims[] = { " - ", "-", " / " };
    const char *sep = NULL;
    size_t delim_len = 0;
    for (size_t d = 0; d < sizeof(delims) / sizeof(delims[0]); d++) {
        const char *p = strstr(stream_title, delims[d]);
        if (p) {
            sep = p;
            delim_len = strlen(delims[d]);
            break;
        }
    }

    if (sep) {
        size_t artist_len = (size_t)(sep - stream_title);
        size_t n = artist_len < sizeof(s_icy_result->subtitle) - 1 ? artist_len : sizeof(s_icy_result->subtitle) - 1;
        memcpy(s_icy_result->subtitle, stream_title, n);
        s_icy_result->subtitle[n] = '\0';
        strncpy(s_icy_result->title, sep + delim_len, sizeof(s_icy_result->title) - 1);
        s_icy_result->title[sizeof(s_icy_result->title) - 1] = '\0';
    } else {
        /* No recognized delimiter - whole string is the title, empty artist,
         * same fallback the source JS uses. */
        strncpy(s_icy_result->title, stream_title, sizeof(s_icy_result->title) - 1);
        s_icy_result->title[sizeof(s_icy_result->title) - 1] = '\0';
    }
    rtrim_spaces(s_icy_result->title);
    rtrim_spaces(s_icy_result->subtitle);

    if (art_url) {
        strncpy(s_icy_result->art_url, art_url, sizeof(s_icy_result->art_url) - 1);
        s_icy_result->art_url[sizeof(s_icy_result->art_url) - 1] = '\0';
    }
    /* No separate album field on the ICY path - left empty (already zeroed
     * by the memset above). */
    s_icy_result->valid = true;

    if (!s_mutex || !s_current) {
        return;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        *s_current = *s_icy_result;
        s_updated_tick = xTaskGetTickCount();
        xSemaphoreGive(s_mutex);
    }
    ESP_LOGI(TAG, "Now playing (ICY): '%s' - '%s'%s",
             s_icy_result->title, s_icy_result->subtitle,
             s_icy_result->art_url[0] ? "" : " [no artwork in this tag]");
}

void nowplaying_get_current(nowplaying_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_mutex || !s_current) {
        return;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        *out = *s_current;
        TickType_t now = xTaskGetTickCount();
        out->age_ms = out->valid ? (uint32_t)((now - s_updated_tick) * portTICK_PERIOD_MS) : 0;
        xSemaphoreGive(s_mutex);
    }
}

void nowplaying_reset(void)
{
    if (!s_mutex || !s_current) {
        return;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        memset(s_current, 0, sizeof(*s_current));
        s_updated_tick = xTaskGetTickCount();
        xSemaphoreGive(s_mutex);
    }
}

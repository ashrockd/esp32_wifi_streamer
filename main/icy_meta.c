#include "icy_meta.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "nowplaying.h"

static const char *TAG = "ICYMETA";

/* A metadata block's length byte is a single byte * 16, so 255*16 is the
 * hard ceiling regardless of what any server actually sends - see
 * icy_meta_strip_and_forward()'s defensive check below. */
#define ICY_META_MAX_BLOCK_BYTES (255 * 16)

/* StreamTitle values observed on this stream ("Artist - Title (Radio Edit) ")
 * run under 60 bytes; this is a combined artist+title field (unlike
 * nowplaying.h's separate NOWPLAYING_TITLE_MAX/SUBTITLE_MAX), so sized well
 * above what nowplaying_ingest_icy_title() will actually keep after its own
 * split. StreamArtwork is empty on this station today (see this file's
 * header comment) but parsed anyway in case 181.fm ever populates it -
 * NOWPLAYING_ART_URL_MAX-sized to match what nowplaying.c can actually use. */
#define ICY_META_TITLE_MAX   256
#define ICY_META_ARTWORK_MAX NOWPLAYING_ART_URL_MAX

/* Byte-position state machine, persistent across icy_meta_strip_and_forward()
 * calls for one connection - see icy_meta.h's header comment on why plain
 * static (not stack, not mutex'd) is correct here: this is only ever called
 * from http_reader's own element task, never concurrently with itself. */
static int  s_metaint;            /* 0 = disabled/passthrough */
static int  s_audio_remaining;    /* bytes of audio left before the next length byte */
static bool s_in_meta;            /* currently copying a metadata block's payload */
static int  s_meta_len;           /* current block's payload length, 0..ICY_META_MAX_BLOCK_BYTES */
static int  s_meta_have;          /* bytes of the current block copied into s_meta_buf so far */

/* PSRAM-backed (this board has 8MB - see icy_meta.h's own comment and
 * nowplaying.c's identical reasoning for buffers only ever touched from one
 * task). Allocated once, up front, in icy_meta_init(); NULL if that
 * allocation ever failed, in which case stripping still works correctly
 * (the byte-position math above does not depend on these at all) but no
 * title/artist is ever extracted - see the NULL checks below. */
static char *s_meta_buf;     /* +1 for a NUL terminator, so it can be strstr()'d as a C string */
static char *s_title_buf;
static char *s_artwork_buf;

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

esp_err_t icy_meta_init(void)
{
    if (s_meta_buf && s_title_buf && s_artwork_buf) {
        return ESP_OK; /* already initialized */
    }
    s_meta_buf    = psram_alloc(ICY_META_MAX_BLOCK_BYTES + 1);
    s_title_buf   = psram_alloc(ICY_META_TITLE_MAX);
    s_artwork_buf = psram_alloc(ICY_META_ARTWORK_MAX);

    if (!s_meta_buf || !s_title_buf || !s_artwork_buf) {
        ESP_LOGE(TAG, "OOM allocating ICY metadata scratch - The Vibe of Vegas will still play, "
                 "just without a title/artist display");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Initialized (PSRAM-backed)");
    return ESP_OK;
}

void icy_meta_reset(int metaint)
{
    s_metaint = metaint > 0 ? metaint : 0;
    s_audio_remaining = s_metaint;
    s_in_meta = false;
    s_meta_len = 0;
    s_meta_have = 0;
    ESP_LOGI(TAG, "Armed for a new connection: metaint=%d%s", s_metaint,
             s_metaint > 0 ? "" : " (disabled - stream passes through unmodified)");
}

/* Pulls one key='value' field out of a NUL-terminated metadata block body,
 * e.g. icy_extract_field(block, "StreamTitle", ...) on
 * "StreamTitle='Foo - Bar';StreamUrl='';StreamArtwork='';" yields "Foo - Bar".
 * Returns false (and leaves *out empty) if the key is missing or its value
 * is empty - both normal: most blocks on this stream are empty pings between
 * real title changes (the same "timestamp ping" pattern nowplaying.h documents
 * for the CMAF/emsg side), and StreamArtwork is simply never populated by
 * this station today. */
static bool icy_extract_field(const char *block, const char *key, char *out, size_t out_cap)
{
    out[0] = '\0';
    char needle[24];
    int n = snprintf(needle, sizeof(needle), "%s='", key);
    if (n < 0 || (size_t)n >= sizeof(needle)) {
        return false; /* key too long for `needle` - never happens for the two literal keys used below */
    }
    const char *start = strstr(block, needle);
    if (!start) {
        return false;
    }
    start += n;
    const char *end = strstr(start, "';");
    if (!end) {
        return false; /* truncated block (should not happen - meta_len bytes were fully copied) */
    }
    size_t value_len = (size_t)(end - start);
    if (value_len >= out_cap) {
        value_len = out_cap - 1;
    }
    memcpy(out, start, value_len);
    out[value_len] = '\0';
    return value_len > 0;
}

/* Called once s_meta_buf holds exactly s_meta_len freshly-copied bytes of
 * one complete metadata block's payload. */
static void handle_complete_meta_block(void)
{
    s_meta_buf[s_meta_len] = '\0';
    if (s_meta_len == 0) {
        return; /* the common case: no metadata change since the last block */
    }
    if (!icy_extract_field(s_meta_buf, "StreamTitle", s_title_buf, ICY_META_TITLE_MAX)) {
        ESP_LOGD(TAG, "Metadata block carried no (non-empty) StreamTitle - keeping last known track");
        return;
    }
    /* Best-effort; empty on this station as of 2026-09-05 (see this file's
     * header comment) - nowplaying_ingest_icy_title() treats an empty
     * artwork URL as "no artwork", same as a CMAF tag with no WXXX frame. */
    icy_extract_field(s_meta_buf, "StreamArtwork", s_artwork_buf, ICY_META_ARTWORK_MAX);
    nowplaying_ingest_icy_title(s_title_buf, s_artwork_buf);
}

int icy_meta_strip_and_forward(char *buffer, int len, ringbuf_handle_t out_rb, TickType_t ticks_to_wait)
{
    if (len <= 0) {
        return len;
    }
    if (s_metaint <= 0) {
        /* Disabled (icy_meta_reset() never called, or called with 0) - pure
         * passthrough, identical to what http_reader's own default output
         * behavior (no write_cb at all) would have done. */
        return rb_write(out_rb, buffer, len, ticks_to_wait);
    }

    int i = 0;
    while (i < len) {
        if (s_in_meta) {
            int want = s_meta_len - s_meta_have;
            int take = (len - i) < want ? (len - i) : want;
            if (s_meta_buf) {
                memcpy(s_meta_buf + s_meta_have, buffer + i, (size_t)take);
            }
            s_meta_have += take;
            i += take;
            if (s_meta_have >= s_meta_len) {
                if (s_meta_buf) {
                    handle_complete_meta_block();
                }
                s_in_meta = false;
                s_audio_remaining = s_metaint;
            }
            continue;
        }

        if (s_audio_remaining > 0) {
            int take = (len - i) < s_audio_remaining ? (len - i) : s_audio_remaining;
            int wrote = rb_write(out_rb, buffer + i, take, ticks_to_wait);
            if (wrote <= 0) {
                /* Abort/fail/timeout - propagate exactly like a direct
                 * rb_write() tap (led_viz_write_cb) already would. */
                return wrote;
            }
            s_audio_remaining -= wrote;
            i += wrote;
            if (wrote < take) {
                /* Ring buffer only took part of it (e.g. timed out mid-write).
                 * Same as ADF's own http_stream: whatever's left of `buffer`
                 * this call is not retried - the next call reads a fresh
                 * chunk from the connection's current position. */
                return i;
            }
            continue;
        }

        /* s_audio_remaining == 0: the next byte is this cycle's metadata
         * LENGTH byte (block length = that byte * 16, per the ICY/Shoutcast
         * protocol). */
        uint8_t length_byte = (uint8_t)buffer[i];
        i += 1;
        s_meta_len = (int)length_byte * 16;
        s_meta_have = 0;
        if (s_meta_len == 0) {
            s_audio_remaining = s_metaint; /* nothing to read this cycle */
        } else if (s_meta_len > ICY_META_MAX_BLOCK_BYTES) {
            /* Cannot actually happen - length_byte is one byte, so
             * length_byte*16 is at most 255*16 = ICY_META_MAX_BLOCK_BYTES -
             * defensive only, in case that invariant is ever changed above
             * without updating this check. */
            ESP_LOGE(TAG, "Impossible metadata length %d - dropping this cycle's metadata "
                     "and resuming audio passthrough", s_meta_len);
            s_meta_len = 0;
            s_audio_remaining = s_metaint;
        } else {
            s_in_meta = true;
        }
    }
    return len;
}

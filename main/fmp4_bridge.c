#include "fmp4_bridge.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "audio_element.h"
#include "audio_mem.h"

static const char *TAG = "FMP4_BRIDGE";

/* Caps runaway allocation from a corrupt or unexpected stream - the real
 * Apple/TuneIn stream's moof boxes run ~9KB (751 samples/fragment), so even
 * the ORIGINAL 16KB/2048 here already had ~1.7x headroom.
 *
 * 2026-09-04: widened further (16KB->64KB, 2048->4096 samples) as a general
 * fail-safe margin pass. The original sizing was deliberately chosen "without
 * needing PSRAM" (see the history below) - this project has since migrated
 * to an ESP32-S3 with 8MB of PSRAM, so that constraint no longer applies, and
 * there is no reason to run this cap close to observed reality when 8MB is
 * available. Still a hard cap, not unbounded - a station with a genuinely
 * pathological moof (corrupt stream, or a fragmentation scheme wildly
 * different from this project's one confirmed working stream) fails cleanly
 * (ESP_FAIL, logged) rather than accepting an arbitrarily large allocation. */
#define FMP4_MAX_MOOF_BYTES     (64 * 1024)
#define FMP4_MAX_SAMPLES        4096
#define FMP4_IN_CHUNK_BYTES     1536

/* Claimed up front in fmp4_bridge_init(), NOT lazily on the first fragment.
 *
 * Timing is the whole point. Element init runs while the heap is still clean
 * (largest free block ~98KB); the first moof does not arrive until after
 * esp_aac_dec_open() has taken its measured 51564 bytes and mbedTLS has
 * taken its static 16+4KB, by which time a 9KB contiguous request fails:
 *     E FMP4_BRIDGE: OOM allocating 9096 bytes for moof (have 0)
 * Both buffers still grow on demand if a stream ever needs more; these are
 * just the sizes actually observed on this stream (moof 9092-9104 bytes,
 * 750-751 samples) plus headroom - widened 2026-09-04 (12KB->16KB samples
 * 1024->2048) alongside the MAX_* caps above, same PSRAM-headroom reasoning;
 * this only affects heap layout at element-init time (see the OOM history
 * just above), not the FMP4_MAX_* caps' role of rejecting a pathological
 * stream outright. */
#define FMP4_PREALLOC_MOOF_BYTES  (16 * 1024)
#define FMP4_PREALLOC_SAMPLES     2048

/* moov parsing (one-shot, on the small init segment) is capped separately -
 * real init segments are a few hundred bytes. */
#define FMP4_MAX_INIT_BYTES     (8 * 1024)

/* Bring-up diagnostic (2026-08-20): the AAC decoder reports "This audio is
 * RAW AAC" / "Failed to initialize" - i.e. it does not see the ADTS syncword
 * this element is supposed to be synthesizing. Nothing in this element logs
 * an error on that path, so this traces what actually flows: the first bytes
 * in, every box header parsed, and the first ADTS header emitted. Bounded to
 * the first few events so it can never become the kind of unbounded serial
 * spam that starved the watchdog elsewhere in this project. Set to 0 once
 * the ADTS/decoder handshake is confirmed working. */
#ifndef FMP4_DEBUG_TRACE
#define FMP4_DEBUG_TRACE 1
#endif
#define FMP4_TRACE_MAX_BOXES    12

typedef enum {
    FMP4_ST_BOX_HEADER = 0, /* accumulating the 8-byte size+type box header */
    FMP4_ST_BOX_BUFFER,     /* fully buffering a box we parse (moof) */
    FMP4_ST_BOX_SKIP,       /* discarding a box we don't care about */
    FMP4_ST_MDAT_SAMPLE,    /* inside mdat, forwarding the current sample */
} fmp4_state_t;

typedef struct {
    /* AudioSpecificConfig, primed via fmp4_bridge_set_init_segment() */
    bool    asc_valid;
    uint8_t adts_profile;      /* audioObjectType - 1 */
    uint8_t adts_freq_index;
    uint8_t adts_channel_cfg;

    fmp4_state_t state;

    /* box header scratch */
    uint8_t hdr_buf[8];
    int     hdr_have;

    /* generic box buffering (moof). Grow-only and REUSED across fragments -
     * see the note on sample_cap below; a moof is ~9KB and arrives every
     * ~16s, so malloc/free-ing it per fragment is pure heap churn. */
    uint8_t *box_buf;
    uint32_t box_cap;
    uint32_t box_want;
    uint32_t box_have;

    /* generic box skipping (styp/sidx/emsg/free/ftyp-repeat/...) */
    uint32_t skip_remaining;

    /* Current fragment's sample table, built by parsing moof/traf/trun.
     * sample_cap tracks the allocation so it can be reused across fragments
     * instead of freed and re-allocated every time: on this stream every
     * fragment carries ~751 samples (~3KB), and that repeated alloc/free of
     * a mid-size block - alongside the ~9KB moof buffer and mbedTLS's ~17KB
     * record buffer - is exactly the fragmentation pattern that produced
     * "OOM allocating 751 sample sizes" on real hardware while tens of KB
     * were still nominally free. Grow-only, never shrunk. */
    uint32_t *sample_sizes;
    int       sample_cap;
    int       sample_count;
    int       sample_index;
    uint32_t  sample_bytes_left;
    bool      sample_need_header;
    uint32_t  mdat_remaining;

    /* tfhd default, used when trun carries no explicit per-sample size */
    bool     have_default_sample_size;
    uint32_t default_sample_size;

    uint8_t in_scratch[FMP4_IN_CHUNK_BYTES];

#if FMP4_DEBUG_TRACE
    bool trace_logged_first_input;
    bool trace_logged_first_adts;
    int  trace_boxes_seen;
    uint32_t trace_total_out;
#endif
} fmp4_bridge_t;

#if FMP4_DEBUG_TRACE
static void fmp4_trace_hex(const char *label, const uint8_t *data, int len)
{
    char hex[3 * 24 + 1];
    int n = len < 24 ? len : 24;
    int used = 0;
    for (int i = 0; i < n; i++) {
        used += snprintf(hex + used, sizeof(hex) - used, "%02x ", data[i]);
    }
    ESP_LOGI(TAG, "TRACE %s (%d bytes): %s", label, len, hex);
}
#endif

static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Per-fragment state reset. Deliberately does NOT free sample_sizes - that
 * buffer is retained and reused for the next fragment (see sample_cap);
 * fmp4_bridge_destroy() is what actually releases it. */
static void fmp4_reset_fragment(fmp4_bridge_t *b)
{
    b->sample_count = 0;
    b->sample_index = 0;
    b->sample_bytes_left = 0;
    b->sample_need_header = false;
    b->mdat_remaining = 0;
    b->have_default_sample_size = false;
    b->default_sample_size = 0;
}

/* ---- Init-segment (moov) parsing: find stsd/mp4a/esds and pull the 2-byte
 * AAC AudioSpecificConfig out of it. Runs once, synchronously, over a small
 * fully-buffered blob - not part of the streaming state machine. ---- */

static bool box_find(const uint8_t *data, uint32_t len, const char *want,
                      const uint8_t **out_body, uint32_t *out_body_len)
{
    uint32_t i = 0;
    while (i + 8 <= len) {
        uint32_t size = be32(data + i);
        if (size == 0 || size == 1) {
            /* ISO/IEC 14496-12 sentinel sizes this parser does not support:
             * 0 = box extends to the end of its parent, 1 = a 64-bit
             * largesize follows the header (same stance fmp4_bridge_feed()
             * takes on the streaming path below). The plain `size < 8` check
             * right after this would also reject both values (0 and 1 are
             * both < 8), so behavior is unchanged - this just names the
             * actual reason instead of looking like generic truncation. */
            ESP_LOGE(TAG, "Box '%.4s' uses an unsupported 64-bit-largesize/extends-to-end size (%u); not parsed",
                     (const char *)(data + i + 4), (unsigned)size);
            return false;
        }
        if (size < 8 || i + size > len) {
            return false;
        }
        if (memcmp(data + i + 4, want, 4) == 0) {
            *out_body = data + i + 8;
            *out_body_len = size - 8;
            return true;
        }
        i += size;
    }
    return false;
}

static bool parse_audio_specific_config(const uint8_t *asc, uint32_t asc_len,
                                         uint8_t *object_type, uint8_t *freq_index, uint8_t *chan_cfg)
{
    if (asc_len < 2) {
        return false;
    }
    uint16_t bits = ((uint16_t)asc[0] << 8) | asc[1];
    *object_type = (bits >> 11) & 0x1F;  /* 5 bits */
    *freq_index  = (bits >> 7) & 0x0F;   /* 4 bits */
    *chan_cfg    = (bits >> 3) & 0x0F;   /* 4 bits */
    if (*freq_index == 0x0F) {
        ESP_LOGE(TAG, "Explicit (non-table) sample rate in AudioSpecificConfig is not supported");
        return false;
    }
    return true;
}

esp_err_t fmp4_bridge_set_init_segment(audio_element_handle_t self, const uint8_t *data, size_t len)
{
    fmp4_bridge_t *b = (fmp4_bridge_t *)audio_element_getdata(self);
    if (!b || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > FMP4_MAX_INIT_BYTES) {
        ESP_LOGE(TAG, "Init segment too large (%u bytes, max %d)", (unsigned)len, FMP4_MAX_INIT_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *moov = NULL, *trak = NULL, *mdia = NULL, *minf = NULL, *stbl = NULL, *stsd = NULL;
    uint32_t moov_len = 0, trak_len = 0, mdia_len = 0, minf_len = 0, stbl_len = 0, stsd_len = 0;

    if (!box_find(data, (uint32_t)len, "moov", &moov, &moov_len) ||
        !box_find(moov, moov_len, "trak", &trak, &trak_len) ||
        !box_find(trak, trak_len, "mdia", &mdia, &mdia_len) ||
        !box_find(mdia, mdia_len, "minf", &minf, &minf_len) ||
        !box_find(minf, minf_len, "stbl", &stbl, &stbl_len) ||
        !box_find(stbl, stbl_len, "stsd", &stsd, &stsd_len)) {
        ESP_LOGE(TAG, "Could not find moov/trak/mdia/minf/stbl/stsd in init segment");
        return ESP_ERR_NOT_FOUND;
    }

    /* stsd: version(1)+flags(3)+entry_count(4), then SampleEntry boxes */
    if (stsd_len < 8) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t *entry = stsd + 8;
    uint32_t entry_len = stsd_len - 8;
    if (entry_len < 8) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t entry_size = be32(entry);
    /* AudioSampleEntry fixed header is 28 bytes (reserved/data_ref_index/
     * reserved/channelcount/samplesize/pre_defined/reserved/samplerate)
     * before any child boxes (esds). */
    if (entry_size < 8 + 28 || entry_size > entry_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t *esds = NULL;
    uint32_t esds_len = 0;
    if (!box_find(entry + 8 + 28, entry_size - 8 - 28, "esds", &esds, &esds_len)) {
        ESP_LOGE(TAG, "No esds box in audio sample entry");
        return ESP_ERR_NOT_FOUND;
    }

    /* esds: version(1)+flags(3), then MPEG-4 ES_Descriptor (tag/len fields
     * use the base-128 continuation encoding). We only need to walk down to
     * the DecoderSpecificInfo (tag 0x05) payload, which is the ASC. */
    if (esds_len < 4) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t *p = esds + 4;
    const uint8_t *end = esds + esds_len;

    for (int depth = 0; depth < 8 && p < end; depth++) {
        uint8_t tag = *p++;
        uint32_t desc_len = 0;
        int shifts = 0;
        while (p < end && shifts < 4) {
            uint8_t byte = *p++;
            desc_len = (desc_len << 7) | (byte & 0x7F);
            shifts++;
            if (!(byte & 0x80)) {
                break;
            }
        }
        if (p + desc_len > end) {
            break;
        }
        if (tag == 0x03) {          /* ES_DescriptorTag: ES_ID(2)+flags(1), then nested descriptors */
            p += 3;
            continue;
        }
        if (tag == 0x04) {          /* DecoderConfigDescrTag: fixed fields, then nested DecoderSpecificInfo */
            p += 13;
            continue;
        }
        if (tag == 0x05) {          /* DecoderSpecificInfoTag: this is the AudioSpecificConfig */
            uint8_t object_type, freq_index, chan_cfg;
            if (!parse_audio_specific_config(p, desc_len, &object_type, &freq_index, &chan_cfg)) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (object_type != 2) {
                /* Not plain AAC-LC (e.g. 5 = HE-AAC/SBR) - ADTS profile bits
                 * can't represent that unambiguously without more work. */
                ESP_LOGE(TAG, "Unsupported AAC object type %u (only AAC-LC/2 is handled)", object_type);
                return ESP_ERR_NOT_SUPPORTED;
            }
            b->adts_profile = (uint8_t)(object_type - 1);
            b->adts_freq_index = freq_index;
            b->adts_channel_cfg = chan_cfg;
            b->asc_valid = true;
            ESP_LOGI(TAG, "Init segment parsed: AAC-LC, freq_index=%u, channels=%u",
                     freq_index, chan_cfg);
            return ESP_OK;
        }
        p += desc_len;
    }

    ESP_LOGE(TAG, "DecoderSpecificInfo (AudioSpecificConfig) not found in esds");
    return ESP_ERR_NOT_FOUND;
}

/* MPEG-4 sampling_frequency_index table (ISO/IEC 14496-3). Index 0x0F means
 * "explicit rate follows", which parse_audio_specific_config() rejects, so
 * every index reaching here is a real table entry. */
static const int fmp4_sample_rate_table[13] = {
    96000, 88200, 64000, 48000, 44100, 32000,
    24000, 22050, 16000, 12000, 11025, 8000, 7350,
};

int fmp4_bridge_get_sample_rate(audio_element_handle_t self)
{
    fmp4_bridge_t *b = (fmp4_bridge_t *)audio_element_getdata(self);
    if (!b || !b->asc_valid || b->adts_freq_index >= 13) {
        return 0;
    }
    return fmp4_sample_rate_table[b->adts_freq_index];
}

int fmp4_bridge_get_channels(audio_element_handle_t self)
{
    fmp4_bridge_t *b = (fmp4_bridge_t *)audio_element_getdata(self);
    if (!b || !b->asc_valid) {
        return 0;
    }
    return b->adts_channel_cfg;
}

/* ---- Per-fragment moof parsing: walk moof/traf/{tfhd,trun} to build a
 * per-sample size table for the mdat that immediately follows. ---- */

static void parse_trun(fmp4_bridge_t *b, const uint8_t *body, uint32_t len)
{
    if (len < 8) {
        return;
    }
    uint32_t flags = be32(body) & 0x00FFFFFF;
    uint32_t sample_count = be32(body + 4);
    const uint8_t *p = body + 8;
    const uint8_t *end = body + len;

    if (flags & 0x000001) { /* data-offset-present */
        p += 4;
    }
    if (flags & 0x000004) { /* first-sample-flags-present */
        p += 4;
    }

    bool has_duration = flags & 0x000100;
    bool has_size     = flags & 0x000200;
    bool has_flags    = flags & 0x000400;
    bool has_cto      = flags & 0x000800;

    if (!has_size && !b->have_default_sample_size) {
        ESP_LOGE(TAG, "trun has no per-sample size and tfhd gave no default; cannot demux this fragment");
        return;
    }
    if (sample_count > FMP4_MAX_SAMPLES) {
        ESP_LOGE(TAG, "Fragment has %u samples, more than the %d supported; skipping",
                 (unsigned)sample_count, FMP4_MAX_SAMPLES);
        return;
    }

    /* Reuse the existing table when it is already big enough - on this
     * stream every fragment has the same ~751 samples, so after the first
     * fragment this allocates nothing at all. */
    int want = (int)(sample_count ? sample_count : 1);
    if (b->sample_cap < want) {
        uint32_t *grown = audio_calloc(want, sizeof(uint32_t));
        if (!grown) {
            ESP_LOGE(TAG, "OOM allocating %d sample sizes (%d bytes); keeping previous %d-entry table",
                     want, (int)(want * sizeof(uint32_t)), b->sample_cap);
            return;
        }
        if (b->sample_sizes) {
            audio_free(b->sample_sizes);
        }
        b->sample_sizes = grown;
        b->sample_cap = want;
    }
    uint32_t *sizes = b->sample_sizes;

    uint32_t n = 0;
    for (; n < sample_count; n++) {
        if (has_duration) {
            if (p + 4 > end) break;
            p += 4;
        }
        if (has_size) {
            if (p + 4 > end) break;
            sizes[n] = be32(p);
            p += 4;
        } else {
            sizes[n] = b->default_sample_size;
        }
        if (has_flags) {
            if (p + 4 > end) break;
            p += 4;
        }
        if (has_cto) {
            if (p + 4 > end) break;
            p += 4;
        }
    }

    if (n != sample_count) {
        ESP_LOGW(TAG, "trun truncated: parsed %u of %u samples", (unsigned)n, (unsigned)sample_count);
    }
    if (n == 0) {
        /* Nothing usable this fragment. sizes IS b->sample_sizes now (it is
         * retained across fragments) - freeing it here would leave a
         * dangling pointer for the next one. Just leave sample_count at 0;
         * the mdat branch already treats that as "no usable moof/trun". */
        return;
    }

    /* sizes aliases b->sample_sizes - no ownership transfer needed. */
    b->sample_count = (int)n;
    b->sample_index = 0;
    b->sample_need_header = true;
    b->sample_bytes_left = sizes[0];
}

static void parse_tfhd(fmp4_bridge_t *b, const uint8_t *body, uint32_t len)
{
    if (len < 8) {
        return;
    }
    uint32_t flags = be32(body) & 0x00FFFFFF;
    const uint8_t *p = body + 8; /* skip version/flags(4) + track_ID(4) */
    const uint8_t *end = body + len;

    if (flags & 0x000001) { /* base-data-offset-present */
        p += 8;
    }
    if (flags & 0x000002) { /* sample-description-index-present */
        p += 4;
    }
    if (flags & 0x000008) { /* default-sample-duration-present */
        p += 4;
    }
    if (flags & 0x000010) { /* default-sample-size-present */
        if (p + 4 <= end) {
            b->default_sample_size = be32(p);
            b->have_default_sample_size = true;
        }
    }
}

static void parse_traf(fmp4_bridge_t *b, const uint8_t *body, uint32_t len)
{
    uint32_t i = 0;
    b->have_default_sample_size = false;
    b->default_sample_size = 0;
    /* tfhd always precedes trun within traf per spec; two passes keeps this
     * independent of that ordering assumption. */
    while (i + 8 <= len) {
        uint32_t size = be32(body + i);
        if (size == 0 || size == 1) {
            /* Same unsupported-sentinel-size stance as box_find() - see its
             * comment. Stopping this pass early just means tfhd's defaults
             * (if any) are missed for this fragment; the mdat branch in
             * fmp4_bridge_feed() already treats sample_count==0 as "no
             * usable moof/trun" and skips the fragment safely. */
            ESP_LOGW(TAG, "traf child box uses an unsupported 64-bit-largesize/extends-to-end size (%u); stopping tfhd scan",
                     (unsigned)size);
            break;
        }
        if (size < 8 || i + size > len) break;
        if (memcmp(body + i + 4, "tfhd", 4) == 0) {
            parse_tfhd(b, body + i + 8, size - 8);
        }
        i += size;
    }
    i = 0;
    while (i + 8 <= len) {
        uint32_t size = be32(body + i);
        if (size == 0 || size == 1) {
            ESP_LOGW(TAG, "traf child box uses an unsupported 64-bit-largesize/extends-to-end size (%u); stopping trun scan",
                     (unsigned)size);
            break;
        }
        if (size < 8 || i + size > len) break;
        if (memcmp(body + i + 4, "trun", 4) == 0) {
            parse_trun(b, body + i + 8, size - 8);
            return; /* one audio track => one trun we care about */
        }
        i += size;
    }
}

static void parse_moof(fmp4_bridge_t *b, const uint8_t *body, uint32_t len)
{
    uint32_t i = 0;
    while (i + 8 <= len) {
        uint32_t size = be32(body + i);
        if (size == 0 || size == 1) {
            ESP_LOGW(TAG, "moof child box uses an unsupported 64-bit-largesize/extends-to-end size (%u); stopping traf scan",
                     (unsigned)size);
            break;
        }
        if (size < 8 || i + size > len) break;
        if (memcmp(body + i + 4, "traf", 4) == 0) {
            parse_traf(b, body + i + 8, size - 8);
            return; /* single audio track stream => one traf */
        }
        i += size;
    }
}

/* ---- ADTS header synthesis ---- */

static void write_adts_header(fmp4_bridge_t *b, uint8_t *out, uint32_t raw_len)
{
    uint32_t frame_len = 7 + raw_len; /* ADTS frame length includes its own header */
    out[0] = 0xFF;
    out[1] = 0xF1; /* MPEG-4, layer 0, no CRC */
    out[2] = (uint8_t)((b->adts_profile << 6) | (b->adts_freq_index << 2) | ((b->adts_channel_cfg >> 2) & 0x01));
    out[3] = (uint8_t)(((b->adts_channel_cfg & 0x03) << 6) | ((frame_len >> 11) & 0x03));
    out[4] = (uint8_t)((frame_len >> 3) & 0xFF);
    out[5] = (uint8_t)(((frame_len & 0x07) << 5) | 0x1F);
    out[6] = (uint8_t)0xFC;
}

/* ---- Streaming byte-level state machine ---- */

static esp_err_t fmp4_bridge_feed(audio_element_handle_t self, fmp4_bridge_t *b, const uint8_t *data, int len)
{
#if FMP4_DEBUG_TRACE
    if (!b->trace_logged_first_input) {
        b->trace_logged_first_input = true;
        fmp4_trace_hex("first input chunk", data, len);
        ESP_LOGI(TAG, "TRACE asc_valid=%d profile=%u freq_idx=%u chan=%u",
                 (int)b->asc_valid, b->adts_profile, b->adts_freq_index, b->adts_channel_cfg);
    }
#endif
    while (len > 0) {
        switch (b->state) {
        case FMP4_ST_BOX_HEADER: {
            int need = 8 - b->hdr_have;
            int take = need < len ? need : len;
            memcpy(b->hdr_buf + b->hdr_have, data, take);
            b->hdr_have += take;
            data += take;
            len -= take;
            if (b->hdr_have < 8) {
                break;
            }
            b->hdr_have = 0;
            uint32_t size = be32(b->hdr_buf);
            char type[5];
            memcpy(type, b->hdr_buf + 4, 4);
            type[4] = '\0';
            if (size < 8) {
                ESP_LOGE(TAG, "Bad box size %u for '%s' (64-bit largesize boxes are not supported)",
                         (unsigned)size, type);
                return ESP_FAIL;
            }
            uint32_t body_len = size - 8;
#if FMP4_DEBUG_TRACE
            if (b->trace_boxes_seen < FMP4_TRACE_MAX_BOXES) {
                b->trace_boxes_seen++;
                ESP_LOGI(TAG, "TRACE box #%d type='%s' size=%u", b->trace_boxes_seen, type, (unsigned)size);
            }
#endif
            if (strcmp(type, "moof") == 0) {
                if (body_len > FMP4_MAX_MOOF_BYTES) {
                    ESP_LOGE(TAG, "moof box too large (%u bytes, max %d)", (unsigned)body_len, FMP4_MAX_MOOF_BYTES);
                    return ESP_FAIL;
                }
                /* Grow-and-reuse, same rationale as the sample-size table:
                 * this ~9KB block otherwise churns once per fragment. */
                if (body_len > b->box_cap) {
                    uint8_t *grown = audio_malloc(body_len);
                    if (!grown) {
                        ESP_LOGE(TAG, "OOM allocating %u bytes for moof (have %u)",
                                 (unsigned)body_len, (unsigned)b->box_cap);
                        return ESP_ERR_NO_MEM;
                    }
                    if (b->box_buf) {
                        audio_free(b->box_buf);
                    }
                    b->box_buf = grown;
                    b->box_cap = body_len;
                }
                b->box_want = body_len;
                b->box_have = 0;
                b->state = FMP4_ST_BOX_BUFFER;
            } else if (strcmp(type, "mdat") == 0) {
                if (b->sample_count == 0 || b->sample_sizes == NULL) {
                    ESP_LOGW(TAG, "mdat with no preceding usable moof/trun; skipping %u bytes", (unsigned)body_len);
                    b->skip_remaining = body_len;
                    b->state = body_len ? FMP4_ST_BOX_SKIP : FMP4_ST_BOX_HEADER;
                } else {
                    b->mdat_remaining = body_len;
                    b->state = FMP4_ST_MDAT_SAMPLE;
                }
            } else {
                b->skip_remaining = body_len;
                b->state = body_len ? FMP4_ST_BOX_SKIP : FMP4_ST_BOX_HEADER;
            }
            break;
        }

        case FMP4_ST_BOX_BUFFER: {
            uint32_t need = b->box_want - b->box_have;
            uint32_t take = (uint32_t)len < need ? (uint32_t)len : need;
            if (take > 0 && b->box_buf) {
                memcpy(b->box_buf + b->box_have, data, take);
            }
            b->box_have += take;
            data += take;
            len -= (int)take;
            if (b->box_have >= b->box_want) {
                if (b->box_buf) {
                    parse_moof(b, b->box_buf, b->box_want);
                    /* Retained for the next fragment - see box_cap. */
                }
                b->state = FMP4_ST_BOX_HEADER;
            }
            break;
        }

        case FMP4_ST_BOX_SKIP: {
            uint32_t take = (uint32_t)len < b->skip_remaining ? (uint32_t)len : b->skip_remaining;
            data += take;
            len -= (int)take;
            b->skip_remaining -= take;
            if (b->skip_remaining == 0) {
                b->state = FMP4_ST_BOX_HEADER;
            }
            break;
        }

        case FMP4_ST_MDAT_SAMPLE: {
            if (!b->asc_valid) {
                ESP_LOGE(TAG, "mdat samples arrived before the init segment's AudioSpecificConfig was set");
                return ESP_FAIL;
            }
            if (b->sample_need_header) {
                uint8_t hdr[7];
                write_adts_header(b, hdr, b->sample_sizes[b->sample_index]);
#if FMP4_DEBUG_TRACE
                if (!b->trace_logged_first_adts) {
                    b->trace_logged_first_adts = true;
                    fmp4_trace_hex("first ADTS header out", hdr, sizeof(hdr));
                    ESP_LOGI(TAG, "TRACE first sample: idx=%d size=%u sample_count=%d mdat_remaining=%u",
                             b->sample_index, (unsigned)b->sample_sizes[b->sample_index],
                             b->sample_count, (unsigned)b->mdat_remaining);
                }
#endif
                int wr = audio_element_output(self, (char *)hdr, sizeof(hdr));
                if (wr < 0) {
#if FMP4_DEBUG_TRACE
                    ESP_LOGE(TAG, "TRACE ADTS header output failed: %d", wr);
#endif
                    return ESP_FAIL;
                }
#if FMP4_DEBUG_TRACE
                if (wr != (int)sizeof(hdr)) {
                    /* A short write here would desynchronize the elementary
                     * stream: the decoder would see a truncated ADTS header
                     * followed by raw payload, which is exactly what "RAW
                     * AAC" detection looks like from its side. */
                    ESP_LOGW(TAG, "TRACE short ADTS header write: %d of %d", wr, (int)sizeof(hdr));
                }
                b->trace_total_out += (uint32_t)(wr > 0 ? wr : 0);
#endif
                b->sample_need_header = false;
            }
            uint32_t take = (uint32_t)len;
            if (take > b->sample_bytes_left) take = b->sample_bytes_left;
            if (take > b->mdat_remaining) take = b->mdat_remaining;
            if (take > 0) {
                if (audio_element_output(self, (char *)data, (int)take) < 0) {
                    return ESP_FAIL;
                }
                data += take;
                len -= (int)take;
                b->sample_bytes_left -= take;
                b->mdat_remaining -= take;
            }
            if (b->sample_bytes_left == 0 && b->mdat_remaining > 0) {
                b->sample_index++;
                if (b->sample_index >= b->sample_count) {
                    /* trun/mdat mismatch: more payload than samples we know about */
                    b->skip_remaining = b->mdat_remaining;
                    b->mdat_remaining = 0;
                    fmp4_reset_fragment(b);
                    b->state = b->skip_remaining ? FMP4_ST_BOX_SKIP : FMP4_ST_BOX_HEADER;
                } else {
                    b->sample_need_header = true;
                    b->sample_bytes_left = b->sample_sizes[b->sample_index];
                }
            } else if (b->mdat_remaining == 0) {
                fmp4_reset_fragment(b);
                b->state = FMP4_ST_BOX_HEADER;
            }
            break;
        }
        }
    }
    return ESP_OK;
}

static esp_err_t fmp4_bridge_open(audio_element_handle_t self)
{
    (void)self;
    ESP_LOGI(TAG, "fMP4 bridge opened");
    return ESP_OK;
}

static esp_err_t fmp4_bridge_close(audio_element_handle_t self)
{
    fmp4_bridge_t *b = (fmp4_bridge_t *)audio_element_getdata(self);
    if (b) {
        fmp4_reset_fragment(b);
        /* box_buf/sample_sizes are deliberately kept here: close() runs on
         * every pipeline stop/restart, and this element is re-opened with
         * the same fragment shape each time. They are released in
         * destroy() below, which is the real teardown. */
        b->state = FMP4_ST_BOX_HEADER;
        b->hdr_have = 0;
        b->box_want = 0;
        b->box_have = 0;
        b->skip_remaining = 0;
    }
    return ESP_OK;
}

static esp_err_t fmp4_bridge_destroy(audio_element_handle_t self)
{
    fmp4_bridge_t *b = (fmp4_bridge_t *)audio_element_getdata(self);
    if (b) {
        fmp4_reset_fragment(b);
        if (b->box_buf) {
            audio_free(b->box_buf);
        }
        if (b->sample_sizes) {
            audio_free(b->sample_sizes);
        }
        audio_free(b);
    }
    return ESP_OK;
}

static audio_element_err_t fmp4_bridge_process(audio_element_handle_t self, char *el_buffer, int el_buf_len)
{
    fmp4_bridge_t *b = (fmp4_bridge_t *)audio_element_getdata(self);
    int r = audio_element_input(self, (char *)b->in_scratch, sizeof(b->in_scratch));
    if (r <= 0) {
        return r;
    }
    if (fmp4_bridge_feed(self, b, b->in_scratch, r) != ESP_OK) {
        return AEL_IO_FAIL;
    }
    return r;
}

audio_element_handle_t fmp4_bridge_init(fmp4_bridge_cfg_t *config)
{
    fmp4_bridge_t *b = audio_calloc(1, sizeof(fmp4_bridge_t));
    if (!b) {
        return NULL;
    }

    /* Claim the two big per-fragment buffers now, while the heap is clean -
     * see FMP4_PREALLOC_MOOF_BYTES for why this must not be deferred. */
    b->box_buf = audio_malloc(FMP4_PREALLOC_MOOF_BYTES);
    b->sample_sizes = audio_calloc(FMP4_PREALLOC_SAMPLES, sizeof(uint32_t));
    if (!b->box_buf || !b->sample_sizes) {
        ESP_LOGE(TAG, "OOM pre-allocating fmp4 buffers (%d + %d bytes)",
                 FMP4_PREALLOC_MOOF_BYTES,
                 (int)(FMP4_PREALLOC_SAMPLES * sizeof(uint32_t)));
        if (b->box_buf) audio_free(b->box_buf);
        if (b->sample_sizes) audio_free(b->sample_sizes);
        audio_free(b);
        return NULL;
    }
    b->box_cap = FMP4_PREALLOC_MOOF_BYTES;
    b->sample_cap = FMP4_PREALLOC_SAMPLES;
    ESP_LOGI(TAG, "Pre-allocated moof buffer %d B and %d-sample table (free heap=%" PRIu32
             ", largest block=%u)",
             FMP4_PREALLOC_MOOF_BYTES, FMP4_PREALLOC_SAMPLES,
             esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open = fmp4_bridge_open;
    cfg.close = fmp4_bridge_close;
    cfg.process = fmp4_bridge_process;
    cfg.destroy = fmp4_bridge_destroy;
    cfg.task_stack = config->task_stack;
    cfg.task_prio = config->task_prio;
    cfg.task_core = config->task_core;
    cfg.out_rb_size = config->out_rb_size;
    cfg.tag = "fmp4_bridge";

    audio_element_handle_t el = audio_element_init(&cfg);
    if (!el) {
        audio_free(b);
        return NULL;
    }
    audio_element_setdata(el, b);
    return el;
}

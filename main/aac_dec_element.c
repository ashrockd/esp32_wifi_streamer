#include "aac_dec_element.h"

#include <inttypes.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "audio_element.h"
#include "audio_mem.h"

#include "esp_aac_dec.h"
#include "esp_audio_dec.h"

static const char *TAG = "AAC_DEC";

/* One AAC-LC frame is exactly 1024 samples; stereo 16-bit => 4096 bytes.
 * Sized to that, not doubled: esp_aac_dec reports needed_size when a buffer
 * is short, which process() handles explicitly, so over-allocating here only
 * steals contiguous heap from mbedTLS (whose TLS record buffer needs ~15KB
 * in one block - "Dynamic Impl: alloc(15399 bytes) failed" was caused by
 * exactly that competition). */
#define AAC_DEC_PCM_BYTES        4096

/* Compressed input held per process() call. Real frames on this stream run
 * ~470-1060 bytes (measured from a captured segment), so this holds at least
 * one whole frame plus a partial - which is all the refill logic needs. */
#define AAC_DEC_IN_BYTES         2048

typedef struct {
    void    *dec;               /* esp_aac_dec handle */
    bool     opened;
    bool     reported_info;

    esp_aac_dec_cfg_t cfg;

    uint8_t *in_buf;
    int      in_fill;           /* valid bytes currently in in_buf */

    uint8_t *pcm_buf;
} aac_dec_t;

static esp_err_t aac_dec_open(audio_element_handle_t self)
{
    aac_dec_t *d = (aac_dec_t *)audio_element_getdata(self);
    if (!d) return ESP_FAIL;
    if (d->opened) return ESP_OK;

    /* The format is supplied, never sniffed - that is the entire reason this
     * element exists (see aac_dec_element.h). */
    uint32_t heap_before = esp_get_free_heap_size();
    uint32_t block_before = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    esp_audio_err_t err = esp_aac_dec_open(&d->cfg, sizeof(d->cfg), &d->dec);
    if (err != ESP_AUDIO_ERR_OK || !d->dec) {
        ESP_LOGE(TAG, "esp_aac_dec_open failed: %d (rate=%d ch=%d no_adts=%d)",
                 (int)err, (int)d->cfg.sample_rate, (int)d->cfg.channel,
                 (int)d->cfg.no_adts_header);
        return ESP_FAIL;
    }

    d->in_fill = 0;
    d->reported_info = false;
    d->opened = true;
    /* Log what the decoder itself costs. mbedTLS needs a single ~15KB block
     * for a TLS record on this CDN, so the largest free BLOCK matters more
     * than total free heap - track both. */
    ESP_LOGI(TAG, "Opened: %d Hz, %d ch, %s | heap %" PRIu32 "->%" PRIu32
             " (decoder used %" PRId32 "), largest block %" PRIu32 "->%" PRIu32,
             (int)d->cfg.sample_rate, (int)d->cfg.channel,
             d->cfg.no_adts_header ? "raw AAC (no ADTS)" : "ADTS framed",
             heap_before, esp_get_free_heap_size(),
             (int32_t)heap_before - (int32_t)esp_get_free_heap_size(),
             block_before, heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return ESP_OK;
}

static esp_err_t aac_dec_close(audio_element_handle_t self)
{
    aac_dec_t *d = (aac_dec_t *)audio_element_getdata(self);
    if (d && d->opened) {
        esp_aac_dec_close(d->dec);
        d->dec = NULL;
        d->opened = false;
        d->in_fill = 0;
    }
    return ESP_OK;
}

static esp_err_t aac_dec_destroy(audio_element_handle_t self)
{
    aac_dec_t *d = (aac_dec_t *)audio_element_getdata(self);
    if (d) {
        if (d->opened) {
            esp_aac_dec_close(d->dec);
        }
        if (d->in_buf) {
            audio_free(d->in_buf);
            d->in_buf = NULL;
        }
        if (d->pcm_buf) {
            audio_free(d->pcm_buf);
            d->pcm_buf = NULL;
        }
        /* Clear the element's back-pointer before freeing what it points at,
         * so a second destroy (or anything else that reaches for the private
         * state after teardown) sees NULL rather than a dangling pointer. */
        audio_element_setdata(self, NULL);
        d->dec = NULL;
        audio_free(d);
    }
    return ESP_OK;
}

static audio_element_err_t aac_dec_process(audio_element_handle_t self,
                                           char *el_buffer, int el_buf_len)
{
    aac_dec_t *d = (aac_dec_t *)audio_element_getdata(self);

    /* in_fill must always be a valid index into a AAC_DEC_IN_BYTES buffer.
     * Nothing should be able to violate that any more (see the clamps in the
     * decode loop and the slide below), but check on the way in too: this is
     * the value every memmove()/pointer arithmetic in this function is
     * derived from, so a single bad one corrupts the heap rather than
     * producing bad audio. */
    if (d->in_fill < 0 || d->in_fill > AAC_DEC_IN_BYTES) {
        ESP_LOGE(TAG, "in_fill=%d is out of range [0,%d]; resetting the input buffer",
                 d->in_fill, AAC_DEC_IN_BYTES);
        d->in_fill = 0;
    }

    /* Top up the compressed-input buffer. Whatever a previous call could not
     * consume (a partial trailing frame) stays at the front, so frames that
     * straddle reads are still decoded rather than dropped. */
    int want = AAC_DEC_IN_BYTES - d->in_fill;
    if (want > 0) {
        int r = audio_element_input(self, (char *)d->in_buf + d->in_fill, want);
        if (r <= 0) {
            /* AEL_IO_DONE / ABORT / TIMEOUT - propagate as-is. */
            if (d->in_fill == 0) {
                return r;
            }
        } else {
            d->in_fill += r;
        }
    }

    if (d->in_fill <= 0) {
        return AEL_IO_DONE;
    }

    /* Only ever hand the decoder WHOLE frames.
     *
     * fmp4_bridge emits ADTS, so aac_frame_length is right there in the
     * header - bytes 3..5, 13 bits spanning three bytes. Feeding a trailing
     * partial frame is what produced the endless
     * "ESP_AAC_DEC: Failed to decode aac frame, error:30" spam: the decoder
     * was being asked to decode an incomplete frame every single call. It
     * also resynchronizes us if the stream ever slips, because we verify the
     * 0xFFF syncword before trusting the length. */
    int usable = 0;
    while (usable + 7 <= d->in_fill) {
        const uint8_t *h = d->in_buf + usable;
        if (!(h[0] == 0xFF && (h[1] & 0xF0) == 0xF0)) {
            /* Lost sync - drop one byte and hunt for the next syncword
             * rather than feeding the decoder garbage. */
            memmove(d->in_buf, d->in_buf + 1, d->in_fill - 1);
            d->in_fill--;
            usable = 0;
            continue;
        }
        int frame_len = ((h[3] & 0x03) << 11) | (h[4] << 3) | ((h[5] >> 5) & 0x07);
        if (frame_len < 7) {
            /* Nonsense length - treat as lost sync. */
            memmove(d->in_buf, d->in_buf + 1, d->in_fill - 1);
            d->in_fill--;
            usable = 0;
            continue;
        }
        if (usable + frame_len > d->in_fill) {
            break;  /* frame straddles the buffer end - wait for more input */
        }
        usable += frame_len;
    }

    if (usable == 0) {
        /* Not even one whole frame buffered yet; refill on the next call. */
        return AEL_IO_OK;
    }

    esp_audio_dec_in_raw_t raw = {
        .buffer = d->in_buf,
        .len    = (uint32_t)usable,
    };
    esp_audio_dec_out_frame_t out = {
        .buffer = d->pcm_buf,
        .len    = AAC_DEC_PCM_BYTES,
    };
    esp_audio_dec_info_t info = {0};

    int produced = 0;
    while (raw.len > 0) {
        raw.consumed = 0;
        out.decoded_size = 0;

        esp_audio_err_t err = esp_aac_dec_decode(d->dec, &raw, &out, &info);

        if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            /* Should not happen with AAC-LC at this buffer size, but handle
             * it rather than silently corrupting the stream. */
            ESP_LOGE(TAG, "PCM buffer too small: need %d, have %d",
                     (int)out.needed_size, AAC_DEC_PCM_BYTES);
            return AEL_IO_FAIL;
        }
        if (err != ESP_AUDIO_ERR_OK) {
            if (raw.consumed == 0) {
                /* Not enough bytes for a whole frame yet - keep the remainder
                 * and come back with more input next call. */
                break;
            }
            ESP_LOGW(TAG, "decode error %d, skipping %d bytes",
                     (int)err, (int)raw.consumed);
        }

        if (out.decoded_size > 0) {
            /* Publish the real stream format once known, so the pipeline can
             * retune the I2S clock (this stream is 48 kHz, not 44.1 kHz). */
            if (!d->reported_info && info.sample_rate > 0) {
                audio_element_set_music_info(self, (int)info.sample_rate,
                                             (int)info.channel,
                                             (int)info.bits_per_sample);
                /* audio_element_report_info() is what actually emits
                 * AEL_MSG_CMD_REPORT_MUSIC_INFO, which radio_pipeline_wait()
                 * listens for to retune the I2S clock. */
                audio_element_report_info(self);
                d->reported_info = true;
                ESP_LOGI(TAG, "Decoded format: %d Hz, %d ch, %d bit",
                         (int)info.sample_rate, (int)info.channel,
                         (int)info.bits_per_sample);
            }

            int w = audio_element_output(self, (char *)d->pcm_buf, (int)out.decoded_size);
            if (w < 0) {
                return w;
            }
            produced += w;
        }

        if (raw.consumed == 0) {
            break;  /* no forward progress - avoid spinning */
        }
        /* raw.len is UNSIGNED (uint32_t, esp_audio_dec_in_raw_t). If the
         * decoder ever reports consuming more than it was handed - which a
         * truncated/garbage frame can provoke - the subtraction below wraps
         * to ~4 billion instead of going negative. That does not just spin:
         * the loop then walks raw.buffer far past in_buf, and the
         * `usable - (int)raw.len` below comes out hugely negative, which
         * sets d->in_fill to a huge value and turns the next call's
         * memmove() into a multi-kilobyte write past a 2048-byte heap
         * buffer. That is a heap-corruption bug, not a decode glitch, so
         * clamp rather than trusting the value. */
        if (raw.consumed > raw.len) {
            ESP_LOGE(TAG, "decoder reported consumed=%u of only %u available; clamping",
                     (unsigned)raw.consumed, (unsigned)raw.len);
            raw.consumed = raw.len;
        }
        raw.buffer += raw.consumed;
        raw.len    -= raw.consumed;
    }

    /* Slide everything still unconsumed back to the front. Note there are
     * TWO regions to preserve, not one: whatever the decoder did not take
     * out of the whole-frame region [0, usable), plus the trailing partial
     * frame [usable, in_fill) that was deliberately withheld above. */
    int consumed = usable - (int)raw.len;
    /* Belt and braces on top of the clamp above: in_fill indexes a fixed
     * AAC_DEC_IN_BYTES heap buffer, so every path that assigns it is bounded
     * here rather than trusting the arithmetic that produced it. A wrong
     * value costs at most one dropped frame; an unbounded one corrupts the
     * heap (see the comment in the decode loop). */
    if (consumed < 0) {
        consumed = 0;
    }
    if (consumed > d->in_fill) {
        consumed = d->in_fill;
    }
    int leftover = d->in_fill - consumed;
    if (leftover > 0 && consumed > 0) {
        memmove(d->in_buf, d->in_buf + consumed, leftover);
    }
    d->in_fill = leftover > 0 ? leftover : 0;

    /* Guard against a wedged stream: if the buffer is full and nothing was
     * consumed, we will never make progress. Drop it rather than spin. */
    if (produced == 0 && d->in_fill >= AAC_DEC_IN_BYTES) {
        ESP_LOGW(TAG, "no frame found in a full %d-byte buffer; resyncing",
                 AAC_DEC_IN_BYTES);
        d->in_fill = 0;
    }

    return produced > 0 ? produced : AEL_IO_OK;
}

audio_element_handle_t aac_dec_element_init(aac_dec_element_cfg_t *config)
{
    if (!config) return NULL;

    aac_dec_t *d = audio_calloc(1, sizeof(aac_dec_t));
    if (!d) return NULL;

    d->in_buf  = audio_malloc(AAC_DEC_IN_BYTES);
    d->pcm_buf = audio_malloc(AAC_DEC_PCM_BYTES);
    if (!d->in_buf || !d->pcm_buf) {
        ESP_LOGE(TAG, "OOM allocating decoder buffers");
        if (d->in_buf)  audio_free(d->in_buf);
        if (d->pcm_buf) audio_free(d->pcm_buf);
        audio_free(d);
        return NULL;
    }

    d->cfg.sample_rate     = config->sample_rate;
    d->cfg.channel         = (uint8_t)config->channels;
    d->cfg.bits_per_sample = 16;
    d->cfg.no_adts_header  = config->no_adts_header;
    d->cfg.aac_plus_enable = false;   /* mp4a.40.2 rendition: AAC-LC, no SBR */

    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open        = aac_dec_open;
    cfg.close       = aac_dec_close;
    cfg.process     = aac_dec_process;
    cfg.destroy     = aac_dec_destroy;
    cfg.task_stack  = config->task_stack;
    cfg.task_prio   = config->task_prio;
    cfg.task_core   = config->task_core;
    cfg.out_rb_size = config->out_rb_size;
    cfg.tag         = "aac_dec";

    audio_element_handle_t el = audio_element_init(&cfg);
    if (!el) {
        audio_free(d->in_buf);
        audio_free(d->pcm_buf);
        audio_free(d);
        return NULL;
    }
    audio_element_setdata(el, d);
    return el;
}

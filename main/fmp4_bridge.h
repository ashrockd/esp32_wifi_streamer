#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "audio_element.h"

/*
 * Apple's TuneIn HLS rendition is CMAF/fMP4: an init segment (ftyp+moov,
 * fetched once via the HLS media playlist's EXT-X-MAP tag) followed by a
 * stream of "moof+mdat" media segments, each mdat holding raw (headerless)
 * AAC-LC access units whose sizes are only known from that segment's own
 * moof/traf/trun sample table.
 *
 * ESP-ADF's stock aac_decoder expects an ADTS elementary stream, not ISO-BMFF
 * boxes. This element sits between http_stream (which feeds it the
 * concatenated segment bytes ADF's own HLS engine already fetches, since
 * enable_playlist_parser handles the media-playlist/segment iteration) and
 * aac_decoder: it walks the incoming boxes, and for every AAC sample it
 * finds in an mdat, synthesizes a 7-byte ADTS header (from the AudioSpecificConfig
 * parsed out of the init segment) and forwards header+payload downstream.
 *
 * It streams rather than buffers: a single 16s segment can be 500KB+, far
 * more than this no-PSRAM board can hold, so only small boxes (moof, a few KB)
 * are ever fully buffered; mdat payload is sliced and forwarded as it arrives.
 */

typedef struct {
    int out_rb_size;
    int task_stack;
    int task_prio;
    int task_core;
} fmp4_bridge_cfg_t;

#define FMP4_BRIDGE_RINGBUF_SIZE   (8 * 1024)
#define FMP4_BRIDGE_TASK_STACK     (3 * 1024)
#define FMP4_BRIDGE_TASK_PRIO      (5)
#define FMP4_BRIDGE_TASK_CORE      (0)

#define FMP4_BRIDGE_CFG_DEFAULT() {          \
    .out_rb_size = FMP4_BRIDGE_RINGBUF_SIZE, \
    .task_stack  = FMP4_BRIDGE_TASK_STACK,   \
    .task_prio   = FMP4_BRIDGE_TASK_PRIO,    \
    .task_core   = FMP4_BRIDGE_TASK_CORE,    \
}

audio_element_handle_t fmp4_bridge_init(fmp4_bridge_cfg_t *config);

/*
 * Parses a CMAF init segment (ftyp+moov) and caches the AAC AudioSpecificConfig
 * (object type / sample rate / channel count) it contains, so the element can
 * synthesize correct ADTS headers for the samples in every media segment that
 * follows. Must be called once, before the pipeline is run (the element's own
 * task must not be started yet) - this is plain parsing, not element I/O.
 */
esp_err_t fmp4_bridge_set_init_segment(audio_element_handle_t self, const uint8_t *data, size_t len);

/*
 * Stream format recovered from the init segment's AudioSpecificConfig.
 * Only valid after fmp4_bridge_set_init_segment() has succeeded; both return
 * 0 before that. Exposed so the decoder downstream can be TOLD the format
 * explicitly instead of sniffing it - see aac_dec_element.h for why that
 * distinction matters here.
 */
int fmp4_bridge_get_sample_rate(audio_element_handle_t self);
int fmp4_bridge_get_channels(audio_element_handle_t self);

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "audio_element.h"

/*
 * An AAC decoder audio_element built directly on Espressif's esp_aac_dec
 * (esp-adf-libs/esp_audio_codec), rather than ESP-ADF's bundled
 * aac_decoder_init() element.
 *
 * Why this exists instead of the stock element:
 *   fmp4_bridge produces a provably valid ADTS elementary stream - verified
 *   offline against a real captured segment with tools/fmp4_to_adts.py, whose
 *   output ffprobe reads as "aac (LC), 48000 Hz, stereo, 252 kb/s" and which
 *   decodes to real audio (mean_volume -15.0 dB). On hardware, ESP-ADF's
 *   esp_decoder correctly reported "Detect audio type is AAC" from that same
 *   stream, but the aac_decoder it delegates to then re-decided
 *   "This audio is RAW AAC" and returned "Failed to initialize" - RAW mode
 *   needs an AudioSpecificConfig the stock element has no way to be given
 *   (aac_decoder_cfg_t exposes no such field). That element is a precompiled
 *   blob whose transport detection cannot be inspected or overridden.
 *
 *   esp_aac_dec, by contrast, takes an explicit esp_aac_dec_cfg_t: the format
 *   is *told* to the decoder rather than sniffed, which removes the entire
 *   class of failure above.
 */

typedef struct {
    int  out_rb_size;   /*!< Size of output ringbuffer */
    int  task_stack;    /*!< Task stack size */
    int  task_prio;     /*!< Task priority */
    int  task_core;     /*!< CPU core */

    /* Stream format, taken from the CMAF init segment (fmp4_bridge parses it
     * out of the moov/esds AudioSpecificConfig). Supplying these is the whole
     * point of this element - see the header comment. */
    int  sample_rate;   /*!< e.g. 48000 */
    int  channels;      /*!< 1 or 2 */

    /* false: input carries 7-byte ADTS headers (what fmp4_bridge emits today,
     *        and what the decoder uses to find frame boundaries).
     * true : input is bare AAC frames with no headers. */
    bool no_adts_header;
} aac_dec_element_cfg_t;

#define AAC_DEC_ELEMENT_TASK_STACK   (6 * 1024)
#define AAC_DEC_ELEMENT_TASK_PRIO    (5)
#define AAC_DEC_ELEMENT_TASK_CORE    (0)
#define AAC_DEC_ELEMENT_RINGBUF_SIZE (8 * 1024)

#define AAC_DEC_ELEMENT_CFG_DEFAULT() {           \
    .out_rb_size    = AAC_DEC_ELEMENT_RINGBUF_SIZE, \
    .task_stack     = AAC_DEC_ELEMENT_TASK_STACK,   \
    .task_prio      = AAC_DEC_ELEMENT_TASK_PRIO,    \
    .task_core      = AAC_DEC_ELEMENT_TASK_CORE,    \
    .sample_rate    = 48000,                        \
    .channels       = 2,                            \
    .no_adts_header = false,                        \
}

audio_element_handle_t aac_dec_element_init(aac_dec_element_cfg_t *config);

/*
 * Updates the stream format an ALREADY-CREATED element will use the next
 * time it opens (i.e. the next time its close()/open() cycle runs) -
 * without destroying and recreating the element itself. Exists so
 * radio_pipeline.c can keep ONE decoder element alive across every CMAF
 * session (station change, proactive refresh) instead of creating a fresh
 * one every time: esp_aac_dec_close() is proven on hardware to corrupt the
 * heap (see [[esp32-wifi-streamer-aac-heap-crash]]), so this project's
 * aac_dec_close() deliberately never calls it and just abandons the old
 * decoder handle - fine as a one-time cost at boot, but a real leak if it
 * happens on every session. Reusing the SAME element across sessions means
 * that only ever happens once, since every CMAF station observed so far
 * decodes at the exact same 48kHz/2ch/AAC-LC profile and this function is a
 * no-op when the requested format already matches what is configured - see
 * its own implementation comment for what happens on the rare case it does
 * not match (a real profile change, never observed in practice).
 *
 * Returns true if the format actually changed (caller may want to log
 * that), false if it already matched and nothing needed to change.
 */
bool aac_dec_element_reconfigure(audio_element_handle_t self, int sample_rate,
                                 int channels, bool no_adts_header);

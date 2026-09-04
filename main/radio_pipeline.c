#include "radio_pipeline.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_pipeline.h"
#include "audio_element.h"
#include "audio_event_iface.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "ringbuf.h"
#include "aac_dec_element.h"
#include "esp_decoder.h"
/* Not for configuring anything - only to READ BACK the pins ADF is really
 * going to use, so check_i2s_pins() below can catch a silent mismatch.
 * Available without a CMakeLists change: audio_stream lists audio_board in
 * its own public COMPONENT_REQUIRES. */
#include "board_pins_config.h"

#include "app_config.h"
#include "console_cli.h"
#include "fmp4_bridge.h"
#include "led_viz.h"
#include "playlist_prefetch.h"
#include "tunein_control.h"

static const char *TAG = "PIPELINE";
static audio_pipeline_handle_t pipeline;
static audio_element_handle_t http_reader;
static audio_element_handle_t fmp4_bridge_el;
static audio_element_handle_t aac_decoder;
/* ESP-ADF's auto-detecting decoder, used instead of fmp4_bridge+aac_decoder
 * for every session->format other than TUNEIN_FORMAT_HLS_CMAF_AAC - see
 * build_generic_decoder(). Exactly one of aac_decoder/generic_decoder is
 * non-NULL while a pipeline is up. */
static audio_element_handle_t generic_decoder;
/* Whichever of the two above is actually in the running chain, so the
 * music-info handler, element_name() and teardown do not each have to
 * re-derive it. */
static audio_element_handle_t decoder_el;
static audio_element_handle_t i2s_writer;
static audio_event_iface_handle_t event_iface;
/* What i2s_writer was actually created with, so the music-info
 * handler can tell 'already correct' from a real mismatch. */
static int i2s_configured_rate;
static int i2s_configured_ch;

/* Heap copy of session->hls_variant_url, kept alive for the life of the
 * pipeline (unlike tunein_session_t itself, which main.c frees right after
 * radio_pipeline_start() returns) - this is the URL playlist prefetching
 * re-fetches ahead of the live-window boundary. See playlist_prefetch.h. */
static char *cached_playlist_url;

/* 2026-08-22 (retry storm fix): a hardware log showed a persistently OOM'd
 * prefetch attempt retried on EVERY ~1s poll for 33 STRAIGHT SECONDS at one
 * live-window boundary (free heap bouncing 3.5-11KB, min_free_ever hit 700
 * bytes) - each attempt doing a fresh xTaskCreate/esp_http_client_init/
 * mbedtls_ssl_setup allocation attempt that failed, right when the pipeline
 * most needed a clean shot at its own TLS handshake for that same boundary.
 * That contention, not silence, is what showed up as renewed audio
 * instability. These two bound the damage: back off after any failed/empty
 * result instead of retrying next poll, and give up after a couple of
 * attempts per boundary and let the existing reactive HTTP_STREAM_FINISH_
 * PLAYLIST fallback (section 5b/c of docs/tunein-hls-gapless-streaming.md)
 * handle that one cleanly instead of continuing to hammer an already-tight
 * heap. */
#define PLAYLIST_PREFETCH_RETRY_BACKOFF_MS       4000
#define PLAYLIST_PREFETCH_MAX_ATTEMPTS_PER_WINDOW 2
static TickType_t next_prefetch_retry_tick;
static int prefetch_attempts_this_window;

static int http_stream_hook(http_stream_event_msg_t *msg)
{
    if (msg->event_id == HTTP_STREAM_FINISH_PLAYLIST) {
        /* This is a live HLS rendition, not a finite playlist: re-fetch the
         * (still-signed) playlist URL to pick up newly published segments
         * instead of treating "ran out of currently-known segments" as EOF. */
        esp_err_t err = http_stream_fetch_again(msg->el);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Live playlist re-fetch failed: %s (signed URL may have expired)",
                     esp_err_to_name(err));
        }
        return err;
    }
    return ESP_OK;
}

/* --- I2S pin verification ---------------------------------------------
 * RADIO_I2S_*_GPIO in app_config.h, and the i2s_cfg.std_cfg.gpio_cfg
 * assignments further down in this function, DO NOT reach the hardware on
 * their own - see the long comment on RADIO_I2S_BCLK_GPIO in app_config.h.
 * i2s_stream_idf5.c's i2s_driver_startup() calls the selected audio_board's
 * get_i2s_pins() and memcpy()s the result straight over tx_std_cfg.gpio_cfg,
 * unconditionally, right before i2s_channel_init_std_mode(). The board wins.
 *
 * So the pins are a two-place setting whose halves can drift apart with no
 * compile error and no runtime error - just a dead bus. This chip was
 * driving the stock AtomS3R pins (bck 8 / ws 6 / dout 5 - 5 and 6 present
 * but in SWAPPED roles, so even a scope looks almost-right) until
 * 2026-08-31, and it took disassembling the .elf to find that. The vendored
 * esp-adf tree the fix lives in is shared by every project here, so a
 * re-clone or upstream update reverts it just as silently - which has
 * already happened to ../esp32_wifi_streamer_520kbram and ...-multistation,
 * both of which select LyraT v4.3 whose board file is still stock (bck 5 /
 * ws 25 / dout 26, plus MCLK on GPIO0, a strapping pin).
 *
 * Read the pins back from the same function the driver will call, and
 * refuse to start a pipeline that would quietly clock the wrong GPIOs. */
static esp_err_t check_i2s_pins(void)
{
    board_i2s_pin_t pins = { 0 };
    esp_err_t err = get_i2s_pins(I2S_NUM_0, &pins);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get_i2s_pins(I2S_NUM_0) failed: %s - cannot verify the I2S bus",
                 esp_err_to_name(err));
        return err;
    }

    /* data_in is deliberately unchecked: this is a TX-only master, and IDF
     * gates strictly on handle->dir, so the unused direction is ignored.
     * mclk must be -1 - a board handing back a real MCLK pin (LyraT v4.3
     * returns GPIO0) would claim a GPIO this link neither needs nor wires,
     * and on an S3 a stray output on a strapping pin is worse than useless. */
    bool ok = pins.bck_io_num   == RADIO_I2S_BCLK_GPIO &&
              pins.ws_io_num    == RADIO_I2S_WS_GPIO   &&
              pins.data_out_num == RADIO_I2S_DATA_GPIO &&
              pins.mck_io_num   == -1;

    if (!ok) {
        ESP_LOGE(TAG, "I2S PIN MISMATCH - the selected audio_board's get_i2s_pins() "
                      "does not match app_config.h, and the board is what the hardware obeys.");
        ESP_LOGE(TAG, "  board says : bck=%d ws=%d dout=%d mclk=%d",
                 pins.bck_io_num, pins.ws_io_num, pins.data_out_num, pins.mck_io_num);
        ESP_LOGE(TAG, "  expected   : bck=%d ws=%d dout=%d mclk=-1",
                 RADIO_I2S_BCLK_GPIO, RADIO_I2S_WS_GPIO, RADIO_I2S_DATA_GPIO);
        ESP_LOGE(TAG, "  fix        : esp-adf/components/audio_board/m5stack_atoms3r/board_pins_config.c "
                      "(and check CONFIG_M5STACK_ATOMS3R_BOARD is still set in sdkconfig)");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "I2S pins verified against the audio_board the driver actually uses: "
                  "bck=%d ws=%d dout=%d, no mclk (master/TX)",
             pins.bck_io_num, pins.ws_io_num, pins.data_out_num);
    return ESP_OK;
}

/*
 * PCM tap for the on-board RGB LED (led_viz.h). Installed on the decoder's
 * output with audio_element_set_write_cb(), so it runs inline on the
 * decoder's OWN task exactly where audio_element_output() would otherwise
 * call rb_write() directly - no extra element, no extra task, and no second
 * copy of the stream. The same technique the companion esp32_bt_speaker
 * uses for its I2S level probe, for the same reason: a pass-through element
 * was tried there and its extra task/ring-buffer hop starved a core.
 *
 * This REPLACES the default output behaviour rather than wrapping it, so it
 * must reproduce it exactly - hence the rb_write() below with the untouched
 * buffer/len/ticks. Anything that returns early or alters len here silently
 * corrupts the audio path.
 */
static int led_viz_write_cb(audio_element_handle_t self, char *buffer, int len,
                            TickType_t ticks_to_wait, void *ctx)
{
    if (len > 0) {
        led_viz_feed_pcm(buffer, (size_t)len);
    }
    return rb_write((ringbuf_handle_t)ctx, buffer, len, ticks_to_wait);
}

/*
 * ESP-ADF's own auto-detecting decoder, wired up with every codec the
 * vendored esp-adf-libs ships a decoder for (libesp_codec.a - see
 * esp-adf/components/esp-adf-libs/CMakeLists.txt). esp_decoder_init()
 * sniffs the incoming bytes and dispatches to whichever entry in this list
 * matches, so one element covers MP3, AAC (ADTS), M4A/MP4, MPEG-TS AAC,
 * OGG, Opus, FLAC, WAV, AMR-NB/WB and raw PCM.
 *
 * This is the fallback chain, used for every session->format that is not
 * TUNEIN_FORMAT_HLS_CMAF_AAC. It is NOT used for CMAF, and cannot be:
 * CMAF/fMP4 segments carry their AudioSpecificConfig once in an init
 * segment rather than in every frame, and esp_decoder has no way to be
 * handed one - it was tried on hardware and fails ("This audio is RAW AAC"
 * / "Failed to initialize"), which is the entire reason aac_dec_element.c
 * and fmp4_bridge.c exist. Order in the list is detection preference, so
 * the two formats actually likely to turn up here (AAC and MP3) come first.
 *
 * Unlike the CMAF path, the real sample rate/channel count are not known
 * until the decoder has parsed a frame and reported them - so i2s_writer is
 * created at RADIO_I2S_SAMPLE_RATE and retuned by radio_pipeline_wait()'s
 * AEL_MSG_CMD_REPORT_MUSIC_INFO handler when they arrive. That retune is
 * avoided on the CMAF path for good reason (a resume timeout wedged I2S on
 * hardware), but here there is no init segment to learn the rate from up
 * front, so it is the only option.
 */
static audio_element_handle_t build_generic_decoder(void)
{
    audio_decoder_t decoders[] = {
        DEFAULT_ESP_AAC_DECODER_CONFIG(),
        DEFAULT_ESP_MP3_DECODER_CONFIG(),
        DEFAULT_ESP_M4A_DECODER_CONFIG(),
        DEFAULT_ESP_TS_DECODER_CONFIG(),
        DEFAULT_ESP_OGG_DECODER_CONFIG(),
        DEFAULT_ESP_OPUS_DECODER_CONFIG(),
        DEFAULT_ESP_FLAC_DECODER_CONFIG(),
        DEFAULT_ESP_WAV_DECODER_CONFIG(),
        DEFAULT_ESP_AMRNB_DECODER_CONFIG(),
        DEFAULT_ESP_AMRWB_DECODER_CONFIG(),
        DEFAULT_ESP_PCM_DECODER_CONFIG(),
    };

    esp_decoder_cfg_t cfg = DEFAULT_ESP_DECODER_CONFIG();
    cfg.out_rb_size = RADIO_DECODER_BUFFER_BYTES;
    /* Decode HE-AAC/SBR ("mp4a.40.5") as well as plain AAC-LC. Costs
     * nothing on an LC stream (the flag only enables the SBR path when the
     * bitstream actually signals it) and is the difference between playing
     * and failing on a station that only offers an HE-AAC rendition. */
    cfg.plus_enable = true;
    /* ID3 parsing is for tag metadata this project does nothing with, and
     * it allocates to hold the tags - off. */
    cfg.id3_parse_enable = false;

    audio_element_handle_t el = esp_decoder_init(&cfg, decoders,
                                                 sizeof(decoders) / sizeof(decoders[0]));
    if (!el) {
        ESP_LOGE(TAG, "esp_decoder allocation failed");
    } else {
        ESP_LOGI(TAG, "Using ESP-ADF's auto-detecting decoder (%d codecs: AAC/MP3/M4A/TS/OGG/OPUS/FLAC/WAV/AMR/PCM)",
                 (int)(sizeof(decoders) / sizeof(decoders[0])));
    }
    return el;
}

esp_err_t radio_pipeline_start(tunein_session_t *session)
{
    radio_pipeline_stop();

    /* Before spending a TuneIn resolve and a segment fetch on it: confirm
     * the pins the driver will really use are the ones this project thinks
     * it wired. See check_i2s_pins(). */
    esp_err_t pin_err = check_i2s_pins();
    if (pin_err != ESP_OK) {
        return pin_err;
    }

    ESP_LOGI(TAG, "Creating ADF audio pipeline for format=%d: %s (free heap=%" PRIu32 ")",
             (int)session->format,
             session->format == TUNEIN_FORMAT_HLS_CMAF_AAC
                 ? "HLS -> fMP4/CMAF bridge -> AAC decoder -> I2S out"
                 : "HTTP -> auto-detecting decoder -> I2S out",
             esp_get_free_heap_size());
    audio_pipeline_cfg_t pipe_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipe_cfg);
    if (!pipeline) return ESP_ERR_NO_MEM;

    /* TUNEIN_FORMAT_DIRECT_GENERIC is a single continuous stream, not a
     * playlist of segments: there is nothing for http_stream's playlist
     * parser to parse and no "next track" to advance to, and leaving those
     * on would have it try to interpret raw audio bytes as a manifest. Both
     * HLS formats keep them on, unchanged. */
    const bool is_hls = (session->format != TUNEIN_FORMAT_DIRECT_GENERIC);

    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.type = AUDIO_STREAM_READER;
    http_cfg.enable_playlist_parser = is_hls;
    /* Advance to the next HLS segment automatically when one ends.
     *
     * Without this, http_stream finishes a segment, dispatches
     * HTTP_STREAM_FINISH_TRACK, and returns "done" - the element reports
     * AEL_STATUS_STATE_FINISHED (15) and radio_pipeline_wait() tears the
     * whole session down. That is exactly what the hardware log showed:
     * a full segment decoded cleanly (15.56 s of silent, error-free
     * playback between "Decoded format" and the teardown, matching the
     * playlist's #EXTINF:16.02131), then the pipeline stopped.
     *
     * With it set, ADF opens the next segment URI itself and only fires
     * HTTP_STREAM_FINISH_PLAYLIST once the playlist is exhausted, which
     * http_stream_hook() above handles by re-fetching the live playlist. */
    http_cfg.auto_connect_next_track = is_hls;
    /* 2-SECOND-PAUSE FIX (2026-08-22): every live-window refresh
     * (http_stream_hook() -> http_stream_fetch_again(), see above) re-fetches
     * the manifest and then immediately fetches the segment it names - two
     * requests to the SAME host (itsliveradio.apple.com), back-to-back,
     * every ~2-3 minutes. Without this flag, ESP-ADF's http_stream closes
     * and fully renegotiates a fresh TCP+TLS connection for each of those
     * two requests, which measured ~2s + ~1s on hardware (see hardware log:
     * "No more data" at 200442 to "Decoded format" at 203752) - heard as a
     * silence gap because nothing is buffered to play through it (see
     * RADIO_HTTP_BUFFER_BYTES's own comment: this chip cannot afford bigger
     * buffers, already OOM'd twice trying). This opt-in flag (LOCAL PATCH in
     * esp-adf/components/audio_stream/http_stream.c - grep that file for
     * "LOCAL PATCH") lets the second of those two requests reuse the
     * connection the first one already opened, skipping its TCP+TLS
     * handshake entirely, instead of adding another buffer/allocation this
     * project does not have headroom for. */
    http_cfg.reuse_conn_same_host = true;
    http_cfg.out_rb_size = RADIO_HTTP_BUFFER_BYTES;
    http_cfg.event_handle = http_stream_hook;
    /* HTTP_STREAM_CFG_DEFAULT() leaves crt_bundle_attach NULL - with no
     * verification option set, esp-tls refuses to open any https:// URL at
     * all (a deliberate anti-MITM safeguard, not a flaky failure) - see the
     * single-chip project's history of this exact failure. */
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_reader = http_stream_init(&http_cfg);

    /* THE ELEMENT CHAIN IS CHOSEN HERE, from what tunein_start_session()
     * actually found on the wire (session->format, see tunein_control.h):
     *
     *   TUNEIN_FORMAT_HLS_CMAF_AAC  http -> fmp4_bridge -> aac_dec_element -> i2s
     *   everything else             http -> esp_decoder                     -> i2s
     *
     * The CMAF chain is the primary one and is unchanged - it exists
     * because ESP-ADF's stock decoders cannot be TOLD an
     * AudioSpecificConfig, and CMAF/fMP4 segments do not carry one per
     * frame (see the comment on aac_dec_element below). The generic chain
     * is the fallback for everything self-describing, where sniffing works
     * and there is nothing to prime. */
    if (session->format == TUNEIN_FORMAT_HLS_CMAF_AAC) {
        fmp4_bridge_cfg_t fmp4_cfg = FMP4_BRIDGE_CFG_DEFAULT();
        /* FMP4_BRIDGE_CFG_DEFAULT() leaves this at the element's own 8KB default;
         * override it like the other two stages so all three buffer depths are
         * decided in one place - see RADIO_FMP4_BUFFER_BYTES in app_config.h. */
        fmp4_cfg.out_rb_size = RADIO_FMP4_BUFFER_BYTES;
        fmp4_bridge_el = fmp4_bridge_init(&fmp4_cfg);
        if (!fmp4_bridge_el) {
            ESP_LOGE(TAG, "fmp4_bridge allocation failed");
            radio_pipeline_stop();
            return ESP_ERR_NO_MEM;
        }

        /* Parse the CMAF init segment BEFORE creating the decoder: it is what
         * yields the real sample rate/channel count, and the decoder below is
         * configured from those rather than from a guess. */
        esp_err_t init_err = fmp4_bridge_set_init_segment(fmp4_bridge_el, session->init_segment,
                                                          session->init_segment_len);
        if (init_err != ESP_OK) {
            ESP_LOGE(TAG, "Could not parse CMAF init segment: %s", esp_err_to_name(init_err));
            radio_pipeline_stop();
            return init_err;
        }

        /* Our own decoder element over esp_aac_dec, TOLD the format instead of
         * sniffing it. Both of ESP-ADF's stock paths were tried on hardware and
         * both failed on a stream proven valid offline (tools/fmp4_to_adts.py
         * output plays as "aac (LC), 48000 Hz, stereo, 252 kb/s"):
         *   aac_decoder_init()  -> "This audio is RAW AAC" / "Failed to initialize"
         *   esp_decoder_init()  -> "Detect audio type is AAC" (correct!) but then
         *                          delegates to the same aac_decoder, same failure
         * RAW mode needs an AudioSpecificConfig neither element can be given.
         * We already parse exactly that from the CMAF init segment, so hand it
         * over explicitly - see aac_dec_element.h. (This is also exactly why
         * the generic esp_decoder path below is a FALLBACK and not the
         * default: it cannot be told anything, so it only works on streams
         * that describe themselves frame by frame.)
         *
         * The rate/channels come from the init segment the session just fetched
         * (fmp4_bridge parsed it as AAC-LC / freq_index=3 / 2ch = 48000 Hz), not
         * from a hardcoded guess. */
        aac_dec_element_cfg_t dec_cfg = AAC_DEC_ELEMENT_CFG_DEFAULT();
        dec_cfg.out_rb_size = RADIO_DECODER_BUFFER_BYTES;
        dec_cfg.sample_rate = fmp4_bridge_get_sample_rate(fmp4_bridge_el);
        dec_cfg.channels    = fmp4_bridge_get_channels(fmp4_bridge_el);
        /* fmp4_bridge emits 7-byte ADTS headers, which is also what gives the
         * decoder frame boundaries across the ring buffer between the two
         * elements - so ADTS framing stays on. */
        dec_cfg.no_adts_header = false;
        aac_decoder = aac_dec_element_init(&dec_cfg);
        decoder_el = aac_decoder;
    } else {
        generic_decoder = build_generic_decoder();
        decoder_el = generic_decoder;
    }

    /* I2S MASTER (this chip drives BCLK/WS/DOUT) - the companion
     * esp32_bt_speaker chip just listens as slave. Pins from app_config.h,
     * must match that project's pins and the physical wiring exactly. */
    /* Configure the bus at the stream's ACTUAL rate up front. The init
     * segment was parsed above, so the real rate (48000 for this stream) is
     * already known - there is no reason to start at RADIO_I2S_SAMPLE_RATE
     * and retune later. Retuning mid-run is in fact harmful: it pauses and
     * resumes the element, and on hardware that resume timed out -
     * "AUDIO_ELEMENT: [i2s-...] RESUME timeout" - leaving I2S wedged so no
     * samples reached the pins even though the stream kept downloading. */
    /* Only the CMAF path knows the real rate up front (from the init
     * segment). On the generic path nothing has parsed a frame yet, so the
     * bus starts at RADIO_I2S_SAMPLE_RATE and the music-info handler in
     * radio_pipeline_wait() retunes it once the decoder reports the truth. */
    int stream_rate = fmp4_bridge_el ? fmp4_bridge_get_sample_rate(fmp4_bridge_el) : 0;
    int stream_ch   = fmp4_bridge_el ? fmp4_bridge_get_channels(fmp4_bridge_el) : 0;
    if (stream_rate <= 0) stream_rate = RADIO_I2S_SAMPLE_RATE;
    if (stream_ch   <= 0) stream_ch   = 2;
    ESP_LOGI(TAG, "Configuring I2S master at %d Hz, %d ch (%s)",
             stream_rate, stream_ch,
             fmp4_bridge_el ? "from CMAF init segment" : "provisional - retuned when the decoder reports the real format");
    i2s_configured_rate = stream_rate;
    i2s_configured_ch = stream_ch;
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(
        I2S_NUM_0, stream_rate, I2S_DATA_BIT_WIDTH_16BIT, AUDIO_STREAM_WRITER);
    i2s_cfg.chan_cfg.role = I2S_ROLE_MASTER;
    i2s_cfg.std_cfg.gpio_cfg.bclk = RADIO_I2S_BCLK_GPIO;
    i2s_cfg.std_cfg.gpio_cfg.ws = RADIO_I2S_WS_GPIO;
    i2s_cfg.std_cfg.gpio_cfg.dout = RADIO_I2S_DATA_GPIO;
    i2s_cfg.std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    i2s_cfg.std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    /* Deliberately NOT setting i2s_cfg.out_rb_size: i2s_writer is the last
     * element in the chain below, and audio_pipeline_link() only creates an
     * output ring buffer for non-last elements - a last element gets an input
     * ringbuf only. Raising it here would allocate nothing and mislead.
     * i2s_writer's effective input buffer is the AAC decoder's output
     * ringbuffer (RADIO_DECODER_BUFFER_BYTES). */
    /* DIAGNOSTIC (slow-playback investigation): log exactly what clock
     * parameters get handed to the driver, not just the nominal rate above.
     * bclk_div/mclk_multiple affect how precisely the requested sample_rate_hz
     * is actually achievable - if the real bus ends up running measurably
     * off 48000 Hz, it would explain audio drifting slower/faster than
     * source without any resampling being involved at all. Also bracket the
     * element allocation with heap/largest-block so a fragmentation-driven
     * short allocation (smaller buffer than requested, silently) is visible
     * here rather than inferred later. */
    ESP_LOGI(TAG, "I2S clk_cfg: sample_rate_hz=%" PRIu32 " clk_src=%d mclk_multiple=%d bclk_div=%" PRIu32 " | "
             "slot_cfg: data_bit_width=%d slot_bit_width=%d slot_mode=%d | free heap=%" PRIu32 ", largest block=%" PRIu32,
             i2s_cfg.std_cfg.clk_cfg.sample_rate_hz, (int)i2s_cfg.std_cfg.clk_cfg.clk_src,
             (int)i2s_cfg.std_cfg.clk_cfg.mclk_multiple, i2s_cfg.std_cfg.clk_cfg.bclk_div,
             (int)i2s_cfg.std_cfg.slot_cfg.data_bit_width, (int)i2s_cfg.std_cfg.slot_cfg.slot_bit_width,
             (int)i2s_cfg.std_cfg.slot_cfg.slot_mode,
             esp_get_free_heap_size(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    i2s_writer = i2s_stream_init(&i2s_cfg);
    ESP_LOGI(TAG, "I2S element created: %p | free heap=%" PRIu32 ", largest block=%" PRIu32,
             i2s_writer, esp_get_free_heap_size(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    if (!http_reader || !decoder_el || !i2s_writer) {
        ESP_LOGE(TAG, "Pipeline element allocation failed (HTTP=%p FMP4=%p DEC=%p I2S=%p)",
                 http_reader, fmp4_bridge_el, decoder_el, i2s_writer);
        radio_pipeline_stop();
        return ESP_ERR_NO_MEM;
    }

    /* fmp4_bridge only exists on the CMAF path, so the chain is 4 elements
     * there and 3 on the generic one - registered and linked accordingly
     * rather than always assuming the bridge is present. */
    ESP_ERROR_CHECK(audio_pipeline_register(pipeline, http_reader, "hls"));
    if (fmp4_bridge_el) {
        ESP_ERROR_CHECK(audio_pipeline_register(pipeline, fmp4_bridge_el, "fmp4"));
    }
    ESP_ERROR_CHECK(audio_pipeline_register(pipeline, decoder_el, "dec"));
    ESP_ERROR_CHECK(audio_pipeline_register(pipeline, i2s_writer, "i2s"));
    if (fmp4_bridge_el) {
        const char *links[] = {"hls", "fmp4", "dec", "i2s"};
        ESP_ERROR_CHECK(audio_pipeline_link(pipeline, links, 4));
    } else {
        const char *links[] = {"hls", "dec", "i2s"};
        ESP_ERROR_CHECK(audio_pipeline_link(pipeline, links, 3));
    }

    /* Tap the decoder's PCM for the LED visualiser. Must happen AFTER
     * audio_pipeline_link() above: that call is what creates the ring buffer
     * between the decoder and i2s_writer, and the callback needs that same
     * buffer as its write target so the data path is unchanged. Decorative,
     * so a missing ring buffer is a warning, never a failure. */
    ringbuf_handle_t decoder_out_rb = audio_element_get_output_ringbuf(decoder_el);
    if (decoder_out_rb) {
        esp_err_t tap_err = audio_element_set_write_cb(decoder_el, led_viz_write_cb, decoder_out_rb);
        if (tap_err != ESP_OK) {
            ESP_LOGW(TAG, "Could not tap decoder output for the LED visualiser: %s "
                     "(audio unaffected; the LED just will not react)", esp_err_to_name(tap_err));
        }
    } else {
        ESP_LOGW(TAG, "Decoder has no output ring buffer; LED visualiser will not react");
    }

    /* The resolved media playlist (not the HLS master), or the direct
     * stream URL for TUNEIN_FORMAT_DIRECT_GENERIC - see tunein_control.h. */
    audio_element_set_uri(http_reader, session->hls_variant_url);
    tunein_log_url("ADF input URI", session->hls_variant_url, false);

    /* Own copy: session is freed by main.c right after this function
     * returns, but radio_pipeline_wait()'s loop needs this URL for as long
     * as the pipeline runs, to prefetch the next live window ahead of the
     * boundary. Freed in radio_pipeline_stop(). */
    free(cached_playlist_url);
    cached_playlist_url = NULL;
    /* Prefetching re-fetches a live HLS playlist ahead of its window
     * boundary - meaningless for a direct stream, which has no playlist and
     * no boundary. Leaving this NULL is what service_playlist_prefetch()
     * already treats as "nothing to prefetch". */
    if (is_hls) {
        cached_playlist_url = strdup(session->hls_variant_url);
        if (!cached_playlist_url) {
            ESP_LOGW(TAG, "Could not copy playlist URL for prefetching (OOM); "
                     "live-window boundaries will fall back to the reactive refresh only");
        }
    }

    audio_event_iface_cfg_t event_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    event_iface = audio_event_iface_init(&event_cfg);
    if (!event_iface) {
        ESP_LOGE(TAG, "Could not allocate ADF event interface");
        radio_pipeline_stop();
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(audio_pipeline_set_listener(pipeline, event_iface));

    /* Fold the AVRCP doorbell into the SAME FreeRTOS queue set the listen
     * below blocks on, so a button press unblocks radio_pipeline_wait()
     * immediately instead of waiting out a timeout. This is ESP-ADF's own
     * pattern for non-element event sources (every example does exactly
     * this with esp_periph_set_get_event_iface()).
     *
     * Order matters: audio_pipeline_set_listener() above rebuilds
     * event_iface's queue set from scratch, so this has to come after it or
     * it would be thrown away. */
    audio_event_iface_handle_t avrcp_evt = avrcp_uart_get_event_iface();
    if (avrcp_evt) {
        esp_err_t listen_err = audio_event_iface_set_listener(avrcp_evt, event_iface);
        if (listen_err != ESP_OK) {
            ESP_LOGW(TAG, "Could not add the AVRCP doorbell to the pipeline event set: %s "
                     "(station changes still work - they are taken from the command queue on "
                     "the next event or prefetch tick - but are no longer instant)",
                     esp_err_to_name(listen_err));
        }
    }

    /* Second, independent command source - see console_cli.h. Same pattern,
     * same ordering requirement (must come after audio_pipeline_set_listener()
     * above, which rebuilds event_iface's queue set from scratch). */
    audio_event_iface_handle_t console_evt = console_cli_get_event_iface();
    if (console_evt) {
        esp_err_t listen_err = audio_event_iface_set_listener(console_evt, event_iface);
        if (listen_err != ESP_OK) {
            ESP_LOGW(TAG, "Could not add the console doorbell to the pipeline event set: %s "
                     "(next/prev/status still work from the console - taken from the command "
                     "queue on the next event or prefetch tick - but are no longer instant)",
                     esp_err_to_name(listen_err));
        }
    }

    esp_err_t err = audio_pipeline_run(pipeline);
    ESP_LOGI(TAG, "audio_pipeline_run returned: %s (playback continues in the background; "
             "call radio_pipeline_wait() to block until it needs replacing)", esp_err_to_name(err));
    return err;
}

static bool is_terminal_status(int status)
{
    switch (status) {
    case AEL_STATUS_ERROR_OPEN:
    case AEL_STATUS_ERROR_INPUT:
    case AEL_STATUS_ERROR_PROCESS:
    case AEL_STATUS_ERROR_OUTPUT:
    case AEL_STATUS_ERROR_CLOSE:
    case AEL_STATUS_ERROR_TIMEOUT:
    case AEL_STATUS_ERROR_UNKNOWN:
    case AEL_STATUS_STATE_STOPPED:
    case AEL_STATUS_STATE_FINISHED:
        return true;
    default:
        return false;
    }
}

static const char *element_name(audio_element_handle_t el)
{
    if (el == http_reader) return "hls";
    if (fmp4_bridge_el && el == fmp4_bridge_el) return "fmp4";
    if (decoder_el && el == decoder_el) return "dec";
    if (el == i2s_writer) return "i2s";
    return "?";
}

/* Live-window prefetch, driven from radio_pipeline_wait()'s own loop (see
 * playlist_prefetch.h for the full mechanism/why). Runs every iteration -
 * the loop's wait_ticks is capped below so this gets a chance to run at
 * least once a second even when no ADF event arrives. Both steps are best-
 * effort: any failure just leaves the existing reactive HTTP_STREAM_FINISH_
 * PLAYLIST path (http_stream_hook() above) to handle that boundary exactly
 * as before this feature existed - nothing here can make a boundary worse,
 * only better. */
static void service_playlist_prefetch(void)
{
    playlist_prefetch_result_t *result = NULL;
    if (playlist_prefetch_poll(&result)) {
        if (result->count > 0 && http_reader) {
            esp_err_t err = http_stream_playlist_insert_tracks(http_reader, result->uris, result->count);
            ESP_LOGI(TAG, "Prefetch: inserted %d segment(s) ahead of the live-window boundary (%s)",
                     result->count, esp_err_to_name(err));
        } else {
            /* Failed or found nothing new - cool down instead of retrying
             * next poll. See PLAYLIST_PREFETCH_RETRY_BACKOFF_MS's comment. */
            next_prefetch_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(PLAYLIST_PREFETCH_RETRY_BACKOFF_MS);
        }
        playlist_prefetch_result_free(result);
    }

    if (!http_reader || !cached_playlist_url || playlist_prefetch_is_in_flight()) {
        return;
    }

    int remaining = -1;
    if (http_stream_get_tracks_remaining(http_reader, &remaining) != ESP_OK || remaining < 0) {
        return;
    }
    if (remaining > 1) {
        /* Fresh window (either a prior prefetch topped it up, or the
         * reactive fallback just re-resolved) - reset the per-boundary
         * attempt count so the NEXT boundary gets its own fair attempts. */
        prefetch_attempts_this_window = 0;
        return;
    }
    /* remaining is 0 or 1 here - see http_stream_get_tracks_remaining()'s
     * doc comment: still the entire last known segment's ~16s to work with. */
    if (prefetch_attempts_this_window >= PLAYLIST_PREFETCH_MAX_ATTEMPTS_PER_WINDOW) {
        return; /* already tried enough for this boundary; let the reactive fallback take it */
    }
    TickType_t now = xTaskGetTickCount();
    if ((int32_t)(now - next_prefetch_retry_tick) < 0) {
        return; /* cooling down after a recent failure */
    }

    prefetch_attempts_this_window++;
    if (playlist_prefetch_start(cached_playlist_url) == ESP_OK) {
        ESP_LOGI(TAG, "Prefetch: only %d segment(s) left in the current window (attempt %d/%d); "
                 "fetching the next live playlist now, ahead of the boundary",
                 remaining, prefetch_attempts_this_window, PLAYLIST_PREFETCH_MAX_ATTEMPTS_PER_WINDOW);
    } else {
        next_prefetch_retry_tick = now + pdMS_TO_TICKS(PLAYLIST_PREFETCH_RETRY_BACKOFF_MS);
    }
}

esp_err_t radio_pipeline_wait(TickType_t max_session_ticks, avrcp_cmd_t *out_station_cmd, bool *out_calibrate)
{
    if (!event_iface) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_station_cmd) {
        *out_station_cmd = AVRCP_CMD_NONE;
    }
    if (out_calibrate) {
        *out_calibrate = false;
    }

    bool has_deadline = (max_session_ticks != portMAX_DELAY);
    TickType_t deadline = has_deadline ? xTaskGetTickCount() + max_session_ticks : 0;
    /* Caps how long a single audio_event_iface_listen() call can block, so
     * service_playlist_prefetch() above still gets to run roughly once a
     * second even during a long quiet stretch with no ADF events. */
    const TickType_t prefetch_poll_ticks = pdMS_TO_TICKS(1000);

    while (true) {
        /* Taken before the prefetcher and before blocking: if a station
         * change is already pending there is no point servicing a prefetch
         * for a session that is about to be torn down.
         *
         * This is NOT a poll - avrcp_uart's reader task queues the command
         * the instant the bytes arrive and then rings its doorbell, which is
         * a member of the very queue set audio_event_iface_listen() blocks
         * on below (registered in radio_pipeline_start). So a press wakes
         * this loop straight away and is taken here on the next pass; the
         * timeout below only exists for the prefetcher. */
        avrcp_cmd_t pending = avrcp_uart_take_command(0);
        if (pending != AVRCP_CMD_NONE) {
            ESP_LOGI(TAG, "AVRCP station-change command received; ending this session for a station switch");
            if (out_station_cmd) {
                *out_station_cmd = pending;
            }
            return ESP_OK;   /* healthy session, deliberately ended - no error backoff */
        }

        /* Second, independent source of the same next/previous request, PLUS
         * the console's `cal` (latency calibration) request - see
         * console_cli.h. next/prev is mapped onto the avrcp_cmd_t the caller
         * already understands rather than growing out_station_cmd's type: to
         * every consumer downstream of this function, "switch station" means
         * exactly the same thing regardless of which UI asked for it. `cal`
         * is NOT a station change - it is reported through the separate
         * out_calibrate flag instead, mutually exclusive with
         * out_station_cmd (see radio_pipeline.h's doc comment). Any other
         * console_cmd_t value (the CAL_BEEP/HEARD/ACCEPT/CANCEL family) is
         * never actually enqueued here in the first place - console_cli.c's
         * handlers for those refuse outside an active calibration session,
         * which is the only thing that ever drains them - but ignoring
         * anything unrecognized rather than assuming NEXT/PREV/CAL_ENTER is
         * exhaustive costs nothing and fails safe if that enum ever grows. */
        console_cmd_t console_pending = console_cli_take_command(0);
        avrcp_cmd_t mapped = AVRCP_CMD_NONE;
        bool console_wants_calibrate = false;
        switch (console_pending) {
        case CONSOLE_CMD_NEXT:      mapped = AVRCP_CMD_NEXT; break;
        case CONSOLE_CMD_PREV:      mapped = AVRCP_CMD_PREV; break;
        case CONSOLE_CMD_CAL_ENTER: console_wants_calibrate = true; break;
        default:                    break; /* CONSOLE_CMD_NONE, or a stray CAL_BEEP/HEARD/ACCEPT/CANCEL - ignore */
        }

        if (console_wants_calibrate) {
            ESP_LOGI(TAG, "Console latency-calibration request received; ending this session");
            if (out_calibrate) {
                *out_calibrate = true;
            }
            return ESP_OK;   /* healthy session, deliberately ended - no error backoff */
        }

        if (mapped != AVRCP_CMD_NONE) {
            ESP_LOGI(TAG, "Console station-change command received; ending this session for a station switch");
            if (out_station_cmd) {
                *out_station_cmd = mapped;
            }
            return ESP_OK;   /* healthy session, deliberately ended - no error backoff */
        }

        service_playlist_prefetch();

        TickType_t wait_ticks = prefetch_poll_ticks;
        if (has_deadline) {
            TickType_t now = xTaskGetTickCount();
            if (now >= deadline) {
                ESP_LOGI(TAG, "Proactive session refresh: max session duration reached");
                return ESP_OK;
            }
            TickType_t remaining_ticks = deadline - now;
            if (remaining_ticks < wait_ticks) {
                wait_ticks = remaining_ticks;
            }
        }

        audio_event_iface_msg_t msg;
        esp_err_t err = audio_event_iface_listen(event_iface, &msg, wait_ticks);
        if (err == ESP_ERR_TIMEOUT) {
            continue; /* re-check the deadline/prefetch state (or spurious wakeup); keep waiting */
        }
        if (err != ESP_OK) {
            /* 2026-08-22: checked ADF's own audio_event_iface.c directly -
             * audio_event_iface_listen() returns plain ESP_FAIL for BOTH a
             * genuine timeout AND a real failure; there is no distinct
             * timeout code in this version, so the ESP_ERR_TIMEOUT check
             * above was always dead code here. Before wait_ticks was capped
             * (see prefetch_poll_ticks above), this call almost never
             * actually timed out in practice - ADF's own elements send
             * status/music-info events well within the old ~20-minute
             * session deadline. Now it times out ~once/second by design
             * (that's what drives service_playlist_prefetch()'s polling),
             * so logging this as a WARNing every second was pure false-
             * alarm noise, not a real signal - silently loop instead. */
            continue;
        }

        if (msg.source_type != AUDIO_ELEMENT_TYPE_ELEMENT) {
            continue;
        }

        /* The decoder reports the stream's real sample rate/width/channels
         * once it has parsed a frame; the I2S clock has to be retuned to
         * match or output plays at the wrong pitch/speed. This is not
         * hypothetical for this stream: the CMAF init segment parses as
         * freq_index=3 (48 kHz), while RADIO_I2S_SAMPLE_RATE only sets the
         * bus up at 44.1 kHz initially - an ~8.8% error left uncorrected.
         * Without this the message was silently dropped by the filter below. */
        if (msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO &&
            decoder_el && (audio_element_handle_t)msg.source == decoder_el) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(decoder_el, &music_info);
            /* The I2S element was already created at this rate from the init
             * segment, so normally there is nothing to do. Only retune on a
             * genuine mismatch: i2s_stream_set_clk() pauses and resumes the
             * element, and that resume has been observed timing out on
             * hardware ("[i2s-...] RESUME timeout"), which wedges the output.
             * Never pay that cost just to set the value it already has. */
            if (music_info.sample_rates == i2s_configured_rate &&
                music_info.channels == i2s_configured_ch) {
                ESP_LOGI(TAG, "Decoder confirms %d Hz, %d ch - I2S already configured, no retune",
                         music_info.sample_rates, music_info.channels);
                continue;
            }
            ESP_LOGW(TAG, "Decoder reports %d Hz, %d bit, %d ch but I2S was set up for %d Hz, %d ch; retuning",
                     music_info.sample_rates, music_info.bits, music_info.channels,
                     i2s_configured_rate, i2s_configured_ch);
            if (music_info.sample_rates > 0 && music_info.channels > 0) {
                esp_err_t clk_err = i2s_stream_set_clk(i2s_writer, music_info.sample_rates,
                                                       music_info.bits, music_info.channels);
                if (clk_err != ESP_OK) {
                    ESP_LOGW(TAG, "i2s_stream_set_clk failed: %s", esp_err_to_name(clk_err));
                } else {
                    i2s_configured_rate = music_info.sample_rates;
                    i2s_configured_ch = music_info.channels;
                }
            }
            continue;
        }

        if (msg.cmd != AEL_MSG_CMD_REPORT_STATUS) {
            continue;
        }

        int status = (int)(intptr_t)msg.data;
        ESP_LOGD(TAG, "ADF status: element=%s status=%d", element_name((audio_element_handle_t)msg.source), status);

        /* A live HLS window is finite by design: ADF plays every segment the
         * playlist listed, then http_stream_hook() re-points the element at
         * the playlist URL (log: "HTTP_STREAM: Fetching again ...") and the
         * element ends with AEL_STATUS_STATE_FINISHED. That is a normal
         * refresh boundary, NOT a failure - on hardware it arrived after 10
         * segments / 5515161 bytes of clean continuous playback. Reporting it
         * as ESP_FAIL made main.c apply its exponential error backoff, which
         * is exactly wrong here. Return ESP_OK so the caller rebuilds the
         * session immediately with no penalty. */
        if (status == AEL_STATUS_STATE_FINISHED &&
            (audio_element_handle_t)msg.source == http_reader) {
            /* Restart the pipeline IN PLACE rather than returning, which
             * would make main.c re-resolve the whole TuneIn chain (profile
             * ~61KB + Tune.ashx + master + variant + init segment) - 10-15 s
             * of silence every ~2.7 minutes, which is what "it doesn't last
             * long" actually was.
             *
             * None of that re-resolution is needed: http_stream_hook() has
             * already pointed the element back at the still-signed playlist
             * URL and cleared is_playlist_resolved, and _http_close() drops
             * the stale playlist and zeroes byte_pos. So re-running the same
             * elements re-fetches the playlist, picks up the newly published
             * segments, and continues - a ~1-2 s gap instead of 10-15 s.
             * fmp4_bridge keeps its parsed AudioSpecificConfig across close(),
             * so the decoder stays correctly configured too.
             *
             * 2026-09-04: narrowed from a whole-pipeline audio_pipeline_stop()
             * / reset / audio_pipeline_run() to touching ONLY http_reader and
             * fmp4_bridge_el - see [[esp32-wifi-streamer-aac-heap-crash]].
             * The whole-pipeline version called audio_pipeline_stop() on
             * every registered element, including decoder_el - which runs
             * THIS element's close() callback, aac_dec_close() ->
             * esp_aac_dec_close() -> PVMP4AudioDecoderDeInit() -> free(), a
             * closed-source ESP-ADF-libs call proven on hardware (5 separate
             * crashes, each addr2line-confirmed to this exact chain) to
             * corrupt the heap on close on this build. That call is
             * completely unnecessary here: the stream format (48kHz/2ch
             * AAC-LC) never changes across a live-window boundary, so
             * decoder_el and i2s_writer don't need to be touched at all -
             * they simply block on their input ring buffer while http_reader
             * is between fetches, exactly like any ordinary segment-to-
             * segment gap. Only http_reader (has to re-fetch the playlist)
             * and fmp4_bridge_el (its box/fragment parser state is tied to
             * the specific byte stream that just ended) need to reset; both
             * of their close() callbacks are plain project code with no
             * closed-source calls in them, so this can never reach the
             * decoder-close crash. The "fmp4" ring buffer sits between
             * fmp4_bridge_el and decoder_el and is reset below along with
             * "hls" - decoder_el itself is never stopped, so it just blocks
             * until fresh ADTS bytes appear there again. */
            ESP_LOGI(TAG, "HLS live window exhausted; restarting HLS reader + fMP4 bridge in place "
                     "(decoder/I2S left running, not closed), free heap=%" PRIu32, esp_get_free_heap_size());
            audio_element_stop(http_reader);
            audio_element_wait_for_stop(http_reader);
            audio_element_reset_input_ringbuf(http_reader);
            audio_element_reset_output_ringbuf(http_reader);
            audio_element_reset_state(http_reader);
            if (fmp4_bridge_el) {
                audio_element_stop(fmp4_bridge_el);
                audio_element_wait_for_stop(fmp4_bridge_el);
                audio_element_reset_input_ringbuf(fmp4_bridge_el);
                audio_element_reset_output_ringbuf(fmp4_bridge_el);
                audio_element_reset_state(fmp4_bridge_el);
            }
            /* Same run()-then-resume() sequence audio_pipeline_run() itself
             * uses internally for every element (see esp-adf's
             * audio_pipeline.c) - reproduced here for just these two. */
            esp_err_t rerun = audio_element_run(http_reader);
            if (rerun == ESP_OK && fmp4_bridge_el) {
                rerun = audio_element_run(fmp4_bridge_el);
            }
            if (rerun == ESP_OK) {
                rerun = audio_element_resume(http_reader, 0, pdMS_TO_TICKS(2000));
            }
            if (rerun == ESP_OK && fmp4_bridge_el) {
                rerun = audio_element_resume(fmp4_bridge_el, 0, pdMS_TO_TICKS(2000));
            }
            /* audio_element_stop()/wait_for_stop() above make http_reader and
             * fmp4_bridge_el report AEL_STATUS_STATE_STOPPED to this same
             * event_iface as part of their own normal stop sequence - self-
             * inflicted echoes of the stop just issued, not a new failure
             * (this is what used to send is_terminal_status() a stale
             * status and force a full TuneIn re-resolve on every boundary -
             * see git history). decoder_el/i2s_writer were never stopped, so
             * unlike the old whole-pipeline version there are only two
             * elements' worth of echoes to drain here, not four. */
            audio_event_iface_discard(event_iface);
            if (rerun != ESP_OK) {
                ESP_LOGW(TAG, "In-place restart failed (%s); falling back to a full session refresh",
                         esp_err_to_name(rerun));
                return ESP_OK;   /* clean refresh, no error backoff */
            }
            /* Deadline is deliberately NOT extended here: RADIO_SESSION_MAX_MS
             * still bounds how long we run on one set of signed URLs. */
            continue;
        }

        if (is_terminal_status(status)) {
            ESP_LOGW(TAG, "Pipeline element '%s' reported status=%d; session needs replacing",
                     element_name((audio_element_handle_t)msg.source), status);
            return ESP_FAIL;
        }
    }
}

void radio_pipeline_stop(void)
{
    /* Discard any prefetch result left over from THIS session before
     * tearing it down, so it can never be mistaken for the NEXT session's
     * and inserted into a new http_reader for a different playlist URL -
     * see playlist_prefetch_reset()'s own comment. Done unconditionally,
     * even if pipeline is already NULL below, since a prefetch can complete
     * right around teardown. */
    playlist_prefetch_reset();
    free(cached_playlist_url);
    cached_playlist_url = NULL;
    prefetch_attempts_this_window = 0;
    next_prefetch_retry_tick = 0;

    if (!pipeline) return;
    ESP_LOGW(TAG, "Stopping audio pipeline for refresh/recovery");
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);
    audio_pipeline_unregister(pipeline, http_reader);
    /* Only ever registered on the CMAF path (see radio_pipeline_start) -
     * unregistering/deiniting a NULL handle would fault. */
    if (fmp4_bridge_el) {
        audio_pipeline_unregister(pipeline, fmp4_bridge_el);
    }
    if (decoder_el) {
        audio_pipeline_unregister(pipeline, decoder_el);
    }
    audio_pipeline_unregister(pipeline, i2s_writer);
    if (event_iface) {
        /* Drop the AVRCP + console doorbells before the listener they point
         * into goes away - audio_event_iface_destroy() frees the queue set
         * these sources were added to. */
        audio_event_iface_handle_t avrcp_evt = avrcp_uart_get_event_iface();
        if (avrcp_evt) {
            audio_event_iface_remove_listener(event_iface, avrcp_evt);
        }
        audio_event_iface_handle_t console_evt = console_cli_get_event_iface();
        if (console_evt) {
            audio_event_iface_remove_listener(event_iface, console_evt);
        }
        audio_pipeline_remove_listener(pipeline);
        audio_event_iface_destroy(event_iface);
        event_iface = NULL;
    }
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(http_reader);
    if (fmp4_bridge_el) {
        audio_element_deinit(fmp4_bridge_el);
    }
    if (decoder_el) {
        audio_element_deinit(decoder_el);
    }
    /* Unlike the single-chip project's bt_writer, i2s_writer has no
     * "can only be created once per process" restriction - safe to fully
     * deinit and recreate fresh every session. */
    audio_element_deinit(i2s_writer);
    pipeline = NULL;
    http_reader = NULL;
    fmp4_bridge_el = NULL;
    aac_decoder = NULL;
    generic_decoder = NULL;
    decoder_el = NULL;
    i2s_writer = NULL;

    ESP_LOGI(TAG, "Pipeline torn down; free heap=%" PRIu32, esp_get_free_heap_size());
}

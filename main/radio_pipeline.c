#include "radio_pipeline.h"

#include <inttypes.h>
#include <stdint.h>

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
#include "aac_dec_element.h"

#include "app_config.h"
#include "fmp4_bridge.h"
#include "tunein_control.h"

static const char *TAG = "PIPELINE";
static audio_pipeline_handle_t pipeline;
static audio_element_handle_t http_reader;
static audio_element_handle_t fmp4_bridge_el;
static audio_element_handle_t aac_decoder;
static audio_element_handle_t i2s_writer;
static audio_event_iface_handle_t event_iface;
/* What i2s_writer was actually created with, so the music-info
 * handler can tell 'already correct' from a real mismatch. */
static int i2s_configured_rate;
static int i2s_configured_ch;

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

esp_err_t radio_pipeline_start(tunein_session_t *session)
{
    radio_pipeline_stop();

    ESP_LOGI(TAG, "Creating ADF audio pipeline: HLS -> fMP4/CMAF bridge -> AAC decoder -> I2S out (free heap=%" PRIu32 ")",
             esp_get_free_heap_size());
    audio_pipeline_cfg_t pipe_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipe_cfg);
    if (!pipeline) return ESP_ERR_NO_MEM;

    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.type = AUDIO_STREAM_READER;
    http_cfg.enable_playlist_parser = true;
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
    http_cfg.auto_connect_next_track = true;
    http_cfg.out_rb_size = RADIO_HTTP_BUFFER_BYTES;
    http_cfg.event_handle = http_stream_hook;
    /* HTTP_STREAM_CFG_DEFAULT() leaves crt_bundle_attach NULL - with no
     * verification option set, esp-tls refuses to open any https:// URL at
     * all (a deliberate anti-MITM safeguard, not a flaky failure) - see the
     * single-chip project's history of this exact failure. */
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_reader = http_stream_init(&http_cfg);

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
    esp_err_t err = fmp4_bridge_set_init_segment(fmp4_bridge_el, session->init_segment,
                                                 session->init_segment_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not parse CMAF init segment: %s", esp_err_to_name(err));
        radio_pipeline_stop();
        return err;
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
     * over explicitly - see aac_dec_element.h.
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
    int stream_rate = fmp4_bridge_get_sample_rate(fmp4_bridge_el);
    int stream_ch   = fmp4_bridge_get_channels(fmp4_bridge_el);
    if (stream_rate <= 0) stream_rate = RADIO_I2S_SAMPLE_RATE;
    if (stream_ch   <= 0) stream_ch   = 2;
    ESP_LOGI(TAG, "Configuring I2S master at %d Hz, %d ch (from init segment)",
             stream_rate, stream_ch);
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

    if (!http_reader || !fmp4_bridge_el || !aac_decoder || !i2s_writer) {
        ESP_LOGE(TAG, "Pipeline element allocation failed (HTTP=%p FMP4=%p AAC=%p I2S=%p)",
                 http_reader, fmp4_bridge_el, aac_decoder, i2s_writer);
        radio_pipeline_stop();
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(audio_pipeline_register(pipeline, http_reader, "hls"));
    ESP_ERROR_CHECK(audio_pipeline_register(pipeline, fmp4_bridge_el, "fmp4"));
    ESP_ERROR_CHECK(audio_pipeline_register(pipeline, aac_decoder, "aac"));
    ESP_ERROR_CHECK(audio_pipeline_register(pipeline, i2s_writer, "i2s"));
    const char *links[] = {"hls", "fmp4", "aac", "i2s"};
    ESP_ERROR_CHECK(audio_pipeline_link(pipeline, links, 4));

    /* The AAC-LC media playlist, not the HLS master - see tunein_control.h. */
    audio_element_set_uri(http_reader, session->hls_variant_url);
    tunein_log_url("ADF HLS input URI", session->hls_variant_url, false);

    audio_event_iface_cfg_t event_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    event_iface = audio_event_iface_init(&event_cfg);
    if (!event_iface) {
        ESP_LOGE(TAG, "Could not allocate ADF event interface");
        radio_pipeline_stop();
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(audio_pipeline_set_listener(pipeline, event_iface));

    err = audio_pipeline_run(pipeline);
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
    if (el == fmp4_bridge_el) return "fmp4";
    if (el == aac_decoder) return "aac";
    if (el == i2s_writer) return "i2s";
    return "?";
}

esp_err_t radio_pipeline_wait(TickType_t max_session_ticks)
{
    if (!event_iface) {
        return ESP_ERR_INVALID_STATE;
    }

    bool has_deadline = (max_session_ticks != portMAX_DELAY);
    TickType_t deadline = has_deadline ? xTaskGetTickCount() + max_session_ticks : 0;

    while (true) {
        TickType_t wait_ticks = portMAX_DELAY;
        if (has_deadline) {
            TickType_t now = xTaskGetTickCount();
            if (now >= deadline) {
                ESP_LOGI(TAG, "Proactive session refresh: max session duration reached");
                return ESP_OK;
            }
            wait_ticks = deadline - now;
        }

        audio_event_iface_msg_t msg;
        esp_err_t err = audio_event_iface_listen(event_iface, &msg, wait_ticks);
        if (err == ESP_ERR_TIMEOUT) {
            continue; /* re-check the deadline (or spurious wakeup); keep waiting */
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Event listener error: %s", esp_err_to_name(err));
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
            (audio_element_handle_t)msg.source == aac_decoder) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(aac_decoder, &music_info);
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
             * so the decoder stays correctly configured too. */
            ESP_LOGI(TAG, "HLS live window exhausted; restarting pipeline in place "
                     "(no TuneIn re-resolve), free heap=%" PRIu32, esp_get_free_heap_size());
            audio_pipeline_stop(pipeline);
            audio_pipeline_wait_for_stop(pipeline);
            audio_pipeline_reset_ringbuffer(pipeline);
            audio_pipeline_reset_elements(pipeline);
            audio_pipeline_change_state(pipeline, AEL_STATE_INIT);
            esp_err_t rerun = audio_pipeline_run(pipeline);
            /* audio_pipeline_stop()/wait_for_stop() above make every element
             * (hls/fmp4/aac/i2s) pass through its own normal stop sequence,
             * and audio_element.c reports AEL_STATUS_STATE_STOPPED to this
             * same event_iface as part of that - see audio_element_report_status()
             * calls guarding AEL_STATE_STOPPED in audio_element.c. Those are
             * self-inflicted echoes of the stop we just issued, not a new
             * failure, but nothing above drains them before the loop goes
             * back to audio_event_iface_listen() - so the very next iteration
             * was reading a STALE queued status (observed on hardware: hls
             * status=14/STATE_STOPPED landing in the SAME log line as
             * "Pipeline started"), is_terminal_status() called it terminal,
             * and radio_pipeline_wait() returned ESP_FAIL - which sends
             * main.c through a full TuneIn re-resolve (~12 s of silence,
             * confirmed in a hardware log) on every single live-window
             * boundary, exactly the cost this in-place restart exists to
             * avoid. Discard whatever queued up during the stop/reset/rerun
             * sequence so only events from the freshly-running pipeline are
             * seen from here on. */
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
    if (!pipeline) return;
    ESP_LOGW(TAG, "Stopping audio pipeline for refresh/recovery");
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);
    audio_pipeline_unregister(pipeline, http_reader);
    audio_pipeline_unregister(pipeline, fmp4_bridge_el);
    audio_pipeline_unregister(pipeline, aac_decoder);
    audio_pipeline_unregister(pipeline, i2s_writer);
    if (event_iface) {
        audio_pipeline_remove_listener(pipeline);
        audio_event_iface_destroy(event_iface);
        event_iface = NULL;
    }
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(http_reader);
    audio_element_deinit(fmp4_bridge_el);
    audio_element_deinit(aac_decoder);
    /* Unlike the single-chip project's bt_writer, i2s_writer has no
     * "can only be created once per process" restriction - safe to fully
     * deinit and recreate fresh every session. */
    audio_element_deinit(i2s_writer);
    pipeline = NULL;
    http_reader = NULL;
    fmp4_bridge_el = NULL;
    aac_decoder = NULL;
    i2s_writer = NULL;

    ESP_LOGI(TAG, "Pipeline torn down; free heap=%" PRIu32, esp_get_free_heap_size());
}

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "avrcp_uart.h"
#include "tunein_control.h"

/* Starts HTTP(HLS) -> fMP4/CMAF bridge -> AAC decoder -> I2S-out pipeline,
 * driving the I2S bus as master (see RADIO_I2S_*_GPIO in app_config.h) so
 * the companion esp32_bt_speaker chip can just listen as I2S slave. Returns
 * once the elements are launched; ESP-ADF's own audio_pipeline_run() is
 * non-blocking, so this does not mean playback has finished or even started
 * yet - call radio_pipeline_wait() next.
 *
 * Safe to call with a pipeline already running (radio_pipeline.c no longer
 * requires the caller to radio_pipeline_stop() first - see main.c's
 * radio_task() loop, which now only does that explicitly before
 * calibration). If the existing pipeline is CMAF and `session` is ALSO
 * CMAF, this swaps the station in place (new URL/init segment only,
 * decoder/I2S element instances kept alive - see
 * [[esp32-wifi-streamer-aac-heap-crash]] for why that matters) instead of
 * a full teardown+rebuild; any other case (no pipeline yet, or a format
 * change) does the full rebuild as before, transparently to the caller. */
esp_err_t radio_pipeline_start(tunein_session_t *session);

/*
 * Blocks until the pipeline needs to be replaced with a fresh TuneIn session:
 * an element reported an error or finished (e.g. the signed HLS URL expired
 * and the CDN started rejecting requests), max_session_ticks elapsed (a
 * defensive refresh, since the exact lifetime of TuneIn's signed URLs is
 * undocumented), an AVRCP/console next/previous command arrived (avrcp_uart.h,
 * console_cli.h), or a console `cal` command requested LED/audio latency
 * calibration (console_cli.h, latency_cal.h).
 *
 * Returns ESP_OK and sets *out_station_cmd to AVRCP_CMD_NEXT/AVRCP_CMD_PREV
 * when the caller should switch stations - the session was still healthy,
 * so this is NOT a failure and must not attract the caller's error backoff.
 * Returns ESP_OK and sets *out_calibrate to true when the caller should run
 * latency_cal_run() instead of restarting playback - also not a failure, and
 * mutually exclusive with *out_station_cmd (at most one of the two is ever
 * set on a given call; both stay at their "nothing happened" default
 * otherwise). Returns ESP_OK with both left at their defaults on a normal
 * refresh (max_session_ticks elapsed, or a clean HLS live-window restart
 * that could not be done in place). Returns the element's error otherwise.
 * Either out-parameter may be NULL if the caller does not care.
 *
 * Pass portMAX_DELAY to disable the time-based refresh; the loop still wakes
 * regularly to service the playlist prefetcher and poll for AVRCP/console
 * commands.
 */
esp_err_t radio_pipeline_wait(TickType_t max_session_ticks, avrcp_cmd_t *out_station_cmd, bool *out_calibrate);

void radio_pipeline_stop(void);

/*
 * Snapshot of what the pipeline is actually carrying right now - for
 * diagnostics/logging only (main.c's periodic resource-headroom log; see
 * RADIO_RESOURCE_LOG_INTERVAL_MS in app_config.h), not for pipeline control.
 *
 * `sample_rate_hz`/`channels` are whatever i2s_writer is CONFIGURED at, i.e.
 * the CMAF init segment's AudioSpecificConfig on the primary path, or the
 * decoder's own AEL_MSG_CMD_REPORT_MUSIC_INFO retune on the generic fallback
 * path (see radio_pipeline_wait()) - either way, the real format actually in
 * use, not a guess.
 *
 * There is no per-frame bitrate metadata available on this path (fmp4_bridge
 * synthesizes ADTS headers with no bit_rate field, and esp_aac_dec is TOLD
 * the format rather than parsing one out - see aac_dec_element.h) - so no
 * bitrate field is exposed here. `http_bytes_total` is http_reader's own
 * running count of bytes pulled off the wire - monotonic for as long as the
 * SAME http_reader element instance lives (every full pipeline rebuild, but
 * NOT an in-place station switch - see switch_cmaf_station_in_place() -
 * creates a fresh one, restarting this count from 0). The caller can turn a
 * delta of this across a known time interval into a measured effective
 * throughput (kbps) - which is what's actually informative for a live HLS
 * stream: real network throughput including HLS/CMAF container overhead,
 * not just a nominal per-station figure that may not reflect what's really
 * arriving - as long as it treats a DECREASE between two reads as "the
 * element was recreated, discard this interval" rather than a negative rate.
 *
 * `active` is false (and every other field left zeroed) when no pipeline is
 * currently up at all (between sessions, or before the first one).
 */
typedef struct {
    bool active;
    const char *format_name;   /* e.g. "CMAF/fMP4 AAC-LC (HLS)"; "none" if !active */
    int sample_rate_hz;
    int channels;
    int64_t http_bytes_total;
} radio_pipeline_stream_stats_t;

void radio_pipeline_get_stream_stats(radio_pipeline_stream_stats_t *out);

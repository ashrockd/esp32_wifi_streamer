#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "avrcp_uart.h"
#include "tunein_control.h"

/* Starts HTTP(HLS) -> fMP4/CMAF bridge -> AAC decoder -> I2S-out pipeline,
 * driving the I2S bus as master (see RADIO_I2S_*_GPIO in app_config.h) so
 * the companion esp32_bt_speaker chip can just listen as I2S slave. Returns
 * once the elements are launched; ESP-ADF's own audio_pipeline_run() is
 * non-blocking, so this does not mean playback has finished or even started
 * yet - call radio_pipeline_wait() next. */
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

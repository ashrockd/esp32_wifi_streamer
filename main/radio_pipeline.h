#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
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
 * and the CDN started rejecting requests), or max_session_ticks elapsed
 * (a defensive refresh, since the exact lifetime of TuneIn's signed URLs is
 * undocumented). Returns ESP_OK on a clean/expected stop, or the element's
 * error otherwise. Pass portMAX_DELAY to disable the time-based refresh.
 */
esp_err_t radio_pipeline_wait(TickType_t max_session_ticks);

void radio_pipeline_stop(void);

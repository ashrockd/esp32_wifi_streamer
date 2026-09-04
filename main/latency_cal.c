#include "latency_cal.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_common.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "i2s_stream.h"
#include "raw_stream.h"
#include "ringbuf.h"

#include "app_config.h"
#include "console_cli.h"
#include "led_viz.h"

static const char *TAG = "LATENCY_CAL";

/* Test tone parameters. Matches the main pipeline's own I2S format (48kHz
 * stereo 16-bit) exactly - not incidental: using a different rate/width here
 * would mean the companion esp32_bt_speaker chip's resample/encode path
 * behaves differently for the test tone than for real playback, which would
 * make the measured latency less representative of what is being calibrated
 * for. */
#define CAL_SAMPLE_RATE       48000
#define CAL_CHANNELS          2
#define CAL_TWO_PI            6.283185307f
#define CAL_BEEP_FREQ_HZ      1000.0f
#define CAL_BEEP_DURATION_MS  500   /* long enough to comfortably react to and register 'heard' while it's still audible */
#define CAL_BEEP_FADE_OUT_MS  15    /* tail only - see generate_beep()'s comment on why the onset stays sharp */
#define CAL_BEEP_AMPLITUDE    0.6f  /* headroom below full-scale so the tone itself never clips/distorts */

/* Comfortably larger than one whole beep burst (500ms @ 48kHz stereo 16-bit
 * = 96000 bytes) so a single raw_stream_write() call normally completes in
 * one shot without blocking on the ring buffer draining mid-write - see
 * play_beep_blocking()'s loop, which handles a partial write correctly
 * regardless, but there is no reason to rely on that path when avoiding it
 * costs a trivial amount of RAM on this board. */
#define CAL_RAW_RB_BYTES      (192 * 1024)

#define CAL_MAX_TRIALS        16
#define CAL_POLL_TICKS        pdMS_TO_TICKS(200)

static audio_pipeline_handle_t s_pipeline;
static audio_element_handle_t s_raw_el;
static audio_element_handle_t s_i2s_el;
static int16_t *s_beep_buf;
static size_t s_beep_bytes;
static volatile bool s_active;

/*
 * Identical in spirit to radio_pipeline.c's own led_viz_write_cb() (static
 * there, not exported - see that file for the original and its rationale).
 * Duplicated here rather than shared because the two files' tap points have
 * different element types and sharing would mean widening led_viz.h's
 * dependency footprint (currently plain C types only, no ESP-ADF headers)
 * just for this one small, stable callback. What it does: feed the PCM to
 * the LED exactly like real playback would, then pass it through to the
 * ring buffer completely unchanged - this REPLACES raw_stream's default
 * output behaviour, so it must reproduce it exactly.
 */
static int cal_write_cb(audio_element_handle_t self, char *buffer, int len,
                        TickType_t ticks_to_wait, void *ctx)
{
    (void)self;
    if (len > 0) {
        led_viz_feed_pcm(buffer, (size_t)len);
    }
    return rb_write((ringbuf_handle_t)ctx, buffer, len, ticks_to_wait);
}

/*
 * Synthesizes one beep tone into a heap buffer, reused for every trial in
 * one calibration session (freed in latency_cal_run() before it returns).
 *
 * Deliberately NOT faded in, only faded out: a soft onset makes "the exact
 * instant I heard it" a fuzzier, more variable judgement call for a human
 * timing their own reaction against it, which is precision this measurement
 * cannot afford to give away. The fade-OUT exists purely so the tone doesn't
 * end in an audible click/pop - it plays no role in the timing being
 * measured, since only the ONSET matters for that.
 */
static esp_err_t generate_beep(void)
{
    size_t n = (size_t)CAL_SAMPLE_RATE * CAL_BEEP_DURATION_MS / 1000; /* samples per channel */
    size_t fade_n = (size_t)CAL_SAMPLE_RATE * CAL_BEEP_FADE_OUT_MS / 1000;
    s_beep_bytes = n * CAL_CHANNELS * sizeof(int16_t);

    s_beep_buf = malloc(s_beep_bytes);
    if (!s_beep_buf) {
        ESP_LOGE(TAG, "OOM allocating %u-byte beep buffer", (unsigned)s_beep_bytes);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < n; i++) {
        float t = (float)i / (float)CAL_SAMPLE_RATE;
        float s = sinf(CAL_TWO_PI * CAL_BEEP_FREQ_HZ * t);
        float env = 1.0f;
        if (fade_n > 0 && i + fade_n >= n) {
            size_t from_end = n - i;
            env = (float)from_end / (float)fade_n;
        }
        int16_t sample = (int16_t)(s * env * CAL_BEEP_AMPLITUDE * 32767.0f);
        for (int ch = 0; ch < CAL_CHANNELS; ch++) {
            s_beep_buf[i * CAL_CHANNELS + ch] = sample;
        }
    }
    return ESP_OK;
}

static void free_beep(void)
{
    free(s_beep_buf);
    s_beep_buf = NULL;
    s_beep_bytes = 0;
}

static void teardown_pipeline(void)
{
    if (!s_pipeline) {
        return;
    }
    audio_pipeline_stop(s_pipeline);
    audio_pipeline_wait_for_stop(s_pipeline);
    audio_pipeline_terminate(s_pipeline);
    if (s_raw_el) {
        audio_pipeline_unregister(s_pipeline, s_raw_el);
    }
    if (s_i2s_el) {
        audio_pipeline_unregister(s_pipeline, s_i2s_el);
    }
    audio_pipeline_deinit(s_pipeline);
    if (s_raw_el) {
        audio_element_deinit(s_raw_el);
    }
    if (s_i2s_el) {
        audio_element_deinit(s_i2s_el);
    }
    s_pipeline = NULL;
    s_raw_el = NULL;
    s_i2s_el = NULL;
}

/*
 * [raw_stream (writer)] -> [i2s_stream] - a minimal, self-contained pipeline
 * completely independent of the normal HTTP/fMP4/decode chain (which is
 * already fully torn down by the time this runs - see latency_cal_run()).
 * raw_stream_write() (called per trial, not here) is a thin wrapper directly
 * around audio_element_output() on this element - see raw_stream.c in the
 * vendored esp-adf tree - which is exactly the same underlying call the LED
 * tap below intercepts, so a beep exercises the identical code path real
 * playback does.
 *
 * I2S pin/role/format setup mirrors radio_pipeline.c's own main-pipeline
 * setup exactly (RADIO_I2S_*_GPIO, master, no MCLK) - not re-verified
 * against get_i2s_pins() here (see radio_pipeline.c's check_i2s_pins()):
 * calibration can only ever be entered from within an already-running
 * radio_task loop, which means the main pipeline already verified these
 * exact pins at least once before calibration was ever reachable, and they
 * cannot change at runtime.
 */
static esp_err_t build_pipeline(void)
{
    audio_pipeline_cfg_t pipe_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_pipeline = audio_pipeline_init(&pipe_cfg);
    if (!s_pipeline) {
        return ESP_ERR_NO_MEM;
    }

    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    raw_cfg.out_rb_size = CAL_RAW_RB_BYTES;
    s_raw_el = raw_stream_init(&raw_cfg);

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(
        I2S_NUM_0, CAL_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, AUDIO_STREAM_WRITER);
    i2s_cfg.chan_cfg.role = I2S_ROLE_MASTER;
    i2s_cfg.std_cfg.gpio_cfg.bclk = RADIO_I2S_BCLK_GPIO;
    i2s_cfg.std_cfg.gpio_cfg.ws = RADIO_I2S_WS_GPIO;
    i2s_cfg.std_cfg.gpio_cfg.dout = RADIO_I2S_DATA_GPIO;
    i2s_cfg.std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    i2s_cfg.std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    s_i2s_el = i2s_stream_init(&i2s_cfg);

    if (!s_raw_el || !s_i2s_el) {
        ESP_LOGE(TAG, "Calibration pipeline element allocation failed (raw=%p i2s=%p)",
                 s_raw_el, s_i2s_el);
        teardown_pipeline();
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(audio_pipeline_register(s_pipeline, s_raw_el, "raw"));
    ESP_ERROR_CHECK(audio_pipeline_register(s_pipeline, s_i2s_el, "i2s"));
    const char *link[] = { "raw", "i2s" };
    ESP_ERROR_CHECK(audio_pipeline_link(s_pipeline, link, 2));

    /* Must happen AFTER audio_pipeline_link() - see radio_pipeline.c's
     * identical comment on its own tap: link() is what creates the ring
     * buffer this callback needs as its write target. */
    ringbuf_handle_t raw_out_rb = audio_element_get_output_ringbuf(s_raw_el);
    if (raw_out_rb) {
        esp_err_t tap_err = audio_element_set_write_cb(s_raw_el, cal_write_cb, raw_out_rb);
        if (tap_err != ESP_OK) {
            ESP_LOGW(TAG, "Could not tap the calibration tone for the LED: %s "
                     "(the tone will still play; the LED just will not react to it)",
                     esp_err_to_name(tap_err));
        }
    }

    return ESP_OK;
}

/* Writes the whole beep buffer, looping on a partial write rather than
 * assuming raw_stream_write() (audio_element_output() underneath) always
 * accepts everything in one call - see CAL_RAW_RB_BYTES's comment on why
 * that should be rare in practice, but this is correct either way. */
static esp_err_t play_beep_blocking(void)
{
    if (!s_raw_el || !s_beep_buf) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t sent = 0;
    char *p = (char *)s_beep_buf;
    while (sent < s_beep_bytes) {
        int w = raw_stream_write(s_raw_el, p + sent, (int)(s_beep_bytes - sent));
        if (w <= 0) {
            ESP_LOGW(TAG, "raw_stream_write returned %d after %u/%u bytes; beep truncated",
                     w, (unsigned)sent, (unsigned)s_beep_bytes);
            return ESP_FAIL;
        }
        sent += (size_t)w;
    }
    return ESP_OK;
}

bool latency_cal_is_active(void)
{
    return s_active;
}

void latency_cal_run(void)
{
    s_active = true;

    printf("\n=== LED/Audio Latency Calibration ===\n");
    printf("Normal playback is paused. Setting up the test tone...\n");

    /* Neither the tail of whatever was just playing nor this session's own
     * test tone should bleed across the mode boundary in either direction -
     * see led_viz.h's doc comment on led_viz_reset_history(). Called again
     * at the end of this function for the same reason, in reverse. */
    led_viz_reset_history();

    esp_err_t err = generate_beep();
    if (err != ESP_OK) {
        printf("Could not allocate the test tone (%s) - aborting, resuming playback.\n",
               esp_err_to_name(err));
        s_active = false;
        return;
    }

    err = build_pipeline();
    if (err != ESP_OK) {
        printf("Could not build the test pipeline (%s) - aborting, resuming playback.\n",
               esp_err_to_name(err));
        free_beep();
        s_active = false;
        return;
    }

    err = audio_pipeline_run(s_pipeline);
    if (err != ESP_OK) {
        printf("Could not start the test pipeline (%s) - aborting, resuming playback.\n",
               esp_err_to_name(err));
        teardown_pipeline();
        free_beep();
        s_active = false;
        return;
    }

    printf("Ready. Commands from here:\n");
    printf("  beep    - play a %dms test tone right now\n", CAL_BEEP_DURATION_MS);
    printf("  heard   - type this the INSTANT you actually hear it from the speaker\n");
    printf("  accept  - save the average of all trials so far and resume playback\n");
    printf("  cancel  - discard everything and resume playback unchanged\n");
    printf("Do a few 'beep'/'heard' pairs (3-5 is plenty) before 'accept' for a stable average.\n");
    printf("Note: this measures tone-to-button-press, which includes your own reaction time\n");
    printf("(commonly ~150-250ms) alongside the real hardware delay - that is expected, not\n");
    printf("an error, since the goal is syncing the LED to what a human perceives, not a lab figure.\n");
    printf("Current latency compensation: %" PRId32 "ms\n\n", led_viz_get_latency_ms());

    int32_t trials[CAL_MAX_TRIALS];
    int trial_count = 0;
    bool waiting_for_heard = false;
    int64_t t0_us = 0;
    bool done = false;

    while (!done) {
        console_cmd_t cmd = console_cli_take_command(CAL_POLL_TICKS);
        switch (cmd) {
        case CONSOLE_CMD_CAL_BEEP:
            if (waiting_for_heard) {
                printf("Already waiting for 'heard' on the current trial - type that first.\n");
                break;
            }
            t0_us = esp_timer_get_time();
            if (play_beep_blocking() != ESP_OK) {
                printf("Could not play the test tone - try 'beep' again.\n");
                break;
            }
            waiting_for_heard = true;
            printf("Beep sent - type 'heard' the instant you hear it.\n");
            break;

        case CONSOLE_CMD_CAL_HEARD: {
            if (!waiting_for_heard) {
                printf("No trial in progress - type 'beep' first.\n");
                break;
            }
            waiting_for_heard = false;
            int64_t t1_us = esp_timer_get_time();
            int32_t delay_ms = (int32_t)((t1_us - t0_us) / 1000);

            if (delay_ms < 20) {
                printf("That's suspiciously fast (%" PRId32 "ms) - recorded anyway, but consider "
                       "redoing this trial.\n", delay_ms);
            } else if (delay_ms > 5000) {
                printf("That's suspiciously slow (%" PRId32 "ms) - recorded anyway, but consider "
                       "redoing this trial.\n", delay_ms);
            }

            if (trial_count >= CAL_MAX_TRIALS) {
                printf("Already have %d trials (the most this tracks) - 'accept' or 'cancel' now.\n",
                       CAL_MAX_TRIALS);
                break;
            }
            trials[trial_count++] = delay_ms;

            int32_t sum = 0, tmin = trials[0], tmax = trials[0];
            for (int i = 0; i < trial_count; i++) {
                sum += trials[i];
                if (trials[i] < tmin) tmin = trials[i];
                if (trials[i] > tmax) tmax = trials[i];
            }
            printf("Trial %d: %" PRId32 "ms  (average so far: %" PRId32 "ms over %d trial(s), "
                   "min=%" PRId32 " max=%" PRId32 ")\n",
                   trial_count, delay_ms, sum / trial_count, trial_count, tmin, tmax);
            break;
        }

        case CONSOLE_CMD_CAL_ACCEPT: {
            if (trial_count == 0) {
                printf("No trials recorded yet - do at least one 'beep'/'heard' pair first.\n");
                break;
            }
            int32_t sum = 0;
            for (int i = 0; i < trial_count; i++) {
                sum += trials[i];
            }
            int32_t avg = sum / trial_count;
            led_viz_set_latency_ms(avg);
            printf("Saved: LED will now lag audio by %" PRId32 "ms (average of %d trial(s)).\n",
                   avg, trial_count);
            done = true;
            break;
        }

        case CONSOLE_CMD_CAL_CANCEL:
            printf("Cancelled - no change made. Latency compensation remains %" PRId32 "ms.\n",
                   led_viz_get_latency_ms());
            done = true;
            break;

        case CONSOLE_CMD_CAL_ENTER:
            printf("Already in calibration mode.\n");
            break;

        case CONSOLE_CMD_NEXT:
        case CONSOLE_CMD_PREV:
            printf("Station changes don't apply during calibration - finish with 'accept' "
                   "or 'cancel' first.\n");
            break;

        default:
            break; /* CONSOLE_CMD_NONE: just a poll timeout, keep waiting silently */
        }
    }

    teardown_pipeline();
    free_beep();
    led_viz_reset_history();
    s_active = false;
    printf("Resuming normal playback...\n\n");
}

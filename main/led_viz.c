#include "led_viz.h"

#include <math.h>
#include <string.h>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "app_config.h"

static const char *TAG = "LED_VIZ";

/* --- WS2812 timing -----------------------------------------------------
 * A 10 MHz RMT resolution makes one tick 0.1us, which expresses the
 * WS2812B datasheet's T0H/T0L/T1H/T1L (0.3/0.9/0.9/0.3 us) exactly as
 * whole ticks - no rounding, and comfortably inside the +/-150ns tolerance.
 * The >50us latch/reset gap between frames is not encoded here: the render
 * loop's own 20ms frame period is three orders of magnitude longer than it,
 * so the gap exists for free. */
#define LED_RMT_RESOLUTION_HZ 10000000
#define WS2812_T0H_TICKS      3
#define WS2812_T0L_TICKS      9
#define WS2812_T1H_TICKS      9
#define WS2812_T1L_TICKS      3

/* 50 fps. Fast enough that transients read as instantaneous, slow enough
 * that the render task is nowhere near the decode path's cost. */
#define LED_FRAME_MS          20

/* --- Colour: a fixed, audio-independent loop ---------------------------
 * Requested behaviour: colour just cycles on its own; only brightness
 * reacts to audio. One full trip around the hue wheel every ~20s. See
 * current_hue() for why the modulo is done in integer milliseconds rather
 * than on the raw microsecond timestamp. */
#define LED_COLOR_CYCLE_MS    20000

/* --- Level scaling -------------------------------------------------------
 * Peak amplitude is converted to dBFS before being mapped to anything.
 * Linear amplitude is the wrong axis for this: music sits in the top few
 * percent of linear scale almost all the time, so a linear map produces a
 * light that is either off or pinned. On a dB axis the whole floor-to-peak
 * range is usable. */
#define LED_FLOOR_DBFS        (-60.0f)

/* Fast attack, slow decay - a transient is shown immediately, then falls
 * away smoothly instead of strobing. Per-frame coefficients at 50 fps:
 * a hit reaches ~full in one frame, and decays over roughly a second. This
 * is what keeps a peak flash from just staying lit or flickering frame to
 * frame - see the file header's "2026-09-04" note. */
#define LED_ATTACK             0.60f
#define LED_DECAY               0.055f

/* --- Brightness envelope, threshold-gated -------------------------------
 * Requested behaviour: quiet-to-medium audio -> none to low brightness;
 * only a peak louder than the (runtime-adjustable) threshold gets very
 * bright. Two independent linear segments either side of the threshold,
 * rather than one curve over the whole range, so crossing the threshold
 * reads as a distinct jump rather than just another point on a ramp - see
 * level_to_brightness(). */
#define LED_BRIGHT_FLOOR_MIN   0.00f   /* silence -> fully off */
#define LED_BRIGHT_FLOOR_MAX   0.05f   /* top of "below threshold" -> barely-there glow */
#define LED_BRIGHT_LOUD_MIN    0.12f   /* just above threshold -> a clearly visible jump */
#define LED_BRIGHT_PEAK_MAX    1.00f   /* full-scale peak -> very bright */

/* Starting point before anything has ever been saved to NVS (or if a saved
 * value is somehow out of range) - see load_threshold(). Picked as "louder
 * than typical program level": compressed streamed music spends most of its
 * time well under this, so ordinary listening stays dim, while genuine loud
 * peaks (choruses, drops - which in modern mastering often approach -6..0
 * dBFS) cross it and flash. Freely retunable at runtime; this is only the
 * first-boot default. */
#define LED_DEFAULT_THRESHOLD_DBFS (-18.0f)

/* Brightness of the boot colour cycle, and how long each colour is held. */
#define LED_BOOT_BRIGHTNESS   0.20f
#define LED_BOOT_STEP_MS      1000

/* With no PCM for this long, the pipeline is between segments/sessions (or
 * not playing at all) - decay to black rather than freezing on the last
 * colour, which would otherwise look like a hung device. */
#define LED_SILENCE_TIMEOUT_US (1500 * 1000)

/* NVS-persisted brightness threshold - see led_viz_set_threshold_db(). */
#define LED_NVS_NAMESPACE "led_viz"
#define LED_NVS_KEY       "thr_db_c100"  /* dBFS * 100, stored as an int32 (0.01dB resolution) */
#define LED_LATENCY_NVS_KEY "latency_ms" /* int32 milliseconds - see led_viz_set_latency_ms() */

/* --- LED/audio latency compensation ------------------------------------
 * See led_viz.h's doc comment on led_viz_set_latency_ms() for the "why":
 * what the LED renders and what a human hears are separated by a real,
 * measurable delay dominated by the downstream Bluetooth hop, not by
 * anything on this chip.
 *
 * 8s is a deliberately generous cap, not a guess at how large a real
 * measurement will be (real A2DP/SBC latency is realistically 100s of ms,
 * not seconds) - the whole history buffer this supports is a few KB (see
 * the per-entry size below), trivial even without touching PSRAM, so there
 * is no real cost to supporting a much larger delay than anyone should ever
 * actually need. It just means a wildly-mistaken manual `latency` entry
 * clamps to something still usable instead of needing a second correction. */
#define LED_LATENCY_MAX_MS      8000
#define LED_LATENCY_HISTORY_LEN (LED_LATENCY_MAX_MS / LED_FRAME_MS)  /* 400 entries at 20ms/frame */

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_encoder;
static bool s_started;

/* Written by the decoder task (led_viz_feed_pcm), read by the render task.
 * A single naturally-aligned 32-bit word, so loads and stores are atomic on
 * this core - no lock is needed for one number, and taking one on the decode
 * path would be exactly the sort of cost this tap exists to avoid. Worst
 * case the renderer reads the previous frame's peak, 20ms stale, which is
 * invisible. */
static volatile uint32_t s_peak;
static volatile int64_t s_last_pcm_us;

/* Runtime-adjustable brightness threshold, in dBFS. Same atomicity argument
 * as s_peak above: one naturally-aligned 32-bit value (float is 32 bits),
 * written from whatever task calls led_viz_set_threshold_db() (the console
 * command handler) and read every frame by the render task - no lock. */
static volatile float s_thresh_dbfs = LED_DEFAULT_THRESHOLD_DBFS;

/* Runtime-adjustable render delay, in milliseconds - see
 * led_viz_set_latency_ms()'s doc comment in led_viz.h. Same atomicity
 * argument as s_thresh_dbfs. 0 = no delay (default; identical to this
 * file's pre-2026-09-04 behaviour). */
static volatile int32_t s_latency_ms;

/*
 * Rolling history of this render task's OWN post-envelope brightness level,
 * one entry per LED_FRAME_MS tick - the delay line led_viz_set_latency_ms()
 * reads from. Only ever written/read by led_viz_task() itself (the render
 * loop), so no locking despite living at file scope - it is not shared with
 * led_viz_feed_pcm()'s decoder-task caller the way s_peak is.
 *
 * Why delay this (the ALREADY-COMPUTED level) rather than the raw peak/
 * have_audio pair feeding into it: the attack/decay envelope in led_viz_task
 * is a simple recurrence (this frame's level depends only on last frame's
 * level and this frame's target), so running it in real time against the
 * TRUE, undelayed audio timeline and then delaying its OUTPUT for display
 * produces the identical result as delaying the input and replaying the
 * envelope later - but only the former keeps the envelope's own attack/decay
 * shape faithful to how the audio actually evolved, which is the whole
 * point of an envelope follower. Delay is applied purely at the READ (render)
 * step below, never at the WRITE step, which is also what keeps this a
 * simple flat array indexed by frame count rather than a real circular
 * buffer needing per-entry timestamps: the render loop's own fixed
 * LED_FRAME_MS cadence already IS the time axis.
 */
typedef struct {
    float level;
    bool  have_audio;
} led_history_entry_t;
static led_history_entry_t s_history[LED_LATENCY_HISTORY_LEN];
static uint32_t s_history_write_idx;

/* Sends one pixel. WS2812 wants GRB order, not RGB - the single most common
 * way a driver like this comes out looking "wrong colour" rather than
 * broken. */
static void led_write_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_started) {
        return;
    }
    uint8_t grb[3] = { g, r, b };
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    esp_err_t err = rmt_transmit(s_chan, s_encoder, grb, sizeof(grb), &tx_cfg);
    if (err == ESP_OK) {
        /* Bounded wait: the whole 24-bit frame is ~30us, so this returns
         * almost immediately. Waiting (rather than firing and forgetting)
         * keeps the next frame from queueing on top of this one.
         * NOTE: this timeout is in MILLISECONDS, not ticks - it is one of
         * the few ESP-IDF calls that does not take a TickType_t, and
         * pdMS_TO_TICKS() here would silently shorten it. */
        rmt_tx_wait_all_done(s_chan, 50);
    }
}

/*
 * HSV -> RGB with hue in degrees [0,360), saturation and value in [0,1].
 * Standard sextant formulation; kept local because pulling in a colour
 * library for one function would be silly.
 */
static void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (h < 0.0f)    h = 0.0f;
    if (h >= 360.0f) h = 359.999f;

    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;

    float rf = 0, gf = 0, bf = 0;
    int sextant = (int)(h / 60.0f);
    switch (sextant) {
    case 0:  rf = c; gf = x; bf = 0; break;
    case 1:  rf = x; gf = c; bf = 0; break;
    case 2:  rf = 0; gf = c; bf = x; break;
    case 3:  rf = 0; gf = x; bf = c; break;
    case 4:  rf = x; gf = 0; bf = c; break;
    default: rf = c; gf = 0; bf = x; break;
    }

    *r = (uint8_t)lroundf((rf + m) * 255.0f);
    *g = (uint8_t)lroundf((gf + m) * 255.0f);
    *b = (uint8_t)lroundf((bf + m) * 255.0f);
}

/*
 * Continuous hue rotation, one full loop through the wheel every
 * LED_COLOR_CYCLE_MS (~20s) - entirely time-based and audio-independent
 * (see the file header's 2026-09-04 note: colour no longer encodes level,
 * only brightness does).
 *
 * The modulo is done in integer MILLISECONDS before converting to float:
 * esp_timer_get_time() is microseconds since boot as an int64, and a
 * float's 24-bit mantissa only represents integers exactly up to ~16.7
 * million - a raw microsecond count would start losing precision (and
 * visibly stutter) well under a minute after boot. The remainder after the
 * modulo is always < LED_COLOR_CYCLE_MS though, so converting THAT to float
 * is always exact.
 */
static float current_hue(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t phase_ms = now_ms % LED_COLOR_CYCLE_MS;
    return (float)phase_ms / (float)LED_COLOR_CYCLE_MS * 360.0f;
}

/* dBFS -> normalized [0,1] level against the same floor/ceiling the raw
 * peak conversion below uses - one shared axis, so the threshold and the
 * signal it's compared against always mean the same thing. */
static float dbfs_to_level(float dbfs)
{
    float t = (dbfs - LED_FLOOR_DBFS) / (0.0f - LED_FLOOR_DBFS);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

/*
 * Level [0,1] -> brightness, threshold-gated - see LED_BRIGHT_* above. Below
 * (or at) the threshold, brightness is scaled gently within the low
 * "floor" range (none to low); above it, brightness ramps steeply up
 * through the "loud" range toward LED_BRIGHT_PEAK_MAX (very bright). The
 * jump at the crossing point is deliberate: it is what makes a peak read as
 * an EVENT rather than the tail end of a smooth ramp.
 */
static float level_to_brightness(float level)
{
    float thr = dbfs_to_level(s_thresh_dbfs);
    if (thr >= 1.0f) {
        thr = 0.999f; /* guard the division below if the threshold is set to 0 dBFS (max) */
    }

    if (level <= thr) {
        float t = (thr > 0.0f) ? (level / thr) : 0.0f;
        return LED_BRIGHT_FLOOR_MIN + t * (LED_BRIGHT_FLOOR_MAX - LED_BRIGHT_FLOOR_MIN);
    }
    float t = (level - thr) / (1.0f - thr);
    if (t > 1.0f) t = 1.0f;
    return LED_BRIGHT_LOUD_MIN + t * (LED_BRIGHT_PEAK_MAX - LED_BRIGHT_LOUD_MIN);
}

static void led_boot_cycle(void)
{
    /* Requested order: red, green, blue, cyan, magenta, yellow - 1s each,
     * all at 20%. Written as hues so they go through the exact same
     * HSV path the visualiser uses, which makes this a real end-to-end
     * check of the colour pipeline and not just six hardcoded triples. */
    static const float boot_hues[] = { 0.0f, 120.0f, 240.0f, 180.0f, 300.0f, 60.0f };
    static const char *boot_names[] = { "red", "green", "blue", "cyan", "magenta", "yellow" };

    ESP_LOGI(TAG, "Boot colour cycle: red/green/blue/cyan/magenta/yellow, %dms each at %d%%",
             LED_BOOT_STEP_MS, (int)(LED_BOOT_BRIGHTNESS * 100));

    for (size_t i = 0; i < sizeof(boot_hues) / sizeof(boot_hues[0]); i++) {
        uint8_t r, g, b;
        hsv_to_rgb(boot_hues[i], 1.0f, LED_BOOT_BRIGHTNESS, &r, &g, &b);
        ESP_LOGD(TAG, "Boot colour %s -> RGB(%u,%u,%u)", boot_names[i], r, g, b);
        led_write_rgb(r, g, b);
        vTaskDelay(pdMS_TO_TICKS(LED_BOOT_STEP_MS));
    }
    led_write_rgb(0, 0, 0);
}

static void led_viz_task(void *arg)
{
    led_boot_cycle();

    float level = 0.0f;

    while (true) {
        /* Take and clear the peak, so each frame reflects only the audio
         * that arrived since the last one. Reading-and-clearing rather than
         * just reading is what makes the decay below actually decay: a
         * stale peak left in place would hold the light up forever. */
        uint32_t peak = s_peak;
        s_peak = 0;

        bool have_audio = (esp_timer_get_time() - s_last_pcm_us) < LED_SILENCE_TIMEOUT_US;

        float target = 0.0f;
        if (have_audio && peak > 0) {
            float amp = (float)peak / 32768.0f;
            float dbfs = 20.0f * log10f(amp);
            target = dbfs_to_level(dbfs);
        }

        /* Fast attack, slow decay - computed in REAL TIME against the true,
         * undelayed audio timeline. See s_history's comment above for why:
         * the delay is applied only when READING this history back for
         * display, a few lines down, never here. */
        float coeff = (target > level) ? LED_ATTACK : LED_DECAY;
        level += (target - level) * coeff;
        if (level < 0.0005f) {
            level = 0.0f;
        }

        /* Record this frame, then read back whatever frame is
         * led_viz_get_latency_ms() worth of frames in the past - clamped to
         * both the buffer's own capacity and to how much history actually
         * exists yet (right after boot there may not be LED_LATENCY_MAX_MS
         * of it) so this never wraps into a not-yet-written slot. At the
         * default 0ms latency, delay_frames is 0 and this reads back the
         * exact entry just written - i.e. identical to rendering `level`
         * directly, the pre-2026-09-04 behaviour, bit for bit. */
        uint32_t write_slot = s_history_write_idx % LED_LATENCY_HISTORY_LEN;
        s_history[write_slot].level = level;
        s_history[write_slot].have_audio = have_audio;

        uint32_t delay_frames = (uint32_t)(s_latency_ms / LED_FRAME_MS);
        if (delay_frames > LED_LATENCY_HISTORY_LEN - 1) {
            delay_frames = LED_LATENCY_HISTORY_LEN - 1;
        }
        if (delay_frames > s_history_write_idx) {
            delay_frames = s_history_write_idx; /* startup ramp-up: can't look back further than we've run */
        }
        uint32_t read_slot = (s_history_write_idx - delay_frames) % LED_LATENCY_HISTORY_LEN;
        float display_level = s_history[read_slot].level;
        bool display_have_audio = s_history[read_slot].have_audio;

        s_history_write_idx++;

        uint8_t r, g, b;
        if (!display_have_audio) {
            /* Fully idle - black, not a dim colour. See LED_SILENCE_TIMEOUT_US.
             * Gated on the DELAYED audio-presence flag, not `have_audio`
             * directly - otherwise the LED would cut to black the instant
             * this chip's source feed stops, even while several hundred ms
             * to a few seconds of already-fed audio is still queued up
             * behind it, on its way to actually being heard. */
            r = g = b = 0;
        } else {
            /* Hue is deliberately NOT delayed - it is a pure function of
             * wall-clock time (current_hue()), entirely audio-independent,
             * so there is no audio-timeline value for it to lag behind. */
            hsv_to_rgb(current_hue(), 1.0f, level_to_brightness(display_level), &r, &g, &b);
        }
        led_write_rgb(r, g, b);

        vTaskDelay(pdMS_TO_TICKS(LED_FRAME_MS));
    }
}

void led_viz_feed_pcm(const void *pcm, size_t bytes)
{
    if (!s_started || !pcm || bytes < sizeof(int16_t)) {
        return;
    }

    const int16_t *samples = (const int16_t *)pcm;
    size_t count = bytes / sizeof(int16_t);

    /* Peak, not RMS: a single pass with no multiply, and peak is what the
     * brightness scale is defined against (troughs/peaks, not average
     * power). Both channels are scanned together - this is one LED, so
     * there is nothing to gain from separating them. */
    int32_t peak = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t mag = samples[i] < 0 ? -(int32_t)samples[i] : (int32_t)samples[i];
        if (mag > peak) {
            peak = mag;
        }
    }

    /* Keep the loudest peak seen since the renderer last cleared it, so a
     * transient landing between frames is never missed. */
    if ((uint32_t)peak > s_peak) {
        s_peak = (uint32_t)peak;
    }
    s_last_pcm_us = esp_timer_get_time();
}

static float clamp_threshold_db(float db)
{
    if (db < LED_FLOOR_DBFS) db = LED_FLOOR_DBFS;
    if (db > 0.0f) db = 0.0f;
    return db;
}

/* Loads the last-persisted threshold from NVS, or falls back to
 * LED_DEFAULT_THRESHOLD_DBFS (without writing anything) if nothing has been
 * saved yet or a saved value is somehow out of range - mirrors
 * station_list.c's station_list_load_index() pattern exactly. */
static void load_threshold(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(LED_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved LED threshold yet (%s); using default %.1f dBFS",
                 esp_err_to_name(err), LED_DEFAULT_THRESHOLD_DBFS);
        s_thresh_dbfs = LED_DEFAULT_THRESHOLD_DBFS;
        return;
    }

    int32_t centi_db = 0;
    err = nvs_get_i32(handle, LED_NVS_KEY, &centi_db);
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved LED threshold yet (%s); using default %.1f dBFS",
                 esp_err_to_name(err), LED_DEFAULT_THRESHOLD_DBFS);
        s_thresh_dbfs = LED_DEFAULT_THRESHOLD_DBFS;
        return;
    }

    float db = clamp_threshold_db((float)centi_db / 100.0f);
    ESP_LOGI(TAG, "Resuming saved LED peak-brightness threshold: %.2f dBFS", db);
    s_thresh_dbfs = db;
}

esp_err_t led_viz_set_threshold_db(float threshold_dbfs)
{
    float clamped = clamp_threshold_db(threshold_dbfs);
    s_thresh_dbfs = clamped; /* applied immediately regardless of whether the NVS write below succeeds */

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LED_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not open NVS to persist LED threshold: %s (applied now, "
                 "will NOT survive a restart)", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_i32(handle, LED_NVS_KEY, (int32_t)lroundf(clamped * 100.0f));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not persist LED threshold %.2f dBFS: %s (applied now, "
                 "will NOT survive a restart)", clamped, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "LED peak-brightness threshold set to %.2f dBFS (saved)", clamped);
    }
    nvs_close(handle);
    return err;
}

float led_viz_get_threshold_db(void)
{
    return s_thresh_dbfs;
}

static int32_t clamp_latency_ms(int32_t ms)
{
    if (ms < 0) ms = 0;
    if (ms > LED_LATENCY_MAX_MS) ms = LED_LATENCY_MAX_MS;
    return ms;
}

/* Mirrors load_threshold() exactly - see its comment. */
static void load_latency(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(LED_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved LED latency yet (%s); using default 0ms (uncalibrated)",
                 esp_err_to_name(err));
        s_latency_ms = 0;
        return;
    }

    int32_t ms = 0;
    err = nvs_get_i32(handle, LED_LATENCY_NVS_KEY, &ms);
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved LED latency yet (%s); using default 0ms (uncalibrated)",
                 esp_err_to_name(err));
        s_latency_ms = 0;
        return;
    }

    ms = clamp_latency_ms(ms);
    ESP_LOGI(TAG, "Resuming saved LED/audio latency compensation: %" PRId32 "ms", ms);
    s_latency_ms = ms;
}

esp_err_t led_viz_set_latency_ms(int32_t latency_ms)
{
    int32_t clamped = clamp_latency_ms(latency_ms);
    s_latency_ms = clamped; /* applied immediately regardless of whether the NVS write below succeeds */

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LED_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not open NVS to persist LED latency: %s (applied now, "
                 "will NOT survive a restart)", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_i32(handle, LED_LATENCY_NVS_KEY, clamped);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not persist LED latency %" PRId32 "ms: %s (applied now, "
                 "will NOT survive a restart)", clamped, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "LED/audio latency compensation set to %" PRId32 "ms (saved)", clamped);
    }
    nvs_close(handle);
    return err;
}

int32_t led_viz_get_latency_ms(void)
{
    return s_latency_ms;
}

void led_viz_reset_history(void)
{
    /* Not touched by the render task concurrently in any way that matters:
     * worst case it overwrites a slot the render loop is about to overwrite
     * anyway on its very next tick. memset is not atomic against that, but
     * the failure mode is at most one stale/mixed frame during a mode
     * transition that is already expected to show a brief blackout - see
     * led_viz.h's doc comment on when this is called. */
    memset(s_history, 0, sizeof(s_history));
    s_history_write_idx = 0;
}

esp_err_t led_viz_start(void)
{
    load_threshold();
    load_latency();

    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num = RADIO_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_RMT_RESOLUTION_HZ,
        /* One 24-bit pixel needs 24 symbols; 64 is the minimum block size
         * the driver accepts and leaves the whole frame resident, so the
         * transmit never has to refill mid-pixel (a refill stall would show
         * as a colour glitch, since WS2812 latches on an idle gap). */
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };

    esp_err_t err = rmt_new_tx_channel(&chan_cfg, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel on GPIO%d failed: %s", RADIO_LED_GPIO, esp_err_to_name(err));
        return err;
    }

    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = WS2812_T0H_TICKS, .level1 = 0, .duration1 = WS2812_T0L_TICKS },
        .bit1 = { .level0 = 1, .duration0 = WS2812_T1H_TICKS, .level1 = 0, .duration1 = WS2812_T1L_TICKS },
        .flags = { .msb_first = 1 },   /* WS2812 clocks in MSB first, per byte */
    };
    err = rmt_new_bytes_encoder(&enc_cfg, &s_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_bytes_encoder failed: %s", esp_err_to_name(err));
        rmt_del_channel(s_chan);
        s_chan = NULL;
        return err;
    }

    err = rmt_enable(s_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        rmt_del_encoder(s_encoder);
        rmt_del_channel(s_chan);
        s_encoder = NULL;
        s_chan = NULL;
        return err;
    }

    s_started = true;

    /* Low priority on purpose: this is decoration. It must never compete
     * with the decoder or the I2S writer for CPU, and a dropped frame here
     * costs nothing, whereas a late audio buffer is audible. */
    if (xTaskCreate(led_viz_task, "led_viz", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create LED visualiser task");
        s_started = false;
        rmt_disable(s_chan);
        rmt_del_encoder(s_encoder);
        rmt_del_channel(s_chan);
        s_encoder = NULL;
        s_chan = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Audio visualiser on GPIO%d: %d fps, colour loops every %ds, brightness threshold "
             "%.1f dBFS (%d%%-%d%% below it, %d%%-%d%% above it)",
             RADIO_LED_GPIO, 1000 / LED_FRAME_MS, LED_COLOR_CYCLE_MS / 1000, s_thresh_dbfs,
             (int)(LED_BRIGHT_FLOOR_MIN * 100), (int)(LED_BRIGHT_FLOOR_MAX * 100),
             (int)(LED_BRIGHT_LOUD_MIN * 100), (int)(LED_BRIGHT_PEAK_MAX * 100));
    return ESP_OK;
}

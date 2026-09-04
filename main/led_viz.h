#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Single-pixel audio visualiser for the board's on-board addressable RGB LED
 * (WS2812/SK6812 on RADIO_LED_GPIO - see app_config.h).
 *
 * NOT WLED. WLED (github.com/Aircoookie/WLED) is a complete Arduino-framework
 * firmware application, not an ESP-IDF component: it owns the whole device
 * (its own Wi-Fi stack, web server, effect engine, and main loop) and cannot
 * be linked into an ESP-ADF app that is already using all of those. What it
 * contributes here is the approach, not the code - a single HSV pixel with
 * fast-attack/slow-decay level following, which is what makes WLED's own
 * sound-reactive modes read as musical rather than twitchy. The driver below
 * is ~150 lines of RMT, so this adds no dependency and no component-manager
 * churn to a project whose full rebuilds are already slow.
 *
 * How it hangs together:
 *   - led_viz_feed_pcm() is called from the decoder element's write callback
 *     (radio_pipeline.c), i.e. on the decoder's OWN task, with the same PCM
 *     that is on its way to i2s_stream. It only scans for a peak and stores
 *     one number - no allocation, no blocking, no extra ring-buffer hop. Same
 *     tap technique the companion esp32_bt_speaker uses for its I2S probe.
 *   - A separate render task turns that number into LIGHT at a fixed frame
 *     rate. Rendering is deliberately NOT done in the audio callback: the
 *     frame rate should be constant and independent of decoded block size,
 *     and an RMT transmit has no business happening on the decode path.
 *
 * 2026-09-04: colour and brightness were split apart (previously one HSV
 * sweep did both, hue keyed off the audio level):
 *   - COLOUR is now audio-independent: hue rotates through the full wheel on
 *     a fixed ~20s loop (LED_COLOR_CYCLE_MS in led_viz.c), continuously,
 *     whether or not anything is playing.
 *   - BRIGHTNESS is the audio peak, converted to dBFS (log scale, same
 *     conversion as before) and then gated against a threshold: at/below it
 *     the LED sits at little to no brightness (ordinary/medium listening
 *     level), and only a peak that actually crosses the threshold lights it
 *     up, steeply, toward "very bright". Fast-attack/slow-decay envelope
 *     following (unchanged) makes a flash decay smoothly back down rather
 *     than strobing frame to frame or staying lit - so the LED reads as an
 *     occasional peak flash, not a light that is just always on.
 * The threshold is runtime-adjustable (see led_viz_set_threshold_db() below;
 * console_cli.h's `led-thresh` command drives it) and persisted to NVS, so a
 * retune survives a restart.
 */

/* Initialises the LED, loads the persisted brightness threshold (or the
 * built-in default on first boot - see LED_DEFAULT_THRESHOLD_DBFS in
 * led_viz.c), and starts the render task. The boot colour cycle (red/green/
 * blue/cyan/magenta/yellow, 1s each at 20%) runs inside that task, so this
 * returns immediately and the cycle overlaps Wi-Fi association rather than
 * delaying boot by six seconds. Safe to call once, at startup. */
esp_err_t led_viz_start(void);

/*
 * Feeds one block of decoded PCM. Expects interleaved signed 16-bit samples
 * (what every decoder in this pipeline outputs). `bytes` is the block length
 * in BYTES, not samples. Cheap enough to call on the decode path: one pass
 * for a peak, one 32-bit store.
 *
 * Does nothing if led_viz_start() was never called or failed, so callers do
 * not have to check.
 */
void led_viz_feed_pcm(const void *pcm, size_t bytes);

/*
 * Runtime control of the peak-brightness threshold, in dBFS (same axis as
 * the internal peak->dB conversion: 0.0 = full scale, more negative = more
 * sensitive - quieter peaks light the LED up). Clamped to
 * [LED_FLOOR_DBFS, 0.0] (led_viz.c).
 *
 * led_viz_set_threshold_db() applies the new value immediately AND persists
 * it to NVS (namespace "led_viz"), so it survives a restart. Intended to be
 * driven from console_cli.h's `led-thresh` command, but not specific to it.
 * Returns the NVS commit result; the in-RAM value is applied either way - a
 * failed save just means the OLD value comes back after a restart, not that
 * this call failed to take effect right now.
 *
 * led_viz_get_threshold_db() returns whatever is currently in effect - the
 * built-in default before led_viz_start() has run, whatever was loaded from
 * NVS (or just set) after it has.
 */
esp_err_t led_viz_set_threshold_db(float threshold_dbfs);
float led_viz_get_threshold_db(void);

/*
 * 2026-09-04: LED/audio latency compensation.
 *
 * Physically, what the LED renders and what a human actually HEARS are two
 * different things separated by real time: this chip taps PCM right as it
 * enters the I2S ring buffer, but the sound that reaches a human ear has
 * since crossed the I2S wire to the companion esp32_bt_speaker chip, been
 * resampled, SBC-encoded, sent over Bluetooth A2DP, and decoded/amplified by
 * the BT speaker itself - a chain very likely dominated by the Bluetooth hop
 * (commonly 100s of ms for A2DP/SBC), not by anything on this chip. The LED
 * was reacting to audio the instant it was DECODED, not the (materially
 * later) instant it was actually HEARD.
 *
 * led_viz_set_latency_ms() compensates by delaying which point in the
 * level's own recent history gets RENDERED, without delaying the actual
 * audio going out over I2S at all - see led_viz.c's render loop. The value
 * is intended to be measured empirically (console_cli.h's `cal` command
 * drives an interactive human-in-the-loop measurement: play a tone, note
 * when it's actually heard) rather than guessed, since the true end-to-end
 * figure depends on hardware this chip cannot see or query.
 *
 * Persists to NVS (namespace "led_viz", same as the threshold above) and
 * survives a restart. Clamped to [0, LED_LATENCY_MAX_MS] (led_viz.c) - 0
 * (the default before anything has ever been calibrated) reproduces the
 * original zero-delay behaviour exactly.
 */
esp_err_t led_viz_set_latency_ms(int32_t latency_ms);
int32_t led_viz_get_latency_ms(void);

/*
 * Clears the level-history buffer the latency delay line reads from,
 * without touching the calibrated latency value itself. Call this around a
 * context switch that makes recent history meaningless for what's about to
 * be shown - console_cli.h's `cal` command calls this both entering AND
 * leaving calibration mode, so neither real playback's tail nor the test
 * tone's own history bleeds across that boundary. Safe to call at any time;
 * harmless (a few frames of black/silence) even outside that use case.
 */
void led_viz_reset_history(void);

#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * Interactive LED/audio latency calibration, driven entirely from the
 * serial console (console_cli.h's `cal`/`beep`/`heard`/`accept`/`cancel`
 * commands). See led_viz.h's doc comment on led_viz_set_latency_ms() for
 * WHY this exists: the LED reacts to PCM the instant it enters this chip's
 * I2S ring buffer, but a human hears it only after that audio has crossed
 * to the companion esp32_bt_speaker chip, been resampled, SBC-encoded, sent
 * over Bluetooth A2DP, and decoded/amplified there - a chain this chip
 * cannot see or query, so the true figure has to be measured, not guessed.
 *
 * How the measurement works: latency_cal_run() builds a small, self-
 * contained test-tone pipeline ([raw_stream]->[i2s_stream] - NOT the normal
 * HTTP/fMP4/decode pipeline, which is fully torn down first) and hands
 * control to an interactive loop. You start a trial (`beep`), which plays a
 * short tone and records the instant it entered the pipeline; the moment
 * you actually HEAR it from the speaker, you register that too (`heard`).
 * The difference is one round-trip's worth of latency. Repeat a few times
 * for a stable average, then `accept` to apply and persist it, or `cancel`
 * to discard and resume normal playback unchanged.
 *
 * Caveat worth knowing, not hiding: this necessarily measures "tone-trigger
 * to human button-press", which includes your own auditory reaction time
 * (commonly ~150-250ms) baked in alongside the true hardware/Bluetooth
 * latency - there is no way to separate the two using only a human in the
 * loop and no microphone on this board. In practice this is not a flaw for
 * what the result is actually used for: it calibrates the LED to feel
 * synchronized to a HUMAN hearing the sound, which is the whole point, not
 * to produce a lab-grade hardware latency figure. Averaging several trials
 * (the console flow encourages this) mainly reduces trial-to-trial reaction-
 * time JITTER; it does not remove that baseline reaction-time bias, which
 * is expected and fine.
 *
 * "All tasks killed/suspended" was the original ask for entering this mode;
 * what actually happens is narrower and deliberately so: the normal radio
 * pipeline (HTTP/fMP4/decode/I2S) is torn down and replaced by the minimal
 * test pipeline above, which is the part that actually matters (it removes
 * every source of audio-path interference from the measurement) - but
 * essential system tasks (Wi-Fi housekeeping, the console itself, the LED
 * renderer) stay alive on purpose, because calibration cannot function or
 * ever exit without them.
 */

/* Runs the full interactive calibration flow to completion (accepted or
 * cancelled) or until an unrecoverable error tearing down/rebuilding the
 * test pipeline. Blocks the calling task for the whole session - intended
 * to be called from radio_task in place of a normal pipeline restart, when
 * radio_pipeline_wait() reports a calibration request (see radio_pipeline.h).
 * Never returns an error the caller needs to act on beyond logging: any
 * failure inside this function falls back to leaving the LED's latency
 * exactly as it was before this call (nothing partially applied), and
 * normal playback resumes either way once this returns. */
void latency_cal_run(void);

/*
 * True for the entire duration of latency_cal_run() (from the moment it
 * starts tearing down the normal pipeline to the moment it hands control
 * back), false otherwise. console_cli.c checks this before dispatching
 * `beep`/`heard`/`accept`/`cancel` - those only mean anything while a
 * calibration session is actually in progress, and must never be enqueued
 * as a stray command outside one (radio_pipeline_wait() would have no
 * meaningful way to interpret them). Safe to call from any task.
 */
bool latency_cal_is_active(void);

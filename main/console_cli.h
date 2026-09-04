#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include "audio_event_iface.h"

/*
 * Interactive command-line console for controlling this chip directly from
 * a laptop, over the SAME USB/serial link already used for flashing and log
 * output - no extra wiring, no second cable. Answers the "can it be done via
 * serial on esp-idf?" question: yes, via ESP-IDF's own `console` component
 * (esp_console.h), which is exactly what this file wires up.
 *
 * WHICH PORT: this board's console is UART0 (CONFIG_ESP_CONSOLE_UART_NUM=0
 * / CONFIG_ESP_CONSOLE_UART_DEFAULT=y in sdkconfig), reached in practice
 * through a CH340 USB-UART bridge - confirmed by build.ps1's `-Port`
 * default and its own comment ("COM10, CH340"), not the S3's native
 * USB-Serial-JTAG pins (GPIO19/20, which app_config.h keeps reserved out of
 * general caution for any S3 board, whether or not THIS one wires them out
 * anywhere). So the REPL below is built with esp_console_new_repl_uart(),
 * matching CONFIG_ESP_CONSOLE_UART_NUM - the same port `idf.py monitor`
 * already opens. Typed commands and ESP_LOG output share that one terminal;
 * a log line can land mid-line while typing. That is standard esp_console
 * behaviour (every ESP-IDF console example does the same), not a bug here.
 *
 * Commands (type `help` at the `radio>` prompt for the full generated
 * list):
 *   next / prev        - same effect as an AVRCP NEXT/PREVIOUS press over
 *                         the BT-speaker UART link (avrcp_uart.h): ends the
 *                         current session and starts the next/previous
 *                         station from station_list.h, persisted to NVS.
 *   led-thresh [dBFS]  - get (no argument) or set (argument) the on-board
 *                         LED's peak-brightness threshold - see led_viz.h.
 *                         A set applies immediately and persists (NVS),
 *                         surviving a restart.
 *   latency [ms]       - get (no argument) or manually set (argument) the
 *                         LED/audio latency compensation - see led_viz.h's
 *                         led_viz_set_latency_ms(). A set applies and
 *                         persists immediately, same as led-thresh. For
 *                         measuring this properly rather than guessing, see
 *                         `cal` below.
 *   cal                - enters interactive LED/audio latency calibration
 *                         (latency_cal.h) - ends the current session (like
 *                         next/prev) and hands the console over to a guided
 *                         beep/heard measurement flow instead of resuming
 *                         normal playback. Once inside that flow:
 *                           beep     - play a test tone now
 *                           <Enter>  - (nothing typed) register hearing the tone -
 *                                      'heard' still works too, just slower
 *                           <number> - record that many ms as a trial directly,
 *                                      no beep needed - mixes with real trials
 *                           accept   - save the measured average, resume playback
 *                           cancel   - discard, resume playback unchanged
 *   status             - prints the currently playing station, the LED
 *                         threshold, and the latency compensation.
 *
 * Architecture mirrors avrcp_uart.h exactly, as a second, independent
 * source of the same next/previous command: esp_console's own REPL task
 * (not one this file creates) parses a line and calls a command handler,
 * which pushes into a small FreeRTOS queue; a doorbell event_iface then
 * wakes radio_pipeline_wait() immediately instead of waiting out its ~1s
 * poll. radio_pipeline_wait() drains BOTH this queue and avrcp_uart's every
 * iteration, so either input works at any time, independently.
 *
 * The CAL_* values below are NOT drained by radio_pipeline_wait() at all
 * (CAL_ENTER is the one exception - see radio_pipeline.h) - beep/heard/
 * accept/cancel only mean anything once latency_cal_run() is already
 * driving its own dedicated loop (main.c), which is the only thing that
 * ever takes them off this queue. Their command handlers below check
 * latency_cal_is_active() first and refuse (printing why) rather than
 * enqueueing anything if a calibration session is not actually in progress -
 * otherwise a stray 'beep' typed outside calibration would sit in this
 * queue and could be misread by radio_pipeline_wait()'s next/prev handling.
 */
typedef enum {
    CONSOLE_CMD_NONE = 0,
    CONSOLE_CMD_NEXT,
    CONSOLE_CMD_PREV,
    CONSOLE_CMD_CAL_ENTER,
    CONSOLE_CMD_CAL_BEEP,
    CONSOLE_CMD_CAL_HEARD,
    CONSOLE_CMD_CAL_ACCEPT,
    CONSOLE_CMD_CAL_CANCEL,
    /* (during 'cal' only) a bare number typed at the prompt instead of a
     * beep/heard round-trip - see console_cli_take_pending_cal_ms() and
     * console_repl_task()'s own comment (console_cli.c) for how this gets
     * here, and latency_cal_run()'s own CONSOLE_CMD_CAL_SET_MS case
     * (latency_cal.c) for what consumes it. */
    CONSOLE_CMD_CAL_SET_MS,
} console_cmd_t;

/* Starts the console REPL, registers the commands above, creates the
 * command queue and the doorbell event interface. Call once at boot. */
esp_err_t console_cli_start(void);

/*
 * The doorbell event source - same usage pattern as
 * avrcp_uart_get_event_iface(). Register it with
 *   audio_event_iface_set_listener(console_cli_get_event_iface(), listener)
 * AFTER audio_pipeline_set_listener() (which rebuilds the listener's queue
 * set from scratch), and drop it again with
 * audio_event_iface_remove_listener() before destroying that listener.
 *
 * Returns NULL if console_cli_start() was never called or failed.
 */
audio_event_iface_handle_t console_cli_get_event_iface(void);

/* Takes the next pending NEXT/PREVIOUS command, or CONSOLE_CMD_NONE if none
 * is queued within `wait` ticks (pass 0 to just drain what is already
 * there). Returns CONSOLE_CMD_NONE if console_cli_start() was never called
 * or failed. */
console_cmd_t console_cli_take_command(TickType_t wait);

/*
 * The value that accompanies a CONSOLE_CMD_CAL_SET_MS command - a single-
 * slot handoff, not a queue: console_repl_task() (console_cli.c) sets it
 * immediately before enqueueing that command, and never writes another
 * value before that command is dequeued and consumed, because it blocks on
 * the next linenoise() read in between - there is no code path that could
 * type a second number before the first is consumed. Call this exactly
 * once, right after taking a CONSOLE_CMD_CAL_SET_MS off the queue (see
 * latency_cal.c). Meaningless for any other command. */
int32_t console_cli_take_pending_cal_ms(void);

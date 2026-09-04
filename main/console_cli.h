#pragma once

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
 *   status             - prints the currently playing station and the LED
 *                         threshold.
 *
 * Architecture mirrors avrcp_uart.h exactly, as a second, independent
 * source of the same next/previous command: esp_console's own REPL task
 * (not one this file creates) parses a line and calls a command handler,
 * which pushes into a small FreeRTOS queue; a doorbell event_iface then
 * wakes radio_pipeline_wait() immediately instead of waiting out its ~1s
 * poll. radio_pipeline_wait() drains BOTH this queue and avrcp_uart's every
 * iteration, so either input works at any time, independently.
 */
typedef enum {
    CONSOLE_CMD_NONE = 0,
    CONSOLE_CMD_NEXT,
    CONSOLE_CMD_PREV,
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

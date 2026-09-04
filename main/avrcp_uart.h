#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include "audio_event_iface.h"

/*
 * Receiver for the AVRCP-command bridge coming from the companion
 * esp32_bt_speaker chip over a one-way UART link.
 *
 * That chip is the AVRCP TARGET for the paired Bluetooth speaker (see its
 * main.c: avrc_tg_event_cb() / avrc_uart_send_line() / init_avrc_uart(),
 * and the wire-format comment next to RADIO_AVRC_UART_PORT in its
 * app_config.h). Every AVRCP event it receives is forwarded as one ASCII
 * line, '\n'-terminated, ':'-separated, on its UART2 TX (its GPIO16) -
 * wired to this chip's RX (RADIO_AVRCP_UART_RX_GPIO, app_config.h) plus a
 * common ground, 115200 8N1, no flow control. This chip only ever receives;
 * no TX pin is configured here, and nothing is ever sent back.
 *
 * Line formats (verified against that chip's source):
 *   AVRCP:CONN:<CONNECTED|DISCONNECTED>
 *   AVRCP:CMD:<name>:<PRESSED|RELEASED>
 *   AVRCP:VOL:<0-127>
 * <name> is PLAY/PAUSE/STOP/NEXT/PREVIOUS/REWIND/FAST_FORWARD/VOL_UP/
 * VOL_DOWN/MUTE, or a "0xXX" hex fallback for any other AVRCP passthrough
 * key code. Only NEXT/PREVIOUS PRESSED are acted on (station skip - this is
 * live radio, not track playback, so there is no track to seek within);
 * everything else is parsed but ignored, and any malformed/unrecognized
 * line is dropped rather than treated as an error - this protocol is
 * expected to grow, and a line glitch (baud mismatch, noise on the wire)
 * must never crash the parser or take down playback.
 *
 * DELIVERY IS FULLY EVENT-DRIVEN - a button press is registered the instant
 * its bytes arrive, never on a timer. Three pieces:
 *
 *   1. A dedicated task blocks in uart_read_bytes(portMAX_DELAY), so it is
 *      woken by the UART driver's own RX interrupt rather than waking up to
 *      check. It costs one task and its stack, permanently - which on this
 *      ESP32-S3-WROOM-1 N16R8 (8MB PSRAM, 16MB flash) is not a tradeoff
 *      worth thinking about.
 *   2. Commands go into a real FreeRTOS queue (avrcp_uart_take_command()),
 *      which is the authoritative delivery path: a press is never lost just
 *      because nothing happened to be listening at that instant. It queues
 *      even while radio_task is mid TuneIn-resolve or backing off, and is
 *      taken as soon as the pipeline is up again.
 *   3. The doorbell (avrcp_uart_get_event_iface()) exists so the consumer
 *      does not have to block on the queue INSTEAD of on ADF's pipeline
 *      events. Registering it via audio_event_iface_set_listener() puts it
 *      in the same FreeRTOS queue set radio_pipeline_wait() is already
 *      blocked on, so a press unblocks that wait immediately - the same
 *      mechanism every ESP-ADF example uses to fold button/peripheral
 *      events into a pipeline event loop.
 */
typedef enum {
    AVRCP_CMD_NONE = 0,
    AVRCP_CMD_NEXT,
    AVRCP_CMD_PREV,
} avrcp_cmd_t;

/* Installs the UART driver, creates the command queue and the doorbell
 * event interface, and starts the reader task. Call once at boot. */
esp_err_t avrcp_uart_start(void);

/*
 * The doorbell event source. Register it with
 *   audio_event_iface_set_listener(avrcp_uart_get_event_iface(), listener)
 * AFTER audio_pipeline_set_listener() (which rebuilds the listener's queue
 * set from scratch), and drop it again with
 * audio_event_iface_remove_listener() before destroying that listener.
 *
 * The messages it sends carry source_type AUDIO_ELEMENT_TYPE_PERIPH and no
 * payload: they exist only to wake the listener, which then takes the
 * actual command from avrcp_uart_take_command(). Keeping the payload out of
 * the doorbell is deliberate - audio_event_iface_set_listener() DRAINS
 * member queues when it rebuilds a queue set, so a message living only
 * there could be discarded at pipeline start; the command queue in (2)
 * above is never touched by ADF.
 *
 * Returns NULL if avrcp_uart_start() was never called or failed.
 */
audio_event_iface_handle_t avrcp_uart_get_event_iface(void);

/* Takes the next pending NEXT/PREVIOUS command, or AVRCP_CMD_NONE if none
 * is queued within `wait` ticks (pass 0 to just drain what is already
 * there). Returns AVRCP_CMD_NONE if avrcp_uart_start() was never called or
 * failed. */
avrcp_cmd_t avrcp_uart_take_command(TickType_t wait);

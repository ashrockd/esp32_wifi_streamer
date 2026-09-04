#include "avrcp_uart.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "audio_common.h"

#include "app_config.h"

static const char *TAG = "AVRCP_UART";

/* The sender's own avrc_uart_send_line() (esp32_bt_speaker/main/main.c)
 * caps every line at 80 bytes (char line[80]); this is generous headroom
 * above that, not a tightly-fit value. */
#define AVRCP_LINE_MAX 96

/* The UART driver's own RX ring buffer. Sized well past anything this
 * protocol produces (a press is two ~24-byte lines) purely so that a burst
 * arriving while the reader task is briefly descheduled cannot overrun it -
 * this board has RAM to spare and there is no reason to run this tight. */
#define AVRCP_UART_RX_BUF_BYTES 1024

/* Reader task: blocks in uart_read_bytes(portMAX_DELAY), so it runs only
 * when bytes actually arrive. The stack only has to cover a 128-byte read
 * buffer plus ESP_LOG formatting; 4096 is deliberately roomy. Priority is
 * above radio_task's (5) so a press is parsed and queued the moment it
 * lands, rather than waiting behind pipeline work. */
#define AVRCP_TASK_STACK_BYTES 4096
#define AVRCP_TASK_PRIO        10

/* Deep enough that a rapid double-press is queued rather than coalesced.
 * Each entry is one byte. */
#define AVRCP_CMD_QUEUE_LEN 8

/* Which actionable command was last DISPATCHED, and when - a simple
 * duplicate suppressor, NOT a press/hold state machine. AVRCP controllers
 * are specced to send PRESSED then RELEASED per physical press, and this
 * link has been hardware-confirmed to sometimes deliver the SAME PRESSED
 * line twice in the same millisecond for one physical press (one PREVIOUS
 * press skipped two stations) - so acting on every PRESSED unconditionally
 * is not enough.
 *
 * 2026-09-04, REVIEWED AND REPLACED: the original fix here latched on
 * PRESSED and required a matching RELEASED (or a 3s failsafe timeout) to
 * unlatch before the SAME command could fire again. That over-corrected:
 * this link never acknowledges anything, so there is no way to know a
 * RELEASED was lost rather than just slow, and ANY missed/delayed RELEASED
 * left the key "held" for up to 3 real seconds, silently swallowing a
 * genuine quick re-press in that window. That reads as "the remote doesn't
 * respond" for anyone actually pressing next/previous a few times in a row
 * - a normal way to use it, and the reported symptom this replaces.
 *
 * A short, fixed debounce window is simpler and does not depend on RELEASED
 * arriving at all: ignore a PRESSED only if the SAME command was just
 * dispatched within AVRCP_DEBOUNCE_US. That comfortably covers the
 * confirmed same-millisecond duplicate (and similar jitter) while staying
 * comfortably under any realistic human re-press interval. RELEASED lines
 * carry no state to update any more - see handle_line() - they are simply
 * not acted on.
 *
 * Touched only by the reader task, so no locking. */
static avrcp_cmd_t s_last_cmd;
static int64_t s_last_dispatch_us;

/* See s_last_cmd's comment above: long enough to absorb the confirmed
 * same-millisecond duplicate PRESSED with headroom, short enough that no
 * realistic human re-press of the same button is ever mistaken for one. */
#define AVRCP_DEBOUNCE_US (200 * 1000)

static bool s_started;
static QueueHandle_t s_cmd_queue;
static audio_event_iface_handle_t s_evt;
static TaskHandle_t s_task;

/* Parses one complete, NUL-terminated line (no trailing \n/\r) and returns
 * the command it maps to, or AVRCP_CMD_NONE for anything not acted on here
 * (RELEASED, VOL, CONN, unrecognized/malformed - all deliberately silent,
 * never treated as an error - see avrcp_uart.h's protocol comment). */
static avrcp_cmd_t handle_line(char *line)
{
    if (strncmp(line, "AVRCP:CONN:", 11) == 0) {
        /* Logged for visibility only - no behavior is wired to link state
         * (out of scope; only next/prev station switching was requested).
         * Nothing to reset here any more: s_last_cmd/s_last_dispatch_us is a
         * plain time-based debounce, not a latch that a dropped link could
         * leave stuck - see its comment above. */
        ESP_LOGI(TAG, "AVRCP link: %s", line + 11);
        return AVRCP_CMD_NONE;
    }

    if (strncmp(line, "AVRCP:CMD:", 10) == 0) {
        char *rest = line + 10;
        char *sep = strchr(rest, ':');
        if (!sep) {
            ESP_LOGD(TAG, "Malformed AVRCP:CMD line, ignoring: %s", line);
            return AVRCP_CMD_NONE;
        }
        *sep = '\0';
        const char *name = rest;
        const char *state = sep + 1;

        avrcp_cmd_t cmd = AVRCP_CMD_NONE;
        if (strcmp(name, "NEXT") == 0) {
            cmd = AVRCP_CMD_NEXT;
        } else if (strcmp(name, "PREVIOUS") == 0) {
            cmd = AVRCP_CMD_PREV;
        } else {
            /* PLAY/PAUSE/STOP/REWIND/FAST_FORWARD/VOL_UP/VOL_DOWN/MUTE/hex -
             * not acted on here, deliberately. */
            ESP_LOGD(TAG, "Ignoring AVRCP command '%s' (%s)", name, state);
            return AVRCP_CMD_NONE;
        }

        /* RELEASED carries no state to update any more - see s_last_cmd's
         * comment above - just not acted on. */
        if (strcmp(state, "PRESSED") != 0) {
            return AVRCP_CMD_NONE;
        }

        int64_t now_us = esp_timer_get_time();
        if (cmd == s_last_cmd && (now_us - s_last_dispatch_us) < AVRCP_DEBOUNCE_US) {
            ESP_LOGD(TAG, "Ignoring %s PRESSED within %dms of the last one (debounce)",
                     name, (int)(AVRCP_DEBOUNCE_US / 1000));
            return AVRCP_CMD_NONE;
        }

        s_last_cmd = cmd;
        s_last_dispatch_us = now_us;
        ESP_LOGI(TAG, "AVRCP command: %s", name);
        return cmd;
    }

    if (strncmp(line, "AVRCP:VOL:", 10) == 0) {
        ESP_LOGD(TAG, "AVRCP volume: %s", line + 10);
        return AVRCP_CMD_NONE;
    }

    ESP_LOGD(TAG, "Ignoring unrecognized AVRCP UART line: %s", line);
    return AVRCP_CMD_NONE;
}

/* Queues the command, then rings the doorbell so whoever is blocked on a
 * listener that includes s_evt wakes up now rather than at its next timeout.
 * Queue first, doorbell second: the queue is what actually carries the
 * command, so it must be populated before anything can be woken to look for
 * it. */
static void dispatch_command(avrcp_cmd_t cmd)
{
    uint8_t item = (uint8_t)cmd;
    if (xQueueSend(s_cmd_queue, &item, 0) != pdTRUE) {
        /* Only reachable if AVRCP_CMD_QUEUE_LEN presses have piled up
         * without a single one being taken - i.e. the consumer is wedged,
         * not a pacing problem. Dropping the newest and saying so is more
         * honest than silently evicting an older one. */
        ESP_LOGW(TAG, "Command queue full (%d pending); dropping this press", AVRCP_CMD_QUEUE_LEN);
        return;
    }

    if (s_evt) {
        /* Payload-free wake-up - see avrcp_uart_get_event_iface()'s comment
         * on why the command itself does not travel this way. PERIPH is the
         * source type ESP-ADF itself uses for non-element event sources
         * folded into a pipeline event loop. */
        audio_event_iface_msg_t msg = {
            .source_type = AUDIO_ELEMENT_TYPE_PERIPH,
            .cmd = 0,
            .data = NULL,
            .data_len = 0,
            .source = NULL,
            .need_free_data = false,
        };
        audio_event_iface_sendout(s_evt, &msg);
    }
}

static void avrcp_uart_task(void *arg)
{
    /* Line assembly state lives on this task's stack - nothing else touches
     * it, so no locking and no statics. */
    char line[AVRCP_LINE_MAX];
    size_t line_len = 0;
    uint8_t chunk[128];
    int64_t last_dump_us = 0;

    ESP_LOGI(TAG, "AVRCP UART reader task running (blocking on RX, no polling)");

    while (true) {
        /* Blocks in the driver until the RX interrupt has bytes for us -
         * this is the whole reason there is a task here. */
        int n = uart_read_bytes(RADIO_AVRCP_UART_PORT, chunk, sizeof(chunk), portMAX_DELAY);
        if (n <= 0) {
            continue; /* transient driver error; nothing to do but keep reading */
        }

        bool overflowed = false;
        for (int i = 0; i < n; i++) {
            char c = (char)chunk[i];

            if (c == '\r') {
                continue; /* wire format is \n-terminated only, but tolerate stray \r defensively */
            }

            if (c == '\n') {
                line[line_len] = '\0';
                if (line_len > 0) {
                    avrcp_cmd_t cmd = handle_line(line);
                    if (cmd != AVRCP_CMD_NONE) {
                        dispatch_command(cmd);
                    }
                }
                line_len = 0;
                continue;
            }

            if (line_len + 1 >= sizeof(line)) {
                /* A line longer than the sender ever actually produces -
                 * baud mismatch/noise, most likely. Drop what's buffered and
                 * resync on the next newline rather than truncating into a
                 * different, wrong-looking command. */
                ESP_LOGW(TAG, "AVRCP UART line overflowed %d bytes; discarding and resyncing", AVRCP_LINE_MAX);
                line_len = 0;
                overflowed = true;
                continue;
            }

            line[line_len++] = c;
        }

        /* When bytes are arriving but never forming a valid line, a hex dump
         * of what's actually on the wire is far more diagnostic than the
         * overflow warning alone - garbage that looks like real (but
         * misaligned) protocol bytes points at a baud mismatch, while
         * near-random/repetitive bytes point at electrical noise (a floating
         * or ungrounded line). Throttled to once every 5s: a genuinely
         * noisy/disconnected line would otherwise produce this continuously,
         * which is just a second flavor of log flood rather than a fix. */
        if (overflowed) {
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_dump_us > 5 * 1000 * 1000) {
                last_dump_us = now_us;
                char hex[3 * 32 + 1];
                size_t used = 0;
                for (int i = 0; i < n && used + 3 < sizeof(hex); i++) {
                    used += (size_t)snprintf(hex + used, sizeof(hex) - used, "%02X ", chunk[i]);
                }
                ESP_LOGW(TAG, "Raw bytes on the AVRCP UART line (%d read): %s", n, hex);
            }
        }
    }
}

esp_err_t avrcp_uart_start(void)
{
    const uart_config_t cfg = {
        .baud_rate = RADIO_AVRCP_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* RX ring buffer only - this link is receive-only, so no TX buffer. No
     * driver event queue either: the task below blocks directly on
     * uart_read_bytes(), which is woken by the same RX interrupt an event
     * queue would have been fed from, without the second hop. */
    esp_err_t err = uart_driver_install(RADIO_AVRCP_UART_PORT, AVRCP_UART_RX_BUF_BYTES, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(RADIO_AVRCP_UART_PORT, &cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(RADIO_AVRCP_UART_PORT, UART_PIN_NO_CHANGE,
                           RADIO_AVRCP_UART_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AVRCP UART configuration failed: %s", esp_err_to_name(err));
        uart_driver_delete(RADIO_AVRCP_UART_PORT);
        return err;
    }

    /* A UART idle line sits high; a floating/loosely-connected RX pin has no
     * reason to sit anywhere in particular and picks up whatever noise is
     * nearby, which reads back as a stream of random bytes. An internal weak
     * pull-up costs nothing when the line IS properly driven (the sender's
     * TX output easily overpowers it) but holds it at a clean idle-high
     * whenever it isn't - cheap insurance against a loose/intermittent
     * connection. NOTE: this cannot fix - and is not a substitute for - two
     * boards that don't share a common GND, which is the most likely real
     * explanation for fully garbled (rather than occasionally glitched)
     * reception. */
    esp_err_t pull_err = gpio_set_pull_mode((gpio_num_t)RADIO_AVRCP_UART_RX_GPIO, GPIO_PULLUP_ONLY);
    if (pull_err != ESP_OK) {
        ESP_LOGW(TAG, "Could not enable RX pull-up on GPIO%d: %s (non-fatal)",
                 RADIO_AVRCP_UART_RX_GPIO, esp_err_to_name(pull_err));
    }

    s_cmd_queue = xQueueCreate(AVRCP_CMD_QUEUE_LEN, sizeof(uint8_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "Could not create AVRCP command queue");
        uart_driver_delete(RADIO_AVRCP_UART_PORT);
        return ESP_ERR_NO_MEM;
    }

    /* The doorbell. Only its external queue is ever used (that is the one
     * audio_event_iface_set_listener() folds into a listener's queue set),
     * but the default config sizes all three, which is fine. */
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    s_evt = audio_event_iface_init(&evt_cfg);
    if (!s_evt) {
        ESP_LOGE(TAG, "Could not create AVRCP doorbell event interface");
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        uart_driver_delete(RADIO_AVRCP_UART_PORT);
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(avrcp_uart_task, "avrcp_uart", AVRCP_TASK_STACK_BYTES,
                    NULL, AVRCP_TASK_PRIO, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "Could not create AVRCP UART reader task");
        audio_event_iface_destroy(s_evt);
        s_evt = NULL;
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        uart_driver_delete(RADIO_AVRCP_UART_PORT);
        return ESP_ERR_NO_MEM;
    }

    s_started = true;

    ESP_LOGI(TAG, "AVRCP UART receiver: UART%d RX=GPIO%d @ %d baud, interrupt-driven",
             RADIO_AVRCP_UART_PORT, RADIO_AVRCP_UART_RX_GPIO, RADIO_AVRCP_UART_BAUD);
    return ESP_OK;
}

audio_event_iface_handle_t avrcp_uart_get_event_iface(void)
{
    return s_started ? s_evt : NULL;
}

avrcp_cmd_t avrcp_uart_take_command(TickType_t wait)
{
    if (!s_started) {
        return AVRCP_CMD_NONE;
    }

    uint8_t item = 0;
    if (xQueueReceive(s_cmd_queue, &item, wait) != pdTRUE) {
        return AVRCP_CMD_NONE;
    }
    return (avrcp_cmd_t)item;
}

#include "console_cli.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "freertos/queue.h"

#include "audio_common.h"

#include "latency_cal.h"
#include "led_viz.h"
#include "station_list.h"

static const char *TAG = "CONSOLE";

/* Deep enough that a couple of quick keystrokes are never lost even if
 * radio_pipeline_wait() is briefly busy elsewhere - same size/reasoning as
 * avrcp_uart.c's command queue. Each entry is one byte. */
#define CONSOLE_CMD_QUEUE_LEN 8

static bool s_started;
static QueueHandle_t s_cmd_queue;
static audio_event_iface_handle_t s_evt;
static esp_console_repl_t *s_repl;

/* Queues the command, then rings the doorbell - identical pattern to (and
 * fully independent of) avrcp_uart.c's dispatch_command(); see
 * console_cli.h's architecture note. */
static void dispatch_command(console_cmd_t cmd)
{
    uint8_t item = (uint8_t)cmd;
    if (xQueueSend(s_cmd_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Command queue full (%d pending); dropping this command", CONSOLE_CMD_QUEUE_LEN);
        return;
    }

    if (s_evt) {
        /* Payload-free wake-up - see console_cli.h's doc comment on why the
         * command itself does not travel this way. PERIPH is the source
         * type ESP-ADF itself uses for non-element event sources folded
         * into a pipeline event loop (avrcp_uart.c does the same). */
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

static int cmd_next(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("Requesting next station...\n");
    dispatch_command(CONSOLE_CMD_NEXT);
    return 0;
}

static int cmd_prev(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("Requesting previous station...\n");
    dispatch_command(CONSOLE_CMD_PREV);
    return 0;
}

static int cmd_led_thresh(int argc, char **argv)
{
    if (argc < 2) {
        printf("LED peak-brightness threshold: %.1f dBFS\n", led_viz_get_threshold_db());
        return 0;
    }

    char *end = NULL;
    float db = strtof(argv[1], &end);
    if (end == argv[1]) {
        printf("Usage: led-thresh [dBFS]   (no argument prints the current value)\n"
               "  e.g. 'led-thresh -15' - lower (more negative) = more sensitive, quieter\n"
               "  peaks light the LED; 0 = only true full-scale peaks do.\n");
        return 1;
    }

    /* Clamps internally and logs its own outcome (including any NVS
     * failure) - see led_viz_set_threshold_db(); the value it reports back
     * below is whatever was actually applied, post-clamp. */
    led_viz_set_threshold_db(db);
    printf("LED peak-brightness threshold now %.1f dBFS (saved; survives a restart)\n",
           led_viz_get_threshold_db());
    return 0;
}

static int cmd_latency(int argc, char **argv)
{
    if (argc < 2) {
        printf("LED/audio latency compensation: %" PRId32 "ms\n", led_viz_get_latency_ms());
        printf("(measure this properly with 'cal' instead of guessing, if you have not already)\n");
        return 0;
    }

    char *end = NULL;
    long ms = strtol(argv[1], &end, 10);
    if (end == argv[1]) {
        printf("Usage: latency [ms]   (no argument prints the current value)\n"
               "  e.g. 'latency 220' - manually sets it; prefer 'cal' for a measured value.\n");
        return 1;
    }

    led_viz_set_latency_ms((int32_t)ms);
    printf("LED/audio latency compensation now %" PRId32 "ms (saved; survives a restart)\n",
           led_viz_get_latency_ms());
    return 0;
}

static int cmd_cal(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (latency_cal_is_active()) {
        printf("Already in calibration mode.\n");
        return 0;
    }
    printf("Entering LED/audio latency calibration - ending the current session...\n");
    dispatch_command(CONSOLE_CMD_CAL_ENTER);
    return 0;
}

/* beep/heard/accept/cancel only mean anything while latency_cal_run() is
 * actively draining this queue for them - see console_cli.h's doc comment
 * on why they must not be enqueued otherwise. */
static int cmd_cal_beep(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!latency_cal_is_active()) {
        printf("Not in calibration mode - type 'cal' first.\n");
        return 1;
    }
    dispatch_command(CONSOLE_CMD_CAL_BEEP);
    return 0;
}

static int cmd_cal_heard(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!latency_cal_is_active()) {
        printf("Not in calibration mode - type 'cal' first.\n");
        return 1;
    }
    dispatch_command(CONSOLE_CMD_CAL_HEARD);
    return 0;
}

static int cmd_cal_accept(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!latency_cal_is_active()) {
        printf("Not in calibration mode - nothing to accept.\n");
        return 1;
    }
    dispatch_command(CONSOLE_CMD_CAL_ACCEPT);
    return 0;
}

static int cmd_cal_cancel(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!latency_cal_is_active()) {
        printf("Not in calibration mode - nothing to cancel.\n");
        return 1;
    }
    dispatch_command(CONSOLE_CMD_CAL_CANCEL);
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint8_t idx = station_list_get_now_playing();
    if (idx < RADIO_STATION_COUNT) {
        printf("Station : %s (%s)  [%u/%u]\n",
               RADIO_STATIONS[idx].name, RADIO_STATIONS[idx].id,
               (unsigned)(idx + 1), (unsigned)RADIO_STATION_COUNT);
    } else {
        printf("Station : none playing yet\n");
    }
    printf("LED     : peak-brightness threshold %.1f dBFS, latency compensation %" PRId32 "ms\n",
           led_viz_get_threshold_db(), led_viz_get_latency_ms());
    if (latency_cal_is_active()) {
        printf("          (latency calibration currently in progress - type 'cancel' to abort)\n");
    }
    return 0;
}

static void register_commands(void)
{
    ESP_ERROR_CHECK(esp_console_register_help_command());

    const esp_console_cmd_t next_cmd = {
        .command = "next",
        .help = "Skip to the next station (same effect as an AVRCP NEXT press; persists across restarts)",
        .hint = NULL,
        .func = &cmd_next,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&next_cmd));

    const esp_console_cmd_t prev_cmd = {
        .command = "prev",
        .help = "Go back to the previous station (same effect as an AVRCP PREVIOUS press; persists across restarts)",
        .hint = NULL,
        .func = &cmd_prev,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&prev_cmd));

    const esp_console_cmd_t thresh_cmd = {
        .command = "led-thresh",
        .help = "Get/set the on-board LED's peak-brightness threshold in dBFS (persists across restarts)",
        .hint = "[dBFS]",
        .func = &cmd_led_thresh,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&thresh_cmd));

    const esp_console_cmd_t latency_cmd = {
        .command = "latency",
        .help = "Get/manually set the LED/audio latency compensation in ms (persists; prefer 'cal')",
        .hint = "[ms]",
        .func = &cmd_latency,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&latency_cmd));

    const esp_console_cmd_t cal_cmd = {
        .command = "cal",
        .help = "Enter interactive LED/audio latency calibration (see latency_cal.h)",
        .hint = NULL,
        .func = &cmd_cal,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cal_cmd));

    const esp_console_cmd_t beep_cmd = {
        .command = "beep",
        .help = "(during 'cal' only) play a test tone and start timing",
        .hint = NULL,
        .func = &cmd_cal_beep,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&beep_cmd));

    const esp_console_cmd_t heard_cmd = {
        .command = "heard",
        .help = "(during 'cal' only) register the instant you heard the test tone",
        .hint = NULL,
        .func = &cmd_cal_heard,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&heard_cmd));

    const esp_console_cmd_t accept_cmd = {
        .command = "accept",
        .help = "(during 'cal' only) save the measured latency average and resume playback",
        .hint = NULL,
        .func = &cmd_cal_accept,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&accept_cmd));

    const esp_console_cmd_t cancel_cmd = {
        .command = "cancel",
        .help = "(during 'cal' only) discard calibration and resume playback unchanged",
        .hint = NULL,
        .func = &cmd_cal_cancel,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cancel_cmd));

    const esp_console_cmd_t status_cmd = {
        .command = "status",
        .help = "Show the currently playing station, LED threshold, and latency compensation",
        .hint = NULL,
        .func = &cmd_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&status_cmd));
}

esp_err_t console_cli_start(void)
{
    s_cmd_queue = xQueueCreate(CONSOLE_CMD_QUEUE_LEN, sizeof(uint8_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "Could not create console command queue");
        return ESP_ERR_NO_MEM;
    }

    /* The doorbell. Only its external queue is ever used (that is the one
     * audio_event_iface_set_listener() folds into a listener's queue set),
     * but the default config sizes all three, which is fine - same as
     * avrcp_uart.c's identical setup. */
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    s_evt = audio_event_iface_init(&evt_cfg);
    if (!s_evt) {
        ESP_LOGE(TAG, "Could not create console doorbell event interface");
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "radio> ";

    /* UART, matching CONFIG_ESP_CONSOLE_UART_NUM/_DEFAULT - see this file's
     * header comment for why (this board's actual laptop link is UART0 via
     * a CH340 bridge, not the S3's native USB-Serial-JTAG pins). Passing the
     * *_DEFAULT() macro rather than hardcoding fields keeps this in lockstep
     * with whatever sdkconfig says the console port/baud actually is. */
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

    esp_err_t err = esp_console_new_repl_uart(&uart_config, &repl_config, &s_repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_console_new_repl_uart failed: %s", esp_err_to_name(err));
        audio_event_iface_destroy(s_evt);
        s_evt = NULL;
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        return err;
    }

    register_commands();

    err = esp_console_start_repl(s_repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_console_start_repl failed: %s", esp_err_to_name(err));
        audio_event_iface_destroy(s_evt);
        s_evt = NULL;
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        s_repl = NULL;
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "Serial console ready on UART%d @ %d baud - type 'help' for commands",
             uart_config.channel, uart_config.baud_rate);
    return ESP_OK;
}

audio_event_iface_handle_t console_cli_get_event_iface(void)
{
    return s_started ? s_evt : NULL;
}

console_cmd_t console_cli_take_command(TickType_t wait)
{
    if (!s_started) {
        return CONSOLE_CMD_NONE;
    }

    uint8_t item = 0;
    if (xQueueReceive(s_cmd_queue, &item, wait) != pdTRUE) {
        return CONSOLE_CMD_NONE;
    }
    return (console_cmd_t)item;
}

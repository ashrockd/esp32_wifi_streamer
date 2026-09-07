#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include "audio_event_iface.h"
#include "avrcp_uart.h"

/*
 * companion_server - a small HTTP+JSON server for the DP666 companion
 * display (esp32_dp666_companion, a separate PlatformIO/Arduino repo forked
 * from PE5PVB/TEF6686_ESP32 - see that project's own CLAUDE.md). This is a
 * THIRD, independent WiFi/HTTP/mDNS link, unrelated to the two existing
 * I2S/UART links to esp32_bt_speaker (radio_pipeline.c's I2S output,
 * avrcp_uart.c's UART input) - this chip runs a plain esp_http_server, the
 * DP666 polls it, this chip never has to know the DP666's address (the
 * "poll model" decision - see the plan).
 *
 * Built on ESP-IDF's esp_http_server (REQUIRES in main/CMakeLists.txt) plus
 * the espressif/mdns managed component (main/idf_component.yml - not a
 * built-in IDF v5.x component, split out to the Component Registry) so the
 * DP666 can find this chip by hostname (RADIO_COMPANION_MDNS_HOSTNAME,
 * app_config.h) instead of a hardcoded/discovered IP.
 *
 * Routes (all under RADIO_COMPANION_HTTP_PORT, app_config.h):
 *   GET  /nowplaying - current track + station, JSON. Built from
 *        nowplaying_get_current() (nowplaying.h), station_list_get_now_
 *        playing() (station_list.h), and album_art_get_snapshot()'s version
 *        counter (album_art.h) for "art_version" (NOT the art bytes
 *        themselves - that is what GET /art is for). "tuning" is simply
 *        !info.valid, reusing the existing nowplaying_reset() valid=false->
 *        true transition (see main.c's radio_task - a genuine station
 *        change already calls nowplaying_reset()) rather than adding any
 *        new event plumbing:
 *          {"valid":true,"tuning":false,"title":"...","subtitle":"...",
 *           "album":"...","art_version":4,"art_w":140,"art_h":140,
 *           "station_index":1,"station_name":"...","age_ms":1234}
 *   GET  /stations - the station catalog, JSON:
 *          {"count":5,"current_index":1,
 *           "stations":[{"index":0,"id":"s345732","name":"Apple Music 1"},...]}
 *   POST /tune - body {"index":N}. Validates N is a real RADIO_STATIONS
 *        index (0 <= N < RADIO_STATION_COUNT), then hands off a station
 *        SELECT the same way an AVRCP/console next/prev already does (see
 *        below) - {"ok":true,"tuning":true} on success, HTTP 400 +
 *        {"ok":false,"error":"..."} otherwise.
 *   GET  /art - the current album-art thumbnail as raw RGB565 bytes
 *        (Content-Type: application/octet-stream, headers X-Art-Version/
 *        X-Art-W/X-Art-H - see album_art.h), or HTTP 204 with an empty body
 *        if nothing has been decoded yet.
 *
 * Command handoff (a THIRD, independent source of a station-change request,
 * alongside avrcp_uart.h's UART link and console_cli.h's serial console -
 * radio_pipeline_wait() (radio_pipeline.h) drains all three every loop
 * iteration): `POST /tune`'s handler queues the requested index and rings
 * the doorbell below, mirroring console_cli.c's dispatch_command() exactly.
 * Unlike NEXT/PREV (a direction, no extra data), a station SELECT carries a
 * specific target index, so the queue below stores {cmd, index} pairs
 * rather than a bare command byte - this pairs each SELECT with its own
 * index in strict FIFO order even if a second `/tune` request is queued
 * before radio_pipeline_wait() has drained the first one, which a single
 * separate "pending index" slot (like console_cli.h's CONSOLE_CMD_CAL_SET_MS
 * handoff) could not safely do here - that pattern relies on the REPL
 * blocking between commands, which an HTTP server serving one-shot requests
 * does not.
 */

/**
 * Starts the companion server: creates the command queue and doorbell event
 * interface immediately (synchronous, no WiFi/network dependency), then
 * arranges for mDNS + the HTTP server itself to come up the first time this
 * chip actually has an IP address.
 *
 * NOT literally "mdns_init() called right here, independent of WiFi" the way
 * nowplaying_init()/icy_meta_init()/avrcp_uart_start()/console_cli_start()
 * are - checked directly against this file's actual boot order (main.c's
 * app_main() calls this synchronously, BEFORE creating radio_task, and
 * radio_task's start_wifi() is what calls esp_netif_init()/
 * esp_event_loop_create_default()/esp_netif_create_default_wifi_sta() - none
 * of which exist yet at the point this function runs). Calling mdns_init()
 * or esp_event_handler_instance_register(IP_EVENT, ...) directly here would
 * either crash (mDNS's own internal netif/event-loop hookup with nothing
 * underneath it yet) or silently fail forever (a handler registration
 * attempted before the default event loop exists, which is a real,
 * non-transient ESP_ERR_INVALID_STATE - the loop already-exists check works
 * both ways). Also, main.c's start_wifi() calls
 * ESP_ERROR_CHECK(esp_event_loop_create_default()) UNCONDITIONALLY - this
 * file must never call esp_event_loop_create_default() itself, or whichever
 * of the two calls loses the race aborts the whole chip.
 *
 * So this instead spawns a small, self-deleting task that retries
 * esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ...)
 * every 200ms until it stops seeing ESP_ERR_INVALID_STATE (i.e. until
 * start_wifi() has created the default loop, a handful of milliseconds
 * after radio_task starts) - the SAME "multiple independent handlers can
 * coexist on this event" pattern main.c's own wifi_event_handler already
 * relies on. mdns_init()/hostname/instance/service_add and httpd_start() +
 * route registration all then happen together, once, inside that handler on
 * the FIRST real IP_EVENT_STA_GOT_IP - which is also strictly more correct
 * than announcing mDNS before this chip has an IP address to advertise at
 * all. A later WiFi drop/reconnect (another IP_EVENT_STA_GOT_IP) does NOT
 * redo this setup - both mDNS and the running httpd server are expected to
 * keep working across a reconnect on their own, same as every other
 * long-lived subsystem here.
 *
 * Non-fatal on any failure along this path - same "log a warning, never
 * crash the radio" convention every optional subsystem in this project
 * follows. Worst case, the companion API is simply never reachable; audio
 * playback is completely unaffected.
 */
esp_err_t companion_server_start(void);

/*
 * The doorbell event source - same usage pattern as
 * avrcp_uart_get_event_iface()/console_cli_get_event_iface(). Register it
 * with
 *   audio_event_iface_set_listener(companion_server_get_event_iface(), listener)
 * AFTER audio_pipeline_set_listener() (which rebuilds the listener's queue
 * set from scratch), and drop it again with
 * audio_event_iface_remove_listener() before destroying that listener.
 *
 * Returns NULL if companion_server_start() was never called or failed.
 */
audio_event_iface_handle_t companion_server_get_event_iface(void);

/*
 * Takes the next pending command - today this is ALWAYS AVRCP_CMD_SELECT or
 * AVRCP_CMD_NONE (nothing on this link ever produces NEXT/PREV), returned as
 * avrcp_cmd_t rather than a new type of its own so radio_pipeline.c can
 * drain this source with the exact same handling shape as avrcp_uart_take_
 * command()/console_cli_take_command()'s NEXT/PREV mapping. Pass 0 to just
 * drain what is already there (radio_pipeline_wait()'s own convention).
 *
 * On an AVRCP_CMD_SELECT return, call companion_server_take_pending_
 * station_index() immediately afterward (before taking any other command
 * from any source) to get the index this particular SELECT was for - see
 * this file's header comment on why the index travels paired with its own
 * queue entry rather than through a separate single-slot handoff.
 *
 * Returns AVRCP_CMD_NONE if companion_server_start() was never called or
 * failed.
 */
avrcp_cmd_t companion_server_take_command(TickType_t wait);

/*
 * The RADIO_STATIONS index that accompanied the MOST RECENTLY DEQUEUED
 * AVRCP_CMD_SELECT from companion_server_take_command() above - already
 * range-validated by the `/tune` handler before it was ever queued (an
 * out-of-range request gets an HTTP 400 and is never queued at all).
 * Meaningless if the last command taken was not AVRCP_CMD_SELECT.
 */
uint8_t companion_server_take_pending_station_index(void);

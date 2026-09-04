#pragma once

/*
 * playlist_prefetch - fetch and parse the NEXT copy of a live HLS media
 * playlist on a genuinely separate, short-lived HTTPS connection, while the
 * pipeline's own http_reader element is still busy delivering the LAST
 * known segment of the current window on its own, untouched connection.
 *
 * Why this exists (2026-08-22): reuse_conn_same_host (see http_stream.c/.h's
 * LOCAL PATCH) narrowed the live-window-boundary silence gap from ~10-15s to
 * ~1-2s by skipping a redundant same-host reconnect, but measured hardware
 * logs still showed ~3-3.2s - because the manifest (itsliveradio.apple.com)
 * and every media segment (liveradio.music.apple.com) are on DIFFERENT
 * hosts, so the manifest-refetch -> first-new-segment hop is a forced
 * cross-host reconnect no same-connection trick can avoid. A bigger ring
 * buffer can't hide it either: this stream runs ~32KB/s, so fully covering a
 * ~3s stall needs ~100KB of look-ahead, far past what this heap-constrained,
 * no-PSRAM chip can spare (see app_config.h's RADIO_HTTP_BUFFER_BYTES
 * history).
 *
 * This is the real fix instead: know the next window's segment URIs BEFORE
 * the current one runs out, so the transition never needs a synchronous
 * refetch at all. It costs a second, transient TLS session's worth of RAM
 * (~17-21KB, per this project's own measured mbedTLS+cert-verify numbers) -
 * but only for the ~0.5-2s this task is actually alive, once per ~150s live
 * window, not held for the life of the session the way a bigger static
 * buffer would be.
 *
 * Ownership/threading model: playlist_prefetch_start() spawns ONE short-
 * lived task per call (refuses to start a second while one is already in
 * flight - see playlist_prefetch_is_in_flight()). That task touches nothing
 * belonging to the audio pipeline directly; it only fetches+parses a copy of
 * the manifest and posts the resulting track URIs to a queue, then deletes
 * itself. The caller (radio_pipeline.c, on radio_task) is what hands the
 * result to the live http_stream element via http_stream_playlist_insert_
 * tracks() - see that function's own comment in http_stream.h for why that
 * hand-off is safe to do concurrently with the element's own task.
 */

#include <stdbool.h>
#include "esp_err.h"

/* Real hardware playlists observed here run 8-10 segments; double that for
 * headroom without inviting an unbounded/attacker-controlled parse. */
#define PLAYLIST_PREFETCH_MAX_TRACKS 16

typedef struct {
    char *uris[PLAYLIST_PREFETCH_MAX_TRACKS];
    int   count;
} playlist_prefetch_result_t;

/**
 * One-time setup: preallocates the fixed read buffer and result queue while
 * the heap is still relatively clean (same "claim it up front, not lazily"
 * reasoning as fmp4_bridge_init() - see that file's comment). Idempotent -
 * safe to call again (e.g. if ever called per-session instead of once at
 * boot); a second call is a no-op returning ESP_OK.
 *
 * @param crt_bundle_attach  Passed straight to the prefetch task's own
 *                           esp_http_client_config_t, same as the pipeline's
 *                           http_reader uses - see radio_pipeline.c.
 */
esp_err_t playlist_prefetch_init(esp_err_t (*crt_bundle_attach)(void *conf));

/**
 * Start fetching+parsing playlist_url on a new short-lived task, if one
 * isn't already in flight.
 *
 * @return ESP_OK if a task was started; ESP_ERR_INVALID_STATE if one was
 *         already running or playlist_prefetch_init() was never called;
 *         ESP_ERR_NO_MEM on allocation failure.
 */
esp_err_t playlist_prefetch_start(const char *playlist_url);

/** True while a prefetch task is currently running. */
bool playlist_prefetch_is_in_flight(void);

/**
 * Non-blocking: returns true and sets *out_result if a completed prefetch's
 * result is waiting. Caller takes ownership and must eventually call
 * playlist_prefetch_result_free() on it. A result with count == 0 means the
 * fetch/parse failed or found nothing new - the caller should just fall back
 * to the existing reactive FINISH_PLAYLIST path for that boundary.
 */
bool playlist_prefetch_poll(playlist_prefetch_result_t **out_result);

/** Frees a result obtained from playlist_prefetch_poll(). */
void playlist_prefetch_result_free(playlist_prefetch_result_t *result);

/**
 * Discards any completed-but-unpolled result sitting in the queue. Call from
 * radio_pipeline_stop() between sessions so a result fetched for the OLD
 * session's playlist URL can never be mistakenly handed to a NEW session's
 * http_reader after a restart. Does not (cannot, safely) cancel a task that
 * is still actually in flight - see the .c file's comment on
 * playlist_prefetch_reset() for why that's fine to leave running.
 */
void playlist_prefetch_reset(void);

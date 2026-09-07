#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * album_art - downloads and decodes the CURRENTLY PLAYING track's JPEG
 * artwork (nowplaying_get_current()'s art_url) into a fixed-size RGB565
 * thumbnail, for the DP666 companion display's `GET /art` (companion_server.h)
 * to serve. This exists entirely because of where the two chips' resources
 * sit: the DP666 (esp32_dp666_companion, classic ESP32, no PSRAM) has
 * neither the RAM for a JPEG decoder's working set nor a JPEG library at
 * all, while THIS chip has 8MB of PSRAM sitting mostly idle relative to
 * what audio playback actually uses - so decode-and-resize happens here,
 * once, and the DP666 just blits whatever raw pixel bytes it's handed. See
 * the plan's B2 section for the full reasoning.
 *
 * Decoder: espressif/esp_jpeg (managed component, added via
 * main/idf_component.yml - this S3 has no hardware JPEG block, only the P4
 * does, so this is a plain software decode). Resize: nearest-neighbor, done
 * by hand below - esp_jpeg_decode() itself only offers coarse power-of-two
 * scale factors (0, 1/2, 1/4, 1/8), not an arbitrary target size, so
 * fetch_and_decode() below picks whichever of those scales keeps the
 * decoded image at least as large as the target box in both dimensions
 * (falling back to no scale if the source is already smaller), then resizes
 * that down to the exact RADIO_COMPANION_ART_W x RADIO_COMPANION_ART_H box
 * itself. Good enough for a small on-screen thumbnail; not attempting a
 * higher-quality box filter for a first cut (see this file's own comment
 * above resize_nearest_rgb565()).
 *
 * Own dedicated task (album_art_task, RADIO_COMPANION_ART_TASK_STACK/
 * _PRIORITY in app_config.h - deliberately BELOW radio_task's own priority
 * so a JPEG decode can never preempt audio playback), polling
 * nowplaying_get_current() every RADIO_COMPANION_ART_POLL_INTERVAL_MS and
 * only doing any network/decode work at all when the art_url has actually
 * changed since the last attempt - most polls are a no-op string compare.
 *
 * Buffers: everything here is PSRAM-backed (the psram_alloc() pattern this
 * file copies from nowplaying.c/icy_meta.c - see those files' own comments
 * for why each module keeps its own local copy rather than sharing one) -
 * the bounded JPEG download scratch (RADIO_COMPANION_ART_MAX_JPEG_BYTES),
 * the transient full-resolution decode buffer, and the double-buffered
 * RGB565 output (front/back, ART_W*ART_H*2 bytes each, allocated once at
 * album_art_start() and never freed - this module lives for the process's
 * whole lifetime, same as nowplaying.c). A mutex protects the front buffer
 * + its version counter, the only state a reader (companion_server.c's
 * `/art` handler) ever touches.
 */

/**
 * One-time setup: creates the front/back PSRAM art buffers and the result
 * mutex, then starts album_art_task. Call once at boot, independent of
 * WiFi (same "ready before it's needed" convention as nowplaying_init()/
 * icy_meta_init()/avrcp_uart_start() in main.c's app_main()) - the task
 * itself just no-ops (nowplaying stays !valid, or esp_http_client's GET
 * fails) until WiFi actually connects, which is harmless.
 *
 * Logs and returns an error rather than crashing on PSRAM exhaustion - same
 * "log a non-fatal warning, never take down the radio" convention every
 * optional subsystem in this project follows. On failure, `/art` will keep
 * returning 204 (companion_server.c checks album_art_get_snapshot()'s
 * return value, not whether this succeeded) - a missing album-art thumbnail
 * is a cosmetic-only DP666 issue, never a reason to affect playback.
 */
esp_err_t album_art_start(void);

/**
 * Takes the shared art-buffer mutex and, if a track's artwork has ever been
 * successfully downloaded and decoded (for the CURRENT station - a genuine
 * station change does not proactively clear this the way nowplaying_reset()
 * clears title/artist, but a new station's own first successfully-decoded
 * track naturally replaces it via the normal art_url-changed path above),
 * fills *out_buf, *out_len, and *out_version with the current front-buffer
 * snapshot (raw RGB565, exactly RADIO_COMPANION_ART_W * RADIO_COMPANION_ART_H
 * * 2 bytes) and returns true.
 *
 * Returns false, WITHOUT taking the mutex, if nothing has ever been decoded
 * yet (album_art_start() never called/failed, or no track with artwork has
 * played since boot) - album_art_release_snapshot() must NOT be called in
 * that case, since there is nothing to release.
 *
 * On a true return, the caller MUST call album_art_release_snapshot() when
 * finished reading *out_buf (e.g. immediately after httpd_resp_send()
 * finishes writing it out in companion_server.c's `/art` handler) before
 * doing anything else that could block. The mutex is deliberately held
 * across the whole read/send rather than requiring the caller to copy
 * ART_W*ART_H*2 bytes first: at ~1 DP666 poll per 1-2s there is no
 * meaningful contention with album_art_task's own decode-and-swap cycle
 * (RADIO_COMPANION_ART_POLL_INTERVAL_MS), so this is simpler and cheaper
 * than a private copy per request.
 *
 * `*out_version` bumps by one every time a new decode successfully lands in
 * the front buffer (starts at 0, meaning "never decoded" - a version of 0 is
 * never handed back with a true return). companion_server.c's `/nowplaying`
 * handler also calls this (passing NULL for out_buf/out_len - both optional)
 * just to publish "art_version" - it still must call
 * album_art_release_snapshot() right away on a true return, same as any
 * other caller, rather than holding the mutex across the rest of the JSON
 * response it has nothing to do with.
 */
bool album_art_get_snapshot(const uint8_t **out_buf, size_t *out_len, uint32_t *out_version);

/**
 * Releases the mutex taken by a `true`-returning album_art_get_snapshot()
 * call. Exactly one call per `true` return, never after a `false` one.
 */
void album_art_release_snapshot(void);

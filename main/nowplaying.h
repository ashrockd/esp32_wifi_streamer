#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * nowplaying - background poller for the CURRENTLY PLAYING track's title/
 * artist/album/artwork URL, ported from testing-tools/python-script-albumart-
 * title/tunein_nowplaying.py's HLS-embedded-ID3 technique. See that script's
 * own docstring for the full reverse-engineering story; the short version:
 * TuneIn's own REST/GraphQL now-playing endpoints only ever return the
 * STATION's name/tagline for these Apple Music-curated stations, never the
 * actual track - the real per-track title/artist/album/artwork instead rides
 * along AS ID3v2 tags embedded directly in the HLS audio stream itself
 * (the same "timed metadata" mechanism hls.js/AVPlayer surface client-side),
 * re-embedded fresh near the start of every ~16s segment.
 *
 * PORTING NOTE (2026-09-05) - this needed one real fix, not just a language
 * change: the python script's parse_id3_tags() treats ANY occurrence of the
 * literal bytes "ID3" anywhere in the buffer as a tag header. That is safe
 * for a plain HLS/ADTS segment (which is genuinely all the python script had
 * been tested against - see its docstring's step 3), but this project's
 * actual stream is CMAF/fMP4 (see fmp4_bridge.h), and a REAL captured segment
 * (tools/tunin test/seg.mp4) proved that assumption wrong here: the fMP4
 * 'emsg' box wrapping each ID3 tag carries a scheme_id_uri that itself
 * contains the literal substring "ID3" (something like ".../emsg/ID3"),
 * BEFORE the real tag. The python parser's naive scan finds that first,
 * misreads the following bytes as a tag header, computes a garbage
 * synchsafe size, and gives up scanning the rest of the segment entirely -
 * verified directly: running the unmodified python parser against seg.mp4
 * finds zero tags. This C port instead validates every "ID3" match against
 * the actual ID3v2 header shape (major version in the defined 2-4 range,
 * reserved flag bits zero, synchsafe size bytes all < 0x80) before trusting
 * it, and keeps scanning past a false match instead of aborting - confirmed
 * against the same seg.mp4 fixture to correctly recover all 8 repeated
 * copies of the real tag (title/artist/album/artwork all present).
 *
 * Threading model, deliberately copied from playlist_prefetch.h (same
 * problem shape - a slow, blocking HTTPS fetch that must never run on
 * anyone else's task): nowplaying_poll_start() spawns ONE short-lived task
 * per call (refusing to start a second while one is already in flight - see
 * nowplaying_is_in_flight()) that does two sequential HTTP fetches (the
 * current HLS media playlist, then a Range-limited prefix of its last
 * segment), parses whatever ID3 tag it finds, and updates the shared result
 * under a mutex before deleting itself. radio_pipeline.c drives when a poll
 * starts (it already owns the playlist URL's lifecycle - see its own
 * service_nowplaying_poll()); main.c's periodic status log just reads
 * whatever the last completed poll produced via nowplaying_get_current(),
 * which never blocks.
 */

/* Real titles/artists/albums observed on this stream run well under 64
 * bytes; doubled for headroom. Artwork URLs (mzstatic.com CDN paths) run
 * ~150-200 bytes observed - 320 leaves comfortable margin without inviting
 * an unbounded copy. */
#define NOWPLAYING_TITLE_MAX     128
#define NOWPLAYING_SUBTITLE_MAX  128
#define NOWPLAYING_ALBUM_MAX     128
#define NOWPLAYING_ART_URL_MAX   320

typedef struct {
    /* False until the FIRST track has ever been successfully parsed for the
     * CURRENT station (nowplaying_reset() puts this back to false on a
     * station change - see its own comment) - every string field is empty
     * and meaningless while this is false. */
    bool     valid;
    char     title[NOWPLAYING_TITLE_MAX];
    /* Artist, falling back to album when the tag carries no TPE1 - same
     * precedence tunein_nowplaying.py's display_fields() uses for
     * playerSubtitle. */
    char     subtitle[NOWPLAYING_SUBTITLE_MAX];
    char     album[NOWPLAYING_ALBUM_MAX];
    /* Empty string if the tag carried no WXXX artwork frame at all - still
     * a perfectly normal, valid track (some stations/segments just don't
     * carry artwork), not an error. */
    char     art_url[NOWPLAYING_ART_URL_MAX];
    /* How long ago (ms) this snapshot was last refreshed by a successful
     * poll, filled in by nowplaying_get_current() from its own clock - NOT
     * stored alongside the fields above. Lets a caller tell "fresh" from
     * "stale because the last few polls failed" without a separate call. */
    uint32_t age_ms;
} nowplaying_info_t;

/**
 * One-time setup: creates the result mutex. Idempotent - a second call is a
 * no-op returning ESP_OK. Safe to call before Wi-Fi is even up; nothing here
 * touches the network.
 *
 * @param crt_bundle_attach  Passed straight to both of the poll task's own
 *                            esp_http_client_config_t instances (playlist
 *                            fetch, then segment fetch) - same bundle every
 *                            other HTTPS request in this project uses.
 */
esp_err_t nowplaying_init(esp_err_t (*crt_bundle_attach)(void *conf));

/**
 * Starts a background fetch+parse of the CURRENT track playing on the HLS
 * stream `hls_media_playlist_url` resolves to, if one is not already in
 * flight. `hls_media_playlist_url` is copied (the caller's own copy - e.g.
 * radio_pipeline.c's cached_playlist_url - may change or be freed the
 * moment this returns; ownership is never shared).
 *
 * @return ESP_OK if a task was started; ESP_ERR_INVALID_STATE if one was
 *         already running or nowplaying_init() was never called;
 *         ESP_ERR_NO_MEM on allocation failure.
 */
esp_err_t nowplaying_poll_start(const char *hls_media_playlist_url);

/** True while a poll task is currently running. */
bool nowplaying_is_in_flight(void);

/**
 * Non-blocking: always fills *out with whatever the last completed poll
 * produced (out->valid stays false if none has ever succeeded yet for the
 * current station - see nowplaying_reset()). Safe to call from any task;
 * internally mutex-protected against the poll task updating the same state
 * concurrently.
 */
void nowplaying_get_current(nowplaying_info_t *out);

/**
 * Clears the cached track back to "nothing known yet" WITHOUT touching
 * whether a poll is in flight. Call this on any genuine station change (see
 * main.c's radio_task) so a track from the PREVIOUS station can never be
 * misread as still describing the new one for the ~20-30s until the next
 * poll completes - deliberately NOT called on every routine same-station
 * session refresh (a live-window boundary, a proactive max-session
 * refresh), where the last known track is still perfectly valid and
 * clearing it would just flicker the status log for no reason.
 */
void nowplaying_reset(void);

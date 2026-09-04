#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * nowplaying - the CURRENTLY PLAYING track's title/artist/album/artwork URL,
 * ported from testing-tools/python-script-albumart-title/tunein_nowplaying.py's
 * HLS-embedded-ID3 technique. See that script's own docstring for the full
 * reverse-engineering story; the short version: TuneIn's own REST/GraphQL
 * now-playing endpoints only ever return the STATION's name/tagline for
 * these Apple Music-curated stations, never the actual track - the real
 * per-track title/artist/album/artwork instead rides along AS an ID3v2 tag
 * embedded directly in the HLS audio stream itself (the same "timed
 * metadata" mechanism hls.js/AVPlayer surface client-side).
 *
 * SNIFFED, NOT FETCHED (2026-09-05 redesign) - this module does NOT open any
 * HTTP/TLS connection of its own. An earlier version did (its own playlist
 * GET + a Range-limited segment GET, on a separate short-lived task/TLS
 * session, mirroring playlist_prefetch.c) - which worked, but was real
 * waste: a THIRD independent TLS session on a project whose whole resource-
 * pressure history (see RADIO_DMA_FREE_CRITICAL_BYTES/RADIO_HTTP_BUFFER_
 * BYTES in app_config.h) is about exactly this - concurrent TLS connections
 * competing for the same tight DMA-capable/internal RAM pool, on top of a
 * completely redundant fetch of bytes the pipeline is already downloading
 * for playback. The ID3 tag lives inside an 'emsg' box in the SAME CMAF
 * segment stream fmp4_bridge.c already parses byte-by-byte to find moof/
 * mdat - so fmp4_bridge.c now also recognizes 'emsg' boxes, buffers them
 * (reusing its existing moof buffer - see its own FMP4_MAX_EMSG_BYTES
 * comment), and calls nowplaying_ingest_id3_tag() below directly, on its own
 * task, with zero extra network I/O. This module is now pure parsing plus a
 * small mutex-protected result cache; main.c's periodic status log just
 * reads whatever the last ingested tag produced via nowplaying_get_current().
 *
 * PORTING NOTE - the parsing itself needed one real fix, not just a
 * language change: the python script's parse_id3_tags() treats ANY
 * occurrence of the literal bytes "ID3" anywhere in the buffer as a tag
 * header. That is safe for a plain HLS/ADTS segment (which is genuinely all
 * the python script had been tested against - see its docstring's step 3),
 * but a REAL captured segment from this project's actual CMAF stream (tools/
 * tunin test/seg.mp4) proved that assumption wrong here: the emsg box's own
 * scheme_id_uri field contains the literal substring "ID3" (something like
 * ".../emsg/ID3"), BEFORE the real tag that follows it in the same box. The
 * python parser's naive scan finds that first, misreads the following bytes
 * as a tag header, computes a garbage synchsafe size, and gives up scanning
 * the rest of the buffer entirely - verified directly: running the
 * unmodified python parser against seg.mp4 finds zero tags. This C port
 * instead validates every "ID3" match against the actual ID3v2 header shape
 * (major version in the defined 2-4 range, reserved flag bits zero,
 * synchsafe size bytes all < 0x80) before trusting it, and keeps scanning
 * past a false match instead of aborting - confirmed against the same
 * seg.mp4 fixture to correctly recover all 8 repeated copies of the real
 * tag (title/artist/album/artwork all present). Handing the WHOLE buffered
 * emsg box body (scheme_id_uri text included) to this same validated
 * scanner, rather than separately parsing emsg's own header fields to find
 * where message_data starts, is what lets fmp4_bridge.c stay this simple -
 * the false-positive-tolerant scan already has to handle exactly that text
 * being present ahead of the real tag.
 *
 * Threading model: nowplaying_ingest_id3_tag() is called ONLY from
 * fmp4_bridge.c's own element task (never concurrently with itself), so its
 * internal parsing scratch state is not stack-allocated - deliberately, to
 * keep this cheap on fmp4_bridge's dedicated small task stack
 * (FMP4_BRIDGE_TASK_STACK) the same way parse_moof() already runs inline on
 * that same task at a similar per-fragment frequency. It is also NOT plain
 * `static` (internal RAM): this board has 8MB of PSRAM, so nowplaying.c
 * allocates its result + scratch buffers from PSRAM instead, once, up
 * front in nowplaying_init() - see that file's own comment. The shared
 * result (nowplaying_info_t) IS mutex-protected, because reading it (main.c's
 * periodic status log) happens from a DIFFERENT task.
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
    /* How long ago (ms) this snapshot was last refreshed by a successfully
     * ingested tag, filled in by nowplaying_get_current() from its own
     * clock - NOT stored alongside the fields above. Lets a caller tell
     * "fresh" from "stale because no new tag has come in for a while"
     * without a separate call. */
    uint32_t age_ms;
} nowplaying_info_t;

/**
 * One-time setup: creates the result mutex. Idempotent - a second call is a
 * no-op returning ESP_OK. Nothing here touches the network (there is none
 * to touch any more - see this file's header comment).
 */
esp_err_t nowplaying_init(void);

/**
 * Parses `data` (the fully-buffered body of one 'emsg' box - see
 * fmp4_bridge.c's FMP4_MAX_EMSG_BYTES comment for exactly what calls this
 * and with what) for an ID3v2 tag, and updates the shared "current track"
 * result if one is found. A safe, cheap no-op if none is found (most emsg
 * boxes on this stream are lightweight timing pings with no title/artist -
 * see nowplaying.h's own PORTING NOTE) - the previous result is left
 * untouched rather than cleared, so a station's last known track survives
 * however many pings arrive between real metadata tags.
 *
 * ONLY ever called from fmp4_bridge.c's own element task - see this file's
 * header comment on why that lets the parsing scratch state below be plain
 * static instead of needing its own lock or stack allocation.
 */
void nowplaying_ingest_id3_tag(const uint8_t *data, size_t len);

/**
 * Non-blocking: always fills *out with whatever the last ingested tag
 * produced (out->valid stays false if none has ever succeeded yet for the
 * current station - see nowplaying_reset()). Safe to call from any task;
 * internally mutex-protected against nowplaying_ingest_id3_tag() updating
 * the same state concurrently.
 */
void nowplaying_get_current(nowplaying_info_t *out);

/**
 * Clears the cached track back to "nothing known yet" WITHOUT touching
 * anything else. Call this on any genuine station change (see main.c's
 * radio_task) so a track from the PREVIOUS station can never be misread as
 * still describing the new one for however long it takes the new station's
 * first metadata-carrying tag to arrive - deliberately NOT called on every
 * routine same-station session refresh (a live-window boundary, a
 * proactive max-session refresh), where the last known track is still
 * perfectly valid and clearing it would just flicker the status log for no
 * reason.
 */
void nowplaying_reset(void);

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/*
 * art_fallback - resolves album artwork BY ARTIST/TITLE, for tracks whose
 * station doesn't embed its own artwork in stream metadata (nowplaying.h's
 * nowplaying_info_t.art_url left as an empty string by
 * nowplaying_ingest_icy_title()/_id3_tag() when the tag/metadata carried
 * none). Ported from testing-tools/python-script-albumart-title/
 * art_lookup_fallback.py's PoC - see that script's own docstring for the
 * full reverse-engineering story and its real test case: "Vibes of Vegas"
 * (RADIO_VIBE_OF_VEGAS_STATION_ID, app_config.h) reports title/artist over
 * plain ICY StreamTitle metadata but never an artwork URL at all.
 *
 * Three free, keyless, plain-HTTPS-GET lookup services, tried in order,
 * stopping at the first hit (see art_fallback.c for the per-tier detail):
 *
 *   1. iTunes Search API (itunes.apple.com/search) - explicitly requests
 *      RADIO_COMPANION_ART_W x RADIO_COMPANION_ART_H (240x240) artwork
 *      directly in the URL, which is ALSO the one source that can hand
 *      album_art.c's fetch_and_decode() a JPEG at EXACTLY the target box
 *      size - see that function's own comment on skipping resize/box-filter
 *      postprocessing entirely when it is. The PoC's real-station testing
 *      never fell through past this tier.
 *   2. MusicBrainz (recording search) + Cover Art Archive - tried BEFORE
 *      Deezer (order swapped from the PoC's own default order), because
 *      real testing from the dev machine's network found Deezer's public
 *      search API returns "data": [] despite a correct nonzero "total"
 *      count for EVERY query tried (a content-licensing restriction tied to
 *      the requester's apparent region, confirmed via raw response headers -
 *      not a bug in the query shape) - see the PoC's own docstring for the
 *      exact evidence. This tier needs its own Cover Art Archive existence
 *      check per candidate release (art_fallback.c's musicbrainz_lookup())
 *      since MusicBrainz ranks same-titled live/bootleg recordings at an
 *      IDENTICAL match score to the studio original, and a bootleg release
 *      essentially never has CAA art - confirmed directly against a real
 *      query (Radiohead / "Paranoid Android") during PoC testing.
 *   3. Deezer (api.deezer.com/search) - last, specifically BECAUSE of the
 *      finding above: if that restriction turns out to be Deezer-side
 *      rather than tied to the specific dev machine/network that found it,
 *      this tier silently no-ops every time, which is exactly why it is no
 *      longer trusted as tier 1 or 2. Left in rather than removed, in case
 *      it works from this chip's own Wi-Fi egress (a different ISP path
 *      than the dev machine that found the restriction) - costs nothing
 *      extra to keep as a last try. Only fixed cover sizes are available
 *      (56/120/250/500/1000px) - none is exactly 240 - so a Deezer-resolved
 *      hit always goes through fetch_and_decode()'s normal resize path,
 *      never the exact-240 fast path above.
 *
 * All three are parsed with cJSON (matches companion_server.c's own
 * convention for this project's JSON, not tunein_control.c's older
 * hand-rolled marker-extraction style - that file predates cJSON's use here
 * and parses TuneIn's much larger/quirkier responses, a different tradeoff)
 * - see art_fallback.c for why this matters especially for MusicBrainz's
 * genuinely nested recording->releases->release structure, which a
 * marker/substring scan cannot reliably walk on its own.
 *
 * Threading/caller model: ONLY ever called from album_art_task
 * (album_art.c), which already dedupes repeat calls by (title, subtitle)
 * before ever reaching this - see that file's own comment. One call here
 * may block for the duration of several sequential HTTPS round trips (one
 * per tier tried, plus up to ART_FALLBACK_MB_MAX_CAA_CHECKS Cover Art
 * Archive existence checks on the MusicBrainz tier) - acceptable on
 * album_art_task's own dedicated, below-radio_task-priority task
 * (RADIO_COMPANION_ART_TASK_PRIORITY in app_config.h), the same way
 * fetch_and_decode()'s own JPEG download already blocks that task; and this
 * is a last-resort path taken only when nowplaying_info_t.art_url is empty
 * in the first place - real testing found the primary iTunes tier resolves
 * essentially every real station track on its own, so this rarely runs at
 * all, let alone reaches tier 2/3.
 */

/* Bounds out_url the same way NOWPLAYING_ART_URL_MAX bounds nowplaying_info_t
 * .art_url (nowplaying.h) - a resolved URL from here is handed straight into
 * that same field's-worth of buffer by album_art_task. */
#define ART_FALLBACK_URL_MAX   320

/**
 * One-time setup: allocates the shared PSRAM scratch buffer every tier's
 * HTTP response is downloaded into (see art_fallback.c's
 * ART_FALLBACK_JSON_BUF_BYTES) - same "allocate once, up front" convention
 * as album_art_start()/nowplaying_init(). Call once at boot, from
 * album_art_start() specifically (this module has no other caller - see
 * this file's header comment), before album_art_task can ever run.
 *
 * Logs and returns an error rather than crashing on PSRAM exhaustion, same
 * as every optional subsystem in this project: on failure,
 * art_fallback_resolve() always returns false without attempting any
 * network I/O, so a station like "Vibes of Vegas" just keeps showing no
 * artwork on the DP666 rather than affecting anything else.
 */
esp_err_t art_fallback_init(void);

/**
 * Tries iTunes -> MusicBrainz+CoverArtArchive -> Deezer, in that order (see
 * this file's header comment for why that order), and copies the first hit
 * into out_url. `title`/`artist` are NUL-terminated, human-readable text -
 * already split by nowplaying_ingest_icy_title()'s delimiter fallback or
 * lifted straight from an ID3 TIT2/TPE1 frame. An empty `artist` is passed
 * through as-is (some stations' ICY StreamTitle carries no recognizable
 * delimiter at all - see nowplaying.h's own comment on that) - every tier
 * below degrades to a title-only query in that case rather than failing
 * outright.
 *
 * Returns true and fills out_url on a hit, false (leaving out_url
 * untouched) if `title` is empty, art_fallback_init() was never called (or
 * its PSRAM allocation failed), or every tier misses/fails - all ordinary,
 * expected outcomes (an obscure or misspelled station-reported title, a
 * transient network blip on one tier), logged no higher than ESP_LOGI/W,
 * never treated as a hard error by the caller.
 */
bool art_fallback_resolve(const char *title, const char *artist,
                           char *out_url, size_t out_url_size);

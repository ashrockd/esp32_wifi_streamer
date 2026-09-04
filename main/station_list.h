#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * The station catalog + persisted/loopable selection.
 *
 * Source of truth is ../../tunein_music_stations.csv: of its 6 "Apple"-
 * branded entries, only these 4 are actually streamable (stream type HLS,
 * "AAC/CMAF audio via HLS playlist (Apple Music stream)", 64kbps) - the
 * other 2 (Apple Musica Uno / s345727, Apple Music Club / s345726) are
 * marked N/A: TuneIn returns a notcompatible.mp3 placeholder for those
 * (licensing/region restricted), so they are deliberately excluded here.
 * All 4 are confirmed CMAF/AAC-LC over HLS, i.e. they all take this
 * project's one existing pipeline path (http_stream -> fmp4_bridge ->
 * aac_dec_element -> i2s_stream, see radio_pipeline.c) unchanged - nothing
 * about switching stations changes the element chain.
 *
 * The 5th entry, The Vibe of Vegas (s140762), was added on request and is
 * NOT from that CSV - its stream format has not been verified. This
 * pipeline can only play HLS whose segments are CMAF/fMP4 carrying AAC-LC:
 * tunein_control.c requires an .m3u8 from Tune.ashx, then an EXT-X-MAP init
 * segment to prime fmp4_bridge with. If this station turns out to serve
 * direct MP3, or classic MPEG-TS-segmented HLS, tunein_start_session() will
 * fail cleanly (ESP_ERR_NOT_FOUND) rather than play noise - main.c logs the
 * failure and applies its normal retry backoff, and a next/previous press
 * moves off it - but it will never play. Check the serial log for
 * "Tune.ashx"/"master playlist" errors the first time it is selected.
 */
typedef struct {
    const char *id;    /* TuneIn guideId, e.g. "s345732" */
    const char *name;  /* Display name, for logging only */
} radio_station_t;

/* A small, fixed table - `static const` so it can live directly in this
 * header without needing a .c translation unit of its own; every file that
 * includes this gets its own tiny private copy, which is fine at this size. */
static const radio_station_t RADIO_STATIONS[] = {
    { "s345732", "Apple Music 1" },
    { "s345724", "Apple Music Hits" },
    { "s345725", "Apple Music Country" },
    { "s345733", "Apple Music Chill" },
    { "s140762", "The Vibe of Vegas" }, /* unverified format - see above */
};
#define RADIO_STATION_COUNT (sizeof(RADIO_STATIONS) / sizeof(RADIO_STATIONS[0]))

/*
 * Loads the last-persisted station index from NVS (namespace "radio", key
 * "station_idx"). On first boot after this feature was added (NVS key not
 * found yet) or if a stored value is somehow out of range, falls back to
 * whichever RADIO_STATIONS entry matches RADIO_TUNEIN_STATION_ID (keeps
 * pre-multi-station behavior unchanged on upgrade) WITHOUT writing to NVS -
 * only an actual station change (station_list_next/prev below) persists
 * anything, so a read-only boot never wears NVS.
 */
uint8_t station_list_load_index(void);

/* Advances to the next/previous station, wrapping at both ends (modulo
 * RADIO_STATION_COUNT), persists the new index to NVS, and returns it. */
uint8_t station_list_next(uint8_t current);
uint8_t station_list_prev(uint8_t current);

/*
 * Live "what's currently selected" mirror - NOT the persisted NVS value
 * (station_list_load_index() above already covers reading that back on
 * boot). main.c's radio_task calls the setter on every station (re)
 * selection so other subsystems can report what is actually playing without
 * radio_task exposing anything else about itself - console_cli.h's `status`
 * command is the reason this exists.
 *
 * A single byte, written on one task and read on another: no lock needed,
 * same reasoning as led_viz.c's s_peak (one naturally-aligned word, atomic
 * load/store on this architecture; worst case a reader sees the previous
 * value for one loop iteration).
 */
void station_list_set_now_playing(uint8_t idx);
uint8_t station_list_get_now_playing(void);

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Real init segments observed on this stream are ~600 bytes (ftyp+moov for a
 * single AAC-LC audio track); this leaves generous headroom. */
#define TUNEIN_INIT_SEGMENT_MAX_BYTES 4096

/*
 * What the resolved stream actually turns out to be, detected in
 * tunein_start_session(); radio_pipeline.c branches its element chain on it.
 *
 * TuneIn/Apple are not obliged to keep serving the one shape this project
 * originally hardcoded, and station_list.h now holds stations that were
 * never format-verified at all. Rather than treating "not CMAF/AAC-LC" as a
 * failure, everything else is handed to ESP-ADF's own auto-detecting
 * esp_decoder, which covers every codec ADF ships a decoder for: MP3, AAC
 * (ADTS), M4A/MP4, MPEG-TS AAC, OGG, Opus, FLAC, WAV, AMR-NB/WB and PCM.
 * So the only remaining "unsupported" case is a URL this code cannot even
 * find in the Tune.ashx response.
 */
typedef enum {
    TUNEIN_FORMAT_UNKNOWN = 0,
    /* HLS whose media playlist carries an EXT-X-MAP init segment, i.e.
     * CMAF/fMP4-wrapped AAC-LC. The original and still the primary path:
     * http_stream -> fmp4_bridge -> aac_dec_element -> i2s_stream, with the
     * decoder TOLD its format from the init segment rather than sniffing.
     * All 4 Apple Music stations in station_list.h are this shape. */
    TUNEIN_FORMAT_HLS_CMAF_AAC,
    /* HLS with no EXT-X-MAP: the segments are self-describing (bare ADTS
     * AAC, or MPEG-TS), so there is nothing to prime a bridge with. The
     * segments go straight from http_stream into esp_decoder, which sniffs
     * the codec itself. */
    TUNEIN_FORMAT_HLS_GENERIC,
    /* Not HLS at all - Tune.ashx points straight at one continuous media
     * stream (.mp3, .aac, .m4a/.mp4, .ogg, .opus, .flac, .wav). Same
     * esp_decoder, but with http_stream's playlist parser off. */
    TUNEIN_FORMAT_DIRECT_GENERIC,
} tunein_stream_format_t;

typedef struct {
    char serial[32];
    char item_token[256];
    char listen_id[24];
    char station_id[16];        /* which station this session was resolved for */
    char hls_master_url[1536];

    tunein_stream_format_t format;

    /* The one URL actually handed to ADF's http_stream. For the HLS formats
     * it is the chosen variant's own media playlist, resolved from the
     * master by us rather than by http_stream's built-in ~200kbps-closest
     * heuristic - that guarantees it matches what fmp4_bridge was primed
     * for, and it stays valid to re-fetch as a live playlist until its
     * signed accessKey expires. For TUNEIN_FORMAT_DIRECT_GENERIC it is the
     * direct stream URL from Tune.ashx, with no playlist involved. */
    char hls_variant_url[1536];

    /* The CMAF init segment (ftyp+moov) for hls_variant_url, fetched once so
     * fmp4_bridge can be primed with its AudioSpecificConfig before the
     * pipeline starts consuming media segments. Populated only for
     * TUNEIN_FORMAT_HLS_CMAF_AAC; init_segment_len stays 0 otherwise. */
    uint8_t init_segment[TUNEIN_INIT_SEGMENT_MAX_BYTES];
    size_t  init_segment_len;

    /* esp_timer_get_time() (us) when this session's signed URLs were
     * resolved, so callers can proactively refresh well before any observed
     * expiry rather than relying solely on reacting to HTTP failures. */
    int64_t resolved_at_us;

    /* True only for the direct-stream bypass (currently just The Vibe of
     * Vegas - see RADIO_VIBE_OF_VEGAS_* in app_config.h and this function's
     * own top-of-body special case). Tells radio_pipeline_start() to request
     * ICY inline metadata (Icy-MetaData: 1) on this session's http_stream
     * connection and install icy_meta.h's stripping tap between http_reader
     * and the decoder - see icy_meta.h for why this is a separate mechanism
     * from the ID3-in-CMAF technique the TuneIn/Apple Music stations use.
     * False (the zero value from tunein_start_session()'s own memset) for
     * every TuneIn-resolved session. */
    bool icy_metadata;
} tunein_session_t;

/*
 * Runs TuneIn's non-browser control-plane flow for one station:
 * profile contents -> itemToken -> Tune.ashx -> HLS master playlist
 *   -> AAC-LC variant playlist -> CMAF init segment.
 *
 * station_id is a TuneIn guideId ("s345724") - one of the ids in
 * station_list.h, chosen by the caller (main.c) from the NVS-persisted
 * selection that AVRCP next/previous moves through. Pass NULL to use the
 * compile-time RADIO_TUNEIN_STATION_ID default.
 */
esp_err_t tunein_start_session(tunein_session_t *session, const char *station_id);

/* Logs a redacted URL. Pass false for tokens/signed query strings. */
void tunein_log_url(const char *label, const char *url, bool include_query);

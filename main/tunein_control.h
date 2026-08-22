#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Real init segments observed on this stream are ~600 bytes (ftyp+moov for a
 * single AAC-LC audio track); this leaves generous headroom. */
#define TUNEIN_INIT_SEGMENT_MAX_BYTES 4096

typedef struct {
    char serial[32];
    char item_token[256];
    char listen_id[24];
    char hls_master_url[1536];

    /* Resolved from the master playlist: the AAC-LC ("mp4a.40.2") variant's
     * own media playlist. This is what gets handed to ADF's http_stream -
     * resolving it ourselves (rather than letting http_stream's built-in
     * ~200kbps-closest heuristic pick a variant) guarantees it matches what
     * fmp4_bridge was primed for, and it stays valid to re-fetch as a live
     * playlist until its signed accessKey expires. */
    char hls_variant_url[1536];

    /* The CMAF init segment (ftyp+moov) for hls_variant_url, fetched once so
     * fmp4_bridge can be primed with its AudioSpecificConfig before the
     * pipeline starts consuming media segments. */
    uint8_t init_segment[TUNEIN_INIT_SEGMENT_MAX_BYTES];
    size_t  init_segment_len;

    /* esp_timer_get_time() (us) when this session's signed URLs were
     * resolved, so callers can proactively refresh well before any observed
     * expiry rather than relying solely on reacting to HTTP failures. */
    int64_t resolved_at_us;
} tunein_session_t;

/*
 * Runs TuneIn's non-browser control-plane flow:
 * profile contents -> itemToken -> Tune.ashx -> HLS master playlist
 *   -> AAC-LC variant playlist -> CMAF init segment.
 */
esp_err_t tunein_start_session(tunein_session_t *session);

/* Logs a redacted URL. Pass false for tokens/signed query strings. */
void tunein_log_url(const char *label, const char *url, bool include_query);

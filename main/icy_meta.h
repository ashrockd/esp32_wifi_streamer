#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "ringbuf.h"

/*
 * ICY inline metadata - The Vibe of Vegas's title/artist source.
 *
 * The 4 TuneIn/Apple Music stations get their now-playing track from an
 * ID3v2 tag riding inside an 'emsg' box in their CMAF/fMP4 stream (see
 * nowplaying.h). The Vibe of Vegas is a completely different transport
 * (RADIO_VIBE_OF_VEGAS_DIRECT_URL in app_config.h - a plain MP3 stream, no
 * CMAF, no emsg, no fmp4_bridge in its pipeline chain at all - see
 * station_list.h), so it needed its own mechanism. It turns out to have one
 * that's arguably simpler: 181.fm's stream is a classic Shoutcast/Icecast-
 * style ICE stream, which supports "ICY inline metadata" - the same
 * mechanism player.181fm.com's OWN JavaScript (site.4.6.15.js) uses to fill
 * in the title/artist <div> the user pointed at (id="song"/id="artist") -
 * confirmed directly (2026-09-05) with:
 *
 *   curl -H "Icy-MetaData: 1" -D - https://listen.181fm.com/181-vibe_128k.mp3
 *     -> icy-metaint: 16000
 *
 * i.e. asking for it (the Icy-MetaData: 1 request header) makes the server
 * inject one small text block of metadata into the byte stream every 16000
 * bytes of audio, and tell you the interval via icy-metaint. Reading a
 * chunk at that offset by hand confirmed the shape:
 *
 *   StreamTitle='Riton x Oliver Heldens - Turn Me On (Radio Edit) ';StreamUrl='';StreamArtwork='';
 *
 * This is scraping the SAME underlying data source the user's suggested
 * approach (polling player.181fm.com's rendered HTML) would have gotten to
 * eventually, just at the point closest to the source: inline in the exact
 * bytes this project is already downloading for playback, with no second
 * HTTP/TLS connection (this project has crashed on exactly that kind of
 * redundant concurrent-TLS resource pressure before - see nowplaying.h's own
 * "SNIFFED, NOT FETCHED" redesign note) and no HTML/JS to parse (the raw
 * page's <span id="artist"> is empty until JavaScript runs - not something
 * firmware can do at all).
 *
 * Why this needed a NEW module instead of reusing fmp4_bridge.c's approach:
 * ICY metadata is interleaved WITH the audio bytes at the HTTP transport
 * level, not carried in a separate box type inside a container fmp4_bridge
 * already parses - The Vibe of Vegas's pipeline has no fmp4_bridge at all
 * (TUNEIN_FORMAT_DIRECT_GENERIC goes straight from http_stream to
 * esp_decoder, see radio_pipeline.c). The natural place to intercept the raw
 * byte stream before esp_decoder ever sees it is a write callback installed
 * on http_reader's OUTPUT - exactly the technique radio_pipeline.c's
 * led_viz_write_cb already uses to tap the DECODER's output for the LED
 * visualiser, just one stage earlier in the chain and actually transforming
 * the bytes (stripping the interleaved metadata blocks) rather than only
 * observing them. icy_meta_strip_and_forward() below is that transform;
 * radio_pipeline.c supplies the tiny audio_element write_cb wrapper around
 * it (same split as led_viz.h/led_viz_write_cb) and requests the
 * Icy-MetaData: 1 header from http_stream_hook()'s HTTP_STREAM_PRE_REQUEST
 * handling.
 *
 * Why the metaint value is a compile-time constant (RADIO_VIBE_OF_VEGAS_
 * ICY_METAINT in app_config.h) instead of read back from the response: ESP-
 * ADF's http_stream.c hardcodes its own internal esp_http_client
 * event_handler (_http_event_handle), which only forwards Content-Type/
 * -Encoding/-Range to callers - there is no public hook to see "icy-metaint"
 * or any other arbitrary response header without a local patch to that
 * vendored file (this project already carries a few - see
 * .github/ci-patches/README.md - but this one specific header wasn't worth
 * adding to that list for a value already confirmed stable). See app_config.h's
 * own comment on RADIO_VIBE_OF_VEGAS_ICY_METAINT for the risk this accepts
 * and how a future mismatch would surface.
 */

/**
 * One-time setup: allocates this module's PSRAM-backed scratch (this board
 * has 8MB of PSRAM - see nowplaying.h's own reasoning for why buffers this
 * module only touches from one task belong there rather than on that task's
 * stack or in scarce internal RAM). Idempotent - a second call is a no-op
 * returning ESP_OK. Call once at startup, alongside nowplaying_init().
 */
esp_err_t icy_meta_init(void);

/**
 * (Re)arms the stripper for a fresh connection. Call this right before (or
 * immediately after) opening an http_stream session that requested ICY
 * metadata, with the interval (in bytes of audio data between metadata
 * blocks) the server is expected to use - RADIO_VIBE_OF_VEGAS_ICY_METAINT
 * for the only caller today. Passing 0 (or never calling this at all before
 * the first icy_meta_strip_and_forward() call) disables stripping entirely:
 * every byte is forwarded unchanged, which is the safe degrade-to-passthrough
 * behavior for any station that never requested ICY metadata in the first
 * place.
 */
void icy_meta_reset(int metaint);

/**
 * The actual transform: reads `len` bytes of a raw ICY/Shoutcast HTTP body
 * from `buffer`, strips out any interleaved metadata blocks (parsing a
 * complete one via icy_meta_reset()'s interval, handing any non-empty
 * StreamTitle/StreamArtwork it finds to nowplaying_ingest_icy_title()), and
 * forwards the remaining pure audio bytes to `out_rb` via rb_write() - one or
 * more calls, since a single input chunk can contain any mix of audio bytes
 * and (parts of) a metadata block. Persistent byte-position state is kept
 * internally (module-static - see this file's header comment on why: this
 * is called from exactly one task, http_reader's own element task, the same
 * reasoning nowplaying.c and fmp4_bridge.c already document for their own
 * per-connection scratch) and spans calls, so chunk boundaries never need to
 * line up with metadata block boundaries.
 *
 * Return value mirrors rb_write()'s own convention (this IS what a plain
 * rb_write(out_rb, buffer, len, ticks_to_wait) tap would have returned, had
 * there been no metadata to strip) - a negative AEL_IO_* code on abort/
 * failure, propagated from whichever rb_write() call hit it, or the number
 * of INPUT bytes actually consumed on success (normally all of `len`).
 * Intended to be called from an audio_element write callback (see
 * radio_pipeline.c's icy_meta_write_cb wrapper and led_viz_write_cb for the
 * identical pattern one stage later in the same pipeline) - `ctx` there is
 * this http_reader's own output ring buffer, obtained via
 * audio_element_get_output_ringbuf() exactly as led_viz's tap already does
 * for the decoder.
 */
int icy_meta_strip_and_forward(char *buffer, int len, ringbuf_handle_t out_rb, TickType_t ticks_to_wait);

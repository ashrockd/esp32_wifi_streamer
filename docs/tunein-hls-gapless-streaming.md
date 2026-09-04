# TuneIn HLS streaming: how it works, why esp32_wifi_streamer had a gap, and what was done about it

Written 2026-08-22. Covers the investigation and code changes made in this
project session, in order, with the reasoning behind each one. Companion
reading: [tools/tunein_inspect.py](../../tools/tunein_inspect.py) (control-plane
flow) and
[testing-tools/python-script-albumart-title/tunein_nowplaying.py](../../testing-tools/python-script-albumart-title/tunein_nowplaying.py)
(now-playing metadata).

## 1. How TuneIn actually gets you audio (control plane)

TuneIn's public site is not the audio source for a station like Apple Music
Hits (`s345724`). The real flow, reverse-engineered without a browser
devtools/network tab (see `tunein_inspect.py`'s docstring for the method):

1. `GET api.tunein.com/profiles/<guideId>/contents` - station metadata, and
   critically an `itemToken`.
2. `GET opml.radiotime.com/Tune.ashx?id=<guideId>&itemToken=...&listenId=...` -
   TuneIn's "playback session" endpoint. For this class of station (an
   Apple-Music-curated collection), the response points not at a TuneIn
   media server but at Apple's own CDN:
   `https://itsliveradio.apple.com/3p-tune-in/tune_in/<id>/index-cmaf.m3u8`
3. That master playlist lists variants (bitrates); the app resolves the
   preferred one - a **media playlist**, e.g.
   `https://itsliveradio.apple.com/3p-tune-in/tune_in/<id>/usw/256.m3u8` -
   and that URL is what actually gets played and periodically re-fetched for
   the life of the session.

None of this is a documented/public device API - see `tunein_inspect.py`'s
module docstring for the standing caveat about signed URLs and stability.

## 2. Now-playing metadata: no separate API for this station class

`feed.tunein.com/profiles/<id>/nowPlaying` only returns the *station's* name/
tagline for Apple-curated stations, not the current track - confirmed by
calling it twice and getting the same answer both times. The real per-track
title/artist/album/artwork comes from **ID3 tags embedded directly in the HLS
audio segments** (see finding 3 below for exactly where), the same data
`hls.js`/`AVPlayer` surface client-side as "timed metadata" events. Full
reverse-engineering trail and a working polling extractor:
[tunein_nowplaying.py](../../testing-tools/python-script-albumart-title/tunein_nowplaying.py).

## 3. The media format: CMAF/fMP4, verified byte-for-byte

The stream is packaged as **fragmented MP4 (CMAF)**, confirmed by fetching
and parsing real segments (not assumed from documentation):

- The media playlist carries one shared `#EXT-X-MAP` init segment
  (`segment_..._init.mp4`), referenced by every subsequent media segment.
  Parsing its boxes: `ftyp / moov / mvex / trex` - i.e. the AAC-LC codec
  config (`esds` -> AudioSpecificConfig: sample rate, channel count) is
  declared **once**, not per segment.
- Every ~16s media segment is `styp + sidx + emsg(x4, timed ID3 metadata) +
  moof + mdat` - new samples only, no new codec config. The four repeated
  `emsg` boxes are almost certainly there so a client reading only the first
  N KB of a segment (exactly what `tunein_nowplaying.py` does via an HTTP
  Range request) reliably lands on one.
- **Segment boundaries are sample-accurate and contiguous by construction.**
  Decoded two consecutive segments' `tfdt` (`baseMediaDecodeTime`):
  segment 2426919 -> `1877899660800`, segment 2426920 -> `1877900429824`.
  Delta = `769024` samples, which is **exactly** `751 x 1024` - 751 AAC
  frames of 1024 samples each, matching segment 2426919's own `trun` sample
  count, at a timescale of 48000 Hz (769024/48000 = 16.02133s, matching the
  playlist's own `EXTINF:16.02131`). Zero gap, zero overlap between
  segments - the encoder cut the stream on exact frame boundaries. No
  `elst` (edit list) box exists either, so there's no priming-sample
  trimming to handle - a decoder that just keeps decoding contiguous `mdat`
  payloads across segment boundaries gets a bit-exact continuous stream for
  free.

## 4. How a browser (or hls.js) gets zero-gap playback from this

Two separate facts combine, and it's important to see them as separate:

1. **The container makes gaplessness free at the decode level** (section 3
   above) - one codec init, then a logically unbroken elementary stream.
   Any player that doesn't re-initialize its decoder per segment gets this
   for free.
2. **The player never lets network latency touch the output.** hls.js (and
   native HLS) keeps a live buffer of several segments (`liveSyncDurationCount`
   defaults to 3, i.e. ~48s at this stream's segment length) *behind* the
   fetch cursor, and reloads the live manifest on a timer well before that
   buffer would run dry. Every network cost - TLS handshake, TTFB, manifest
   GET+parse, next segment GET - happens while already-downloaded audio is
   still draining out of the buffer. The network is never on the critical
   path of what's coming out of the speaker.

Fact 2 is the one that actually matters for "no audible break, ever" - and
it's a buffering/scheduling discipline, not a decoder trick. This is the
fact esp32_wifi_streamer's original ~2-3s gap violated.

## 5. Why esp32_wifi_streamer had a gap, in order of what was found

### 5a. Intra-window segment transitions were already gapless

ESP-ADF's `http_stream` element (`auto_connect_next_track`) advances from one
already-known segment to the next on the same connection without tearing the
pipeline down, and `fmp4_bridge`/the AAC decoder keep their parsed codec
config across segments (matches section 3's finding - no per-segment
re-init needed). Confirmed on hardware: "10 segments / 5515161 bytes of
clean continuous playback" before the first boundary.

### 5b. The live-window boundary (every ~9 segments, ~144-168s) was not

A live HLS media playlist only lists a rolling window of segments. Once
`http_stream` exhausts every segment it currently knows about, it dispatches
`HTTP_STREAM_FINISH_PLAYLIST`, and (see
[radio_pipeline.c](../main/radio_pipeline.c)'s `http_stream_hook()`) the
handler re-fetches the *same* manifest URL, then opens whatever segment it
names - two sequential blocking HTTP requests on the pipeline's one
connection, with nothing buffered to play through the wait, because
`RADIO_HTTP_BUFFER_BYTES` (see [app_config.h](../main/app_config.h)) was only
8192 bytes (~0.25s of look-ahead at this stream's ~32KB/s).

### 5c. First fix: reuse the connection when the host doesn't change

`esp-adf`'s stock `http_stream.c` unconditionally closes and reopens the
connection before *every* request, even back-to-back ones to the same host.
Patched in as an opt-in `reuse_conn_same_host` flag (see the `LOCAL PATCH`
blocks in
[http_stream.c](../../esp-adf/components/audio_stream/http_stream.c) /
[http_stream.h](../../esp-adf/components/audio_stream/include/http_stream.h)) -
skips the redundant TCP+TLS handshake when the new request's host matches
the current connection's host and the previous response was fully read.
Measured effect on hardware: the full re-resolve-everything path (10-15s of
silence) was replaced by an in-place pipeline restart (avoids re-running the
whole TuneIn control-plane flow), landing around a ~1-2s gap per the
project's own hardware log at the time.

### 5d. Second finding: that fix structurally can't reach zero

Checked `_http_load_uri()`'s reuse condition directly: it only skips the
reconnect when the new request's host matches the last connection's host.
But **the manifest lives on `itsliveradio.apple.com` while every media
segment is served from `liveradio.music.apple.com`** (see section 1) - a
different host. So the manifest-refetch -> first-new-segment hop is a
*forced* cross-host reconnect that this patch can never help with, by
construction. A hardware log confirmed this directly: **two** separate
`esp-x509-crt-bundle: Certificate validated` events at every boundary (one
full TLS handshake each), and the measured gap was actually ~3.0-3.2s, not
the earlier 1-2s.

### 5e. Why a bigger buffer can't fix 5d either

This stream runs ~32KB/s of compressed AAC. Fully hiding a ~3.2s stall needs
roughly **100KB** of look-ahead just in the HTTP ring buffer. Matching what a
real player does by default (hls.js's ~48s live buffer) needs **~1.5MB**.
This chip (ESP32-WROOM-32U, 4MB flash, **no PSRAM**) has roughly 320KB of
usable internal SRAM total, and boots with only ~268KB free *before* Wi-Fi
even starts. Even the smallest meaningful number (one segment ahead, ~512KB)
exceeds the chip's entire SRAM budget before subtracting anything Wi-Fi/TLS/
decoder need. **This is a hard physical ceiling, not a tuning problem** -
matching hls.js's own buffering strategy is not reachable on this exact
board without PSRAM.

`RADIO_HTTP_BUFFER_BYTES` was nonetheless raised 8192 -> 16384 on 2026-08-22,
not to meaningfully close the boundary gap (a doubling from ~0.25s to ~0.5s
of look-ahead is trivial against a 3.2s stall) but because **hardware testing
showed it fixed a real, separate bug**: at 8192, playback audio would
randomly speed up/slow down; at 16384 it stopped. (A likely-unrelated,
still-present small residual drift - roughly -0.15% x3, +0.49% x1, looping -
looks like ordinary clock-domain drift between this board and
`esp32_bt_speaker`'s own independent crystal, since they're linked via I2S
with no shared MCLK line; that's a `esp32_bt_speaker`-side concern, out of
scope here.)

## 6. The real fix: fetch the next window before the current one runs out

Since buffering-through the stall is physically off the table, the only
option left is the one browsers actually use: know the next window's segment
list **before** the current one runs dry, fetched on a genuinely separate
connection while the existing one keeps delivering the last known segment
undisturbed. Implemented 2026-08-22 as **`playlist_prefetch`**:

- **[playlist_prefetch.h](../main/playlist_prefetch.h) /
  [playlist_prefetch.c](../main/playlist_prefetch.c)** (new) - a standalone
  module, independent of ESP-ADF. `playlist_prefetch_start(url)` spawns one
  short-lived FreeRTOS task that opens its own `esp_http_client` (own TLS
  session, untouched by/untouching the pipeline's own connection), GETs the
  manifest, parses it with a small hand-rolled scanner (same technique as
  `tunein_inspect.py`'s `parse_variant()` / `tunein_nowplaying.py`'s
  `parse_m3u8_segments()` - deliberately not reusing ESP-ADF's own
  `hls_playlist_*` parser, which lives behind a *private* include dir of the
  `audio_stream` component and would have meant patching yet another shared
  CMakeLists just to expose it), posts the resulting segment URIs to a
  queue, and deletes itself. Read buffer (8KB) and result queue are
  allocated once at boot, while the heap is still clean - same reasoning as
  `fmp4_bridge`'s own preallocated scratch buffers.
- **`http_stream.c`/`.h`** (LOCAL PATCH, second one in that file) - two new
  functions: `http_stream_get_tracks_remaining()` (how many not-yet-started
  segments are left in the live playlist - the trigger: firing at `<=1`
  still leaves the *entire* last segment's ~16s to work with) and
  `http_stream_playlist_insert_tracks()` (appends already-fetched URIs to
  the *same* queue `auto_connect_next_track` already drains, deduped the
  same way a normal playlist re-resolve is - see `http_playlist_insert()`).
  Once tracks are inserted this way, the transition to them is
  indistinguishable from an ordinary same-window segment hop: no
  `HTTP_STREAM_FINISH_PLAYLIST` event, no synchronous refetch, for that
  boundary at all. A new mutex (`http->playlist_lock`) guards every touch of
  the shared track queue, because this is the first time anything besides
  the element's own task ever calls into it concurrently.
- **`http_playlist.c`/`.h`** (LOCAL PATCH) - one small addition,
  `http_playlist_count_remaining()`, since the track queue's internal
  `track_t` struct is private to that file.
- **[radio_pipeline.c](../main/radio_pipeline.c)** - `radio_pipeline_wait()`'s
  loop now caps its blocking wait to ~1s (was previously unbounded/until the
  session deadline) so it can poll prefetch state regularly; a new
  `service_playlist_prefetch()` drains any completed prefetch result
  (inserting it via the function above) and starts a new one when the
  segment count drops low. `radio_pipeline_stop()` discards any leftover
  in-flight prefetch result before tearing down, so a result fetched for one
  session's signed URL can never be handed to the next session's
  `http_reader`.

**This is entirely additive/best-effort** - if a prefetch fails or doesn't
finish in time, the existing reactive `HTTP_STREAM_FINISH_PLAYLIST` path
(section 5b/5c) still runs exactly as before, so nothing about this feature
can make a boundary worse, only better or unchanged.

### Memory cost, and the honest tradeoff

- **Permanent**: ~8KB (`playlist_prefetch`'s read buffer, reserved once at
  boot, never freed - by design, so it's never competing for a fresh
  allocation right when heap is already under pressure).
- **Transient** (~0.5-2s, once per ~150s window): a second TLS session's
  worth of RAM, ballpark ~17-21KB per this project's own previously-measured
  numbers for any single HTTPS request with cert-bundle verification.

A hardware log taken *before* this feature existed already showed
`MALLOC_CAP_8BIT` free heap as low as 5.5-14KB with largest contiguous block
down to 1.6-8.7KB during ordinary steady-state playback - tighter than this
project's own documented OOM history assumed. **This has not yet been
hardware-validated at these settings.** See
[app_config.h](../main/app_config.h)'s 2026-08-22 comment for the exact
fallback plan if it doesn't survive: `RADIO_HTTP_BUFFER_BYTES` is the first
lever to pull (even below 8192), since a working prefetch makes that
buffer's original job largely moot - but test that against the pitch-
instability symptom it was raised to fix (section 5e) before committing to a
smaller value long-term.

## 6a. First hardware test, and two bugs it found

First flash of the prefetch feature (16384 HTTP buffer, unchanged from
section 5e) crashed the pipeline on the very first segment open with the
exact `PK verify failed with error 0x4290` RSA-verify-OOM signature section
5e's history already documented - at 39168 free/38912 largest block, i.e.
*more* headroom than the earlier 34012/29696 failure, and it still wasn't
enough. `RADIO_HTTP_BUFFER_BYTES` was cut to 4096 (below the previously-
stable 8192) to make room for prefetch's new permanent 8KB buffer - see
`app_config.h`'s own comment trail for the exact reasoning.

Second flash (4096 buffer) cleared that crash, but surfaced a real bug in
the prefetch trigger logic itself: with no backoff, a persistently-OOM'd
prefetch was retried on *every* ~1s poll for **33 straight seconds** at one
live-window boundary (free heap bouncing 3.5-11KB, `min_free_ever` hit 700
bytes) - each attempt doing a fresh `xTaskCreate`/`esp_http_client_init`/
`mbedtls_ssl_setup` allocation that failed, competing directly with the
pipeline's own critical-path TLS handshake for that same boundary. That
contention (not silence) is what came back as renewed audio instability.
Fixed with a real backoff (`PLAYLIST_PREFETCH_RETRY_BACKOFF_MS`, 4s) and a
hard cap of 2 attempts per boundary (`PLAYLIST_PREFETCH_MAX_ATTEMPTS_PER_
WINDOW`) before yielding to the existing reactive fallback - see
`radio_pipeline.c`'s `service_playlist_prefetch()`.

Same test also surfaced a **stock ADF quirk, not a bug of this project's
own**: `audio_event_iface_listen()` (`esp-adf/components/audio_pipeline/
audio_event_iface.c`) returns plain `ESP_FAIL` for both a genuine timeout
*and* a real failure - there is no distinct `ESP_ERR_TIMEOUT` in this ADF
version. `radio_pipeline_wait()`'s pre-existing `if (err ==
ESP_ERR_TIMEOUT)` check was accordingly always dead code; it just never
mattered before because the wait was effectively unbounded (portMAX_DELAY /
the ~20-minute session deadline) and ADF's own elements send events well
within that. Capping the wait to ~1s for prefetch polling (section 6)
started hitting this path constantly, logging a false "Event listener
error: ESP_FAIL" once a second for the whole session - harmless to
correctness, but noisy and worth silencing since it isn't a real signal.

`PREFETCH_TASK_STACK_BYTES` was also cut 5120 -> 3584 from this test's own
measured high-water-mark log (2732-3100 bytes actually used).

## 7. Backups

Full pre-change snapshots of every touched file, following this project's
existing `backups_V1.0/` convention:

- `backups_V1.0.1/esp32_wifi_streamer/main/*.V1.0.1` - every file in `main/`
  as of just before this session's changes.
- `backups_V1.0.1/esp-adf/components/audio_stream/{http_stream.c,
  http_playlist.c}.V1.0.1` and `include/http_stream.h.V1.0.1` - the shared
  ESP-ADF files patched for connection reuse (earlier session) and playlist
  prefetching (this one).

## 8. Two other things found along the way, worth keeping in mind

- **Serial ports**: `esp32_wifi_streamer` enumerates as a CH340 bridge on
  **COM10**; `esp32_bt_speaker` (the other agent's chip) is a CP210x bridge
  on **COM7**. `build.ps1`'s own `-Port` default is stale (still says
  `COM7`) - always pass `-Port COM10` explicitly when flashing this project.
- This machine accumulates orphaned `idf.py .../monitor` processes across
  sessions, which hold both the serial port and
  `build/log/idf_py_stderr_output_*` open - `idf.py set-target`'s implicit
  `fullclean` fails with a Windows file-lock `PermissionError` when that
  happens. Check for stale python processes referencing the target
  project's `.elf` path before assuming anything else is wrong.

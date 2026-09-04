# CI patches for the vendored ESP-ADF / ESP-IDF trees

This directory holds every local modification to the vendored `../esp-adf`
(and its `components/esp-adf-libs` submodule) and `../esp-idf` trees that
`esp32_wifi_streamer` actually depends on to build **correctly** - not just
successfully. `esp32_wifi_streamer/.github/workflows/build.yml` applies all
of them, in the order listed below, to freshly-checked-out copies of those
trees before running `idf.py build`.

## Why this exists

`../esp-adf` and `../esp-idf` are git repos vendored one level above this
project and shared with sibling projects (`esp32_bt_speaker*`). They carry
**uncommitted** local edits on the dev machine - `git diff`/`git status`
inside them show the changes, but nothing is ever pushed anywhere, so CI
starts from a clean upstream checkout every time and has no way to see these
edits unless they're captured and replayed. That's what this directory is.

## Base versions (verified 2026-09-04)

| Tree | Base | How determined |
|---|---|---|
| `esp-adf` | commit `eac70fd2` on branch `release/v2.x`, i.e. `git describe` = `v2.8-14-geac70fd2` (14 commits past tag `v2.8`) | `git -C esp-adf log/branch/describe` - real git repo, remote `https://github.com/espressif/esp-adf.git` |
| `esp-adf/components/esp-adf-libs` (submodule) | commit `3472016` (detached HEAD, matches what `esp-adf`'s superproject index records at `eac70fd2` - `git submodule status` shows no `+`/`-` drift prefix) | `git -C esp-adf/components/esp-adf-libs log` - remote `https://github.com/espressif/esp-adf-libs` |
| `esp-idf` actually used by the real build | tag `v5.5.5` exactly, **standalone install at `C:\esp\v5.5.5\esp-idf`**, NOT `esp-adf`'s own bundled `esp-idf` submodule | see "Which ESP-IDF actually matters" below |

`esp-adf`'s own bundled `esp-idf` submodule is pinned to **`v5.5.3`** (`git
-C esp-adf/esp-idf describe` = `v5.5.3`) - a different patch version. It is
**not relevant** to this project; see below.

### Which ESP-IDF actually matters (verified, not assumed)

`build.ps1` sources two scripts in order:
1. `C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1` (the official
   Espressif-IDE-installed v5.5.5 profile) - this sets `$env:IDF_PATH =
   "C:\esp\v5.5.5\esp-idf"` unconditionally.
2. `esp-adf\export.ps1` - which only does `if (-not $env:IDF_PATH) { $env:IDF_PATH
   = Join-Path $ADF_PATH "esp-idf" }`. Since step 1 already set `IDF_PATH`,
   this branch never runs, and ESP-ADF's own bundled submodule is never
   touched. `esp-adf/CMakeLists.txt` has the exact same `if(NOT DEFINED
   ENV{IDF_PATH})` guard, so this holds at the CMake level too, not just in
   the PowerShell wrapper.

So the ESP-IDF tree whose diffs matter is the **standalone v5.5.5 install**,
confirmed via `git -C C:\esp\v5.5.5\esp-idf describe --tags` = exactly
`v5.5.5` (on the tag, zero extra commits) with the 3 local files below as its
only modifications. The CI workflow reproduces this by using the
`espressif/idf:v5.5.5` Docker image's own baked-in `/opt/esp/idf` (also
exactly v5.5.5) instead of fetching `esp-adf`'s bundled v5.5.3 submodule at
all - see `build.yml`'s comments for the full reasoning.

## Patches applied (esp-adf) - and how each maps to a project source comment

Extracted with `git -C esp-adf diff -- <file>` against the `eac70fd2` base
above, so each applies cleanly with a plain `git apply` once `esp-adf` is
checked out at that exact commit.

1. **`esp-adf/0001-i2s-pin-override-m5stack_atoms3r.patch`**
   `components/audio_board/m5stack_atoms3r/board_pins_config.c` - repoints
   `get_i2s_pins()` from stock M5Stack AtomS3R pins (bck 8/ws 6/dout 5/din 7)
   to this project's actual wiring (bck 4/ws 5/dout 6, no din/mclk).
   **THE CRITICAL PATCH.** `i2s_stream_idf5.c`'s `i2s_driver_startup()` calls
   this function and `memcpy()`s its result straight over whatever
   `radio_pipeline.c` configured, unconditionally, with no public API to
   override it. Building against an unpatched/fresh `esp-adf` checkout
   compiles fine and boots fine - it just silently drives the wrong GPIOs,
   no compile error, no runtime error. Documented at length in
   `main/app_config.h` (the `RADIO_I2S_BCLK_GPIO` comment block, approx.
   lines 270-296) and `main/radio_pipeline.c`'s `check_i2s_pins()` function
   and its header comment (approx. lines 96-153), which is this project's
   own runtime guard against exactly this class of drift (it reads the pins
   back via this same `get_i2s_pins()` and refuses to start the pipeline on
   a mismatch) - but that guard only fires once the firmware is *running on
   real hardware*; it cannot help a CI build catch the same silent drift, so
   the CI workflow's patch-verification step (see `build.yml`) is this
   project's CI-side equivalent of that runtime check.

2. **`esp-adf/0002-m5stack_atoms3r-board-adc-channel-api-fix.patch`**
   `components/audio_board/m5stack_atoms3r/board.c` - `ADC1_CHANNEL_4` ->
   `ADC_CHANNEL_4` in `audio_board_key_init()`. This is a straight compile
   compatibility fix (legacy `driver/adc.h` enum name -> the modern
   `hal/adc_types.h` enum name this ESP-IDF version expects); not referenced
   by name anywhere under `main/` since it has no behavioral effect on this
   project (it's dead code here - `audio_board_key_init()` is never called,
   this board has no physical buttons wired per `app_config.h`). Included
   anyway because `audio_board/CMakeLists.txt` compiles all of
   `m5stack_atoms3r/board.c` unconditionally whenever
   `CONFIG_M5STACK_ATOMS3R_BOARD=y` (confirmed by reading that CMakeLists.txt
   directly - there's no finer-grained file selection), so **without this fix
   the build does not compile at all**, regardless of MINIMAL_BUILD.

3. **`esp-adf/0003-audio_board-idf_component-drop-ili9341-dep.patch`**
   `components/audio_board/idf_component.yml` - removes a component-manager
   dependency on `esp_lcd_ili9341: "^1"` (replaced with `dependencies: {}`).
   This manifest is shared by the whole `audio_board` component (not
   per-board), so it's resolved regardless of which board is selected. Not
   referenced anywhere under `main/` - the reason for the local removal
   isn't documented in this project's own source, only inferable from the
   diff itself. **Flagged as lower-confidence** (see the task report) - it's
   a real, verified local edit, and reproducing it is safe (it only removes
   an unused dependency), but its root cause wasn't confirmed the way the
   I2S pin patch was.

4. **`esp-adf/0004-audio_pipeline-cmakelists-werror-return-type.patch`**
   `components/audio_pipeline/CMakeLists.txt` - adds
   `target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=return-type)`.
   `audio_pipeline` is a direct `REQUIRES` of `main/CMakeLists.txt`, so this
   component always builds. Not referenced under `main/`; presumably added
   because this ESP-IDF version's default warning flags promote a
   missing-return warning somewhere in `audio_pipeline`'s sources to a hard
   error. **Without this, the build fails to compile** (not just "builds
   wrong") - high-confidence necessary despite not being doc-linked, purely
   from what `REQUIRES` and the flag name imply.

5. **`esp-adf/0005-http_stream-connection-reuse-and-live-prefetch.patch`**
   Combined patch (4 files, one atomic feature - see "Why some patches are
   combined" below):
   - `components/audio_stream/http_stream.c`
   - `components/audio_stream/include/http_stream.h`
   - `components/audio_stream/http_playlist.c`
   - `components/audio_stream/http_playlist.h`

   Adds the opt-in `http_stream_cfg_t.reuse_conn_same_host` flag (skips a
   redundant TCP+TLS teardown/reconnect when back-to-back requests target the
   same host) plus `http_stream_get_tracks_remaining()` /
   `http_stream_playlist_insert_tracks()` / `http_playlist_count_remaining()`
   (the live-playlist-prefetch mechanism). Both changes are off-by-default /
   additive, so stock `http_stream` callers elsewhere in the tree are
   unaffected. Extensively documented in this project's own sources:
   - `main/radio_pipeline.c` lines ~287-303 (the `reuse_conn_same_host`
     assignment and its "2-SECOND-PAUSE FIX" comment, which explicitly says
     "LOCAL PATCH in esp-adf/components/audio_stream/http_stream.c - grep
     that file for 'LOCAL PATCH'").
   - `main/radio_pipeline.c` lines ~563-620 (`service_playlist_prefetch()`,
     which calls the two new `http_stream_*` functions).
   - `docs/tunein-hls-gapless-streaming.md` sections 5c/5d (connection reuse)
     and 6/6a (prefetch) - the full investigation trail, hardware
     measurements, and the two hardware-found bugs fixed along the way.
   - The vendored files' own `LOCAL PATCH` comment headers (grep either file
     for that string), which is this project's established convention for
     marking exactly this kind of edit.

   **2026-09-04: regenerated to also include `MAX_PLAYLIST_LINE_SIZE`'s
   512 -> 2048 widening** (unrelated to connection reuse/prefetch, but the
   same file - see patch #6 below for why). Kept as one file rather than
   splitting `http_stream.c` across two patches, to avoid two patches both
   claiming ownership of the same file's diff against the same base.

6. **`esp-adf/0006-line_reader-null-terminate-overflow-and-widen-buffers.patch`**
   `components/audio_stream/lib/hls/line_reader.c` /
   `components/audio_stream/lib/hls/include/line_reader.h` /
   `components/audio_stream/lib/hls/hls_parse.h` - fixes a real heap-
   corruption bug found on hardware, plus widens two related buffers.
   `line_reader_add_char()` dropped EVERY write once a line hit its
   `line_size` cap - including the caller's own NUL-terminator write, which
   is indistinguishable from any other write to that function - so an
   overlong line came back from `line_reader_get_line()` unterminated
   inside its own heap allocation. Every caller (`http_stream.c`'s playlist
   parsing, `hls_parse.c`'s HLS demuxer) treats that return value as a
   plain C string, so `strlen()`/URL-building code kept reading past the
   end of the buffer until it happened to hit a zero byte in whatever
   memory followed, splicing adjacent heap contents onto whatever was being
   built. **Confirmed on real hardware**: a fetched HLS segment URL (from
   `http_stream.c`'s live-playlist re-fetch) came back with several bytes
   of unrelated binary garbage appended after a real query string, which
   then failed `esp_http_client`'s URL parser outright - traced directly to
   this function via the matching `"LINE_READER: Line too long..."` log
   line appearing immediately before the corrupted URL in the same
   hardware log. Fixed by reserving the buffer's last byte exclusively for
   the terminator (real characters now stop one slot short of `line_size`,
   and the terminator write is honored unconditionally into that reserved
   slot) - an overlong line now comes back safely truncated instead of
   unterminated. Also widens `MAX_PLAYLIST_LINE_SIZE`
   (`http_stream.c`, folded into patch #5 above since it is the same file)
   and `HLS_MAX_LINE_CHAR` (`hls_parse.h`) from 512 to 2048 bytes, so this
   truncation path is not hit at all for realistic playlist lines (some
   stations' signed accessKey query strings alone run past 512 bytes) -
   cheap on this board's 8MB PSRAM. **Without this patch, any station whose
   playlist has a line over 512 bytes can corrupt the heap and crash the
   board** - not cosmetic, a real correctness/stability fix.

## Patches applied (esp-adf-libs submodule)

6. **`esp-adf-libs/0001-cmakelists-expose-esp_audio_codec.patch`**
   `CMakeLists.txt` - adds include dirs and prebuilt-library linkage for
   `esp_audio_codec` (headers/lib already ship inside this submodule commit,
   just weren't wired into the build by upstream). This is what exposes
   `esp_aac_dec`'s explicit-configuration API
   (`esp_aac_dec_cfg_t.no_adts_header`/`sample_rate`/`channel`) to
   `main/aac_dec_element.c`. Directly matches `main/radio_pipeline.c`'s own
   comment (~lines 349-361) on why `aac_dec_element.c`/`fmp4_bridge.c` exist
   at all: both of ESP-ADF's stock decode paths were tried on hardware and
   both failed ("This audio is RAW AAC" / "Failed to initialize") on a
   stream proven valid offline, because CMAF/fMP4 segments carry their
   `AudioSpecificConfig` once in an init segment rather than per-frame, and
   neither stock path can be told one - `esp_aac_dec_cfg_t`'s explicit fields
   are what let `aac_dec_element.c` hand it over directly instead of
   sniffing. **Without this patch, `main/aac_dec_element.c` almost certainly
   fails to compile/link** (missing headers/symbols) - high-confidence
   necessary.

## Patches applied (esp-idf, standalone v5.5.5 tree)

7. **`esp-idf/0001-freertos-xTaskCreateRestrictedPinnedToCore.patch`**
   Combined patch (3 files, one atomic feature, matching how ESP-ADF itself
   ships this same change - see below):
   - `components/freertos/esp_additions/freertos_tasks_c_additions.h`
   - `components/freertos/esp_additions/include/freertos/idf_additions.h`
   - `components/freertos/linker_common.lf`

   Adds `xTaskCreateRestrictedPinnedToCore()` to IDF's (non-SMP) FreeRTOS
   additions layer. **This is not really a project-invented patch** - it is
   ESP-ADF's own official patch, shipped in `esp-adf/idf_patches/
   idf_v5.5_freertos.patch`, which `esp-adf/export.ps1` and `export.sh` are
   *supposed* to auto-apply on every activation via `tools/
   adf_install_patches.py apply-patch`. It ended up hand-applied instead
   because, per this project's own comment in the affected file (dated
   2026-08-31), `git apply` could not match the patch's hunk context against
   this exact IDF 5.5.5 checkout.

   Investigating *why* surfaced a real bug worth flagging: `adf_install_
   patches.py`'s `apply_patch()` runs `subprocess.run(["git","apply",...],
   stdout=DEVNULL, stderr=DEVNULL)` with **no `check=True` and no return-code
   inspection at all** - a failed `git apply` is silently swallowed, no
   error, no warning, build continues as if nothing happened. That is
   exactly consistent with what happened on the dev machine, and there is no
   reason to expect CI's `espressif/idf:v5.5.5` image to behave differently
   (same IDF version, same patch file, same `git apply` context-matching
   logic) - so `build.yml` does **not** trust the automatic mechanism.
   Instead it applies this hand-verified patch file directly (diffed
   straight off this project's already-working v5.5.5 checkout, so it is
   guaranteed to match a byte-identical fresh v5.5.5 checkout) and then
   explicitly greps the result for `xTaskCreateRestrictedPinnedToCore`,
   hard-failing the CI job immediately if it's missing rather than letting a
   silent no-op surface later as a confusing link error.

   Confirmed load-bearing, not cosmetic: `xTaskCreateRestrictedPinnedToCore`
   is called from `esp-adf/components/audio_sal/audio_thread.c` - ESP-ADF's
   own task-creation abstraction, used throughout the audio pipeline. A
   silently-skipped patch would fail the **link** step, not the compile
   step, with an "undefined reference" error pointing nowhere near the real
   cause.

## Deliberately NOT included: files modified in `esp-adf` but not this project's

`git -C esp-adf status` also shows two more modified files, both excluded on
purpose:

- **`components/bluetooth_service/bluetooth_service.c`** - its own inline
  comments name `esp32_bt_speaker_48khz` explicitly (the `ESP_BT_MODE_BTDM`
  -> `ESP_BT_MODE_CLASSIC_BT` fix and the `esp_avrc_tg_init()` ordering fix
  are both about that project's A2DP-source/AVRCP-TG setup). `esp32_wifi_
  streamer` runs no Bluetooth stack at all (`main/app_config.h`'s file
  header says so explicitly), and `main/CMakeLists.txt`'s `REQUIRES` list
  has no `bt`/`bluetooth_service` entry - confirmed directly, not assumed.
- **`components/audio_board/lyrat_v4_2/board_pins_config.c`** - its own
  comment says "Repointed at esp32_bt_speaker's actual I2S wiring... it is
  the I2S SLAVE end of the two-chip link." `esp32_wifi_streamer`'s
  `sdkconfig.defaults` selects `CONFIG_M5STACK_ATOMS3R_BOARD=y` with
  `CONFIG_ESP_LYRAT_V4_3_BOARD` (and, transitively, any LyraT variant)
  explicitly unset - this file is never compiled into this project's build
  at all.

Both are real local patches - just for the sibling `esp32_bt_speaker` repo,
which owns its own CI concerns. Applying them here would be harmless (they
would not be compiled in) but is skipped to keep this patch set scoped to
what this project actually needs, per the task.

## Why some patches are combined into one file instead of one-per-source-file

Where a set of files implements one indivisible change - such that applying
a subset would leave the tree in a state that does not compile
(`http_stream.c` calls `http_playlist_count_remaining()`, declared in
`http_playlist.h` and defined in `http_playlist.c`; the freertos change
declares in one header and defines in one source file) - they're kept as one
combined `.patch` with multiple `diff --git` sections rather than split
per-file. `git apply` treats a multi-file patch as one atomic unit (it
validates every hunk across every file before writing anything), so this
also means a partially-matching tree fails the whole apply loudly instead of
silently landing in a broken, half-patched state. Every other change is a
true single-file, independently-meaningful edit and gets its own patch file.

## Application order (see `build.yml` for the actual commands)

1. Check out `esp-adf` at `eac70fd2`, init only the `components/esp-adf-libs`
   submodule (at `3472016`) - **not** the `esp-idf` submodule, per "Which
   ESP-IDF actually matters" above.
2. Apply all 6 `esp-adf/*.patch` files (`git apply`, run from the `esp-adf`
   checkout root) - `docker-build.sh` globs the directory alphabetically, so
   a new patch just needs the right filename prefix, no script change.
3. Apply `esp-adf-libs/0001-*.patch` (run from `esp-adf/components/
   esp-adf-libs`).
4. Apply `esp-idf/0001-*.patch` to the Docker image's baked-in `/opt/esp/idf`
   (run from there) - done *instead of* trusting `adf_install_patches.py`,
   then verified with a grep, per point 7 above.
5. Only then source IDF's `export.sh` and ADF's `export.sh` (the latter will
   also attempt its own `adf_install_patches.py apply-patch` run - harmless
   either way: it will find the freertos patch already applied and its own
   `git apply` attempt will just no-op/fail-silently against already-patched
   content, which is fine since step 4's grep already confirmed success
   before this step ever runs).

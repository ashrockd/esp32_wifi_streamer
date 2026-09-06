# esp32_wifi_streamer

Wi-Fi / streaming half of a two-ESP32 TuneIn-to-Bluetooth internet radio bridge.

This chip connects to Wi-Fi, resolves a configured [TuneIn](https://tunein.com/) station link to a live
stream URL, and pulls down the HLS/fMP4/AAC audio over HTTPS. It decodes that stream with
[ESP-ADF](https://github.com/espressif/esp-adf) and drives the decoded PCM out over I2S as the
**I2S master** (BCLK + WS + DATA) to a second ESP32.

It runs no Bluetooth stack at all — Wi-Fi, mbedTLS/TLS, and the AAC decoder get the full RAM
budget of this chip, with nothing else competing for it.

## Companion project

This board is one half of a pair. The other half,
**[esp32_bt_speaker](https://github.com/ashrockd/esp32_bt_speaker)**, receives the PCM this chip
sends over I2S and forwards it out to a classic-Bluetooth (A2DP) speaker. The two firmwares are
built and flashed independently but are only useful together — they replace what was originally a
single ESP32 doing Wi-Fi, TLS, decode, *and* Bluetooth/A2DP at once, split across two chips so each
side gets an untouched heap for its own job instead of Wi-Fi/TLS/Bluetooth fighting over one radio's
worth of RAM. It also carries a one-way UART link back to this chip so a Bluetooth AVRCP
next/previous press can skip stations (see below).

## Hardware

Currently targets an **ESP32-S3-WROOM-1 N16R8** (16MB flash, 8MB octal PSRAM), migrated from an
original ESP32-WROOM-32U (4MB flash, no PSRAM) — see `main/app_config.h`'s header comment and
`main/sdkconfig.defaults` for what that changed and why. Console/flashing is over the board's
USB-UART bridge (`idf.py -p <port> flash monitor`; see `build.ps1`).

### I2S link (this chip → esp32_bt_speaker)

Both firmwares' `main/app_config.h` hard-code the same three GPIOs for the cross-board I2S link
(no MCLK wire needed for a direct ESP32-to-ESP32 digital link — just the 3 signal wires + a shared
ground):

| Signal | Role on this chip | GPIO |
|---|---|---|
| BCLK (bit clock) | output (I2S master) | GPIO4 |
| WS / LRCLK (word select) | output (I2S master) | GPIO5 |
| DATA (audio) | output — DOUT | GPIO6 |
| GND | shared reference | — |

`esp32_bt_speaker` wires the identical trio as its I2S **slave** input (BCLK/WS in, DIN only) —
see that repo's README for its side of the link. **Note:** on this ESP-ADF version the pins actually
driven at runtime come from the selected audio board's `get_i2s_pins()` (a local ESP-ADF patch),
not straight from these macros — see the large comment above `RADIO_I2S_BCLK_GPIO` in
`main/app_config.h` before changing either side.

### AVRCP next/previous bridge (UART, receive-only)

A one-way UART link from `esp32_bt_speaker` (which is the AVRCP target for the paired Bluetooth
speaker) forwards transport button presses as ASCII lines. This chip only ever receives — no TX
pin is configured — and only acts on NEXT/PREVIOUS (this is live radio, so there's no track to
seek within). Default: UART2, RX on GPIO16, 115200 8N1. See `main/avrcp_uart.h` for the wire
format and `main/app_config.h` for pin reasoning.

### On-board LED

A single addressable RGB LED (WS2812/SK6812, GPIO48 on most N16R8 dev boards — check your board
revision) is driven as a one-pixel audio visualizer: hue cycles continuously and independently of
audio, brightness flashes on audio peaks that cross a runtime-adjustable dBFS threshold. See
`main/led_viz.h`.

## Software architecture

Built on ESP-ADF's audio pipeline. The element chain depends on the station's stream format
(`main/tunein_control.h` / `main/radio_pipeline.c`):

- **CMAF/fMP4/AAC-LC over HLS** (the Apple Music–curated TuneIn stations): `http_stream` →
  `fmp4_bridge` (a custom element unwrapping ISO-BMFF boxes) → `aac_dec_element` → `i2s_stream`.
- **Plain generic streams** (MP3/ICY, e.g. the direct 181.fm station): `http_stream` →
  `esp_decoder` → `i2s_stream`.

Key modules under `main/`:

| File | Responsibility |
|---|---|
| `tunein_control.c/.h` | Resolves a TuneIn `guideId` to a playable stream session (profile + `Tune.ashx` control plane), or bypasses TuneIn entirely for a direct stream URL. |
| `radio_pipeline.c/.h` | Builds/tears down the ESP-ADF pipeline, drives the main wait/event loop, services playlist prefetch. |
| `fmp4_bridge.c/.h` | Custom ADF element that demuxes fragmented MP4 (CMAF) segments into raw AAC for the decoder. |
| `aac_dec_element.c/.h` | AAC decode element wrapping `esp_aac_dec`. |
| `playlist_prefetch.c/.h` | Fetches the *next* live-window's HLS segment list ahead of time on a separate connection, to hide the cross-host reconnect stall at playlist-window boundaries (see `docs/tunein-hls-gapless-streaming.md`). |
| `icy_meta.c/.h` | Strips ICY (Shoutcast) inline metadata for stations that use it. |
| `nowplaying.c/.h` | Extracts now-playing title/artist, either from ID3-in-CMAF timed metadata or ICY metadata depending on station. |
| `station_list.c/.h` | The station catalog, NVS-persisted selection, next/prev. |
| `avrcp_uart.c/.h` | Receives AVRCP NEXT/PREVIOUS from the companion chip over UART. |
| `console_cli.c/.h` | Interactive serial console (see below). |
| `led_viz.c/.h` | On-board LED audio visualizer. |
| `latency_cal.c/.h` | Interactive LED/audio latency calibration flow. |
| `app_config.h` | All user-facing configuration: Wi-Fi credentials, GPIOs, buffer sizes, timeouts — heavily commented with the hardware/memory reasoning behind each value. |

## Features

- **Multi-station**, switchable at runtime (NVS-persisted), via three independent input paths that
  all feed the same next/prev logic: the on-chip serial console, the AVRCP UART bridge from
  `esp32_bt_speaker`, and automatic failover past a station whose stream/API call is actually
  failing.
- **Gapless-ish live HLS playback** across playlist-window boundaries via `playlist_prefetch`
  (best-effort — falls back to the original reactive re-fetch if a prefetch doesn't land in time).
  See `docs/tunein-hls-gapless-streaming.md` for the full investigation and design.
- **Now-playing metadata** surfaced from whichever source a given station actually provides
  (ID3-in-CMAF timed metadata, or ICY inline metadata).
- **Interactive serial console** (`console_cli.h`) on the same USB/serial link used for
  flashing/logging — no extra wiring:
  - `next` / `prev` — switch station.
  - `led-thresh [dBFS]` — get/set the LED peak-brightness threshold (persisted).
  - `latency [ms]` — get/manually set LED/audio latency compensation (persisted).
  - `cal` — guided interactive latency calibration (`beep` / `heard` / `<number>` / `accept` /
    `cancel`).
  - `status` — currently playing station, LED threshold, latency compensation.
- **Resource headroom logging** — periodic free-heap/free-flash reporting to inform buffer sizing
  decisions (`RADIO_RESOURCE_LOG_INTERVAL_MS`).
- **Self-healing reboot** if DMA-capable internal RAM stays critically low for a sustained period
  (a mitigation for a not-yet-root-caused slow leak in that pool — see `app_config.h`).
- **Automatic station failover** — a real stream/API failure advances to the next station instead
  of stalling behind exponential backoff on one bad entry; only after every station has failed once
  in the same streak does it fall back to normal backoff.

## Building

This is an [ESP-ADF](https://github.com/espressif/esp-adf) project (which itself wraps
[ESP-IDF](https://github.com/espressif/esp-idf)). Export both environments so `ADF_PATH` (and
`IDF_PATH`) are set, then build as usual:

```sh
. $IDF_PATH/export.sh
. $ADF_PATH/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

`build.ps1` automates this for the Windows/PowerShell dev setup this project was written against
(fixed paths for the ESP-IDF/ESP-ADF checkouts, `-Clean`/`-Flash`/`-Port` switches) — adjust its
hardcoded paths for your own machine before use.

Set `MINIMAL_BUILD=1` in the environment before configuring to build only the ESP-ADF components
this project's `main/CMakeLists.txt` actually `REQUIRES` (no Bluetooth components at all on this
chip) instead of ESP-ADF's entire component tree — see `CMakeLists.txt`.

Before building, edit `main/app_config.h` for your own Wi-Fi credentials and hardware wiring (do
not commit real credentials/URLs to a public fork, and don't paste serial logs containing signed
TuneIn URLs into public issues).

## Status

Working end-to-end (Wi-Fi → TuneIn resolve → decode → I2S out), with active tuning around I2S
clock-drift/jitter between the two boards. See `main/app_config.h` for the current, commented
constants and the reasoning behind each.

## Further reading

- [`docs/tunein-hls-gapless-streaming.md`](docs/tunein-hls-gapless-streaming.md) — how TuneIn's HLS
  control/media plane actually works, why the original firmware had an audible gap at every
  playlist-window boundary, and the `playlist_prefetch` fix.

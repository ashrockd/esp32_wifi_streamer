# esp32_wifi_streamer

Wi-Fi / streaming half of a two-ESP32 TuneIn-to-Bluetooth internet radio bridge.

This chip connects to Wi-Fi, resolves a configured [TuneIn](https://tunein.com/) station link to a live
stream URL, and pulls down the HLS/fMP4/AAC audio over HTTPS. It decodes that stream with
[ESP-ADF](https://github.com/espressif/esp-adf) and drives the decoded PCM out over I2S as the
**I2S master** (BCLK + WS + DATA) to a second ESP32.

It runs no Bluetooth stack at all — Wi-Fi, mbedTLS/TLS, and the AAC decoder get the full ~320KB of
this chip's usable RAM (no PSRAM) with nothing else competing for it.

## Companion project

This board is one half of a pair. The other half,
**[esp32_bt_speaker](https://github.com/ashrockd/esp32_bt_speaker)**, receives the PCM this chip
sends over I2S and forwards it out to a classic-Bluetooth (A2DP) speaker. The two firmwares are
built and flashed independently but are only useful together — they replace what was originally a
single ESP32 doing Wi-Fi, TLS, decode, *and* Bluetooth/A2DP at once, split across two chips so each
side gets an untouched heap for its own job instead of Wi-Fi/TLS/Bluetooth fighting over one radio's
worth of RAM.

## I2S link (this chip → esp32_bt_speaker)

Both firmwares' `main/app_config.h` hard-code the same three GPIOs for the cross-board I2S link
(no MCLK wire needed for a direct ESP32-to-ESP32 digital link — just the 3 signal wires + a shared
ground). The sdkconfig on both boards has `CONFIG_ESP_LYRAT_V4_3_BOARD=y` (ESP-ADF's ESP32-LyraT
V4.3 board profile); the pins below are this project's own I2S GPIO assignment, chosen to steer
clear of that board's strapping pins (0, 2, 5, 12, 15), its flash-connected pins (6–11), UART0
(1, 3 — used for flashing/serial console), and the input-only pins (34–39, unusable for this side's
outputs):

| Signal | Role on this chip | GPIO |
|---|---|---|
| BCLK (bit clock) | output (I2S master) | GPIO26 |
| WS / LRCLK (word select) | output (I2S master) | GPIO25 |
| DATA (audio) | output — DOUT | GPIO27 |
| GND | shared reference | — |

`esp32_bt_speaker` wires the identical GPIO26 / GPIO25 / GPIO27 trio as its I2S **slave** input
(BCLK/WS in, DIN only) — see that repo's README for its side of the link.

## Status

Working end-to-end (Wi-Fi → TuneIn resolve → decode → I2S out), with active tuning around I2S
clock-drift/jitter between the two boards. See `main/app_config.h` for the current, commented
constants and the reasoning behind each.

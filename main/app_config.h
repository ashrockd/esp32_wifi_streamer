#pragma once

#include "driver/uart.h" /* for UART_NUM_2 (RADIO_AVRCP_UART_PORT below) */

/*
 * User configuration for the Wi-Fi/streaming chip - one half of the
 * two-ESP32 split of esp32_tunein_bt_radio. This chip does Wi-Fi + TuneIn
 * control-plane + HLS/fMP4/AAC decode, then hands decoded PCM to the other
 * chip (esp32_bt_speaker) over I2S. It has no Bluetooth stack at all - see
 * ../../esp32_bt_speaker for that half.
 *
 * 2026-08-22 (hardware migration): moving off the ESP32-WROOM-32U (4MB
 * flash, no PSRAM) onto an ESP32-S3-WROOM-1 N16R8 (16MB flash, 8MB octal
 * PSRAM). See RADIO_HTTP_BUFFER_BYTES/RADIO_FMP4_BUFFER_BYTES/
 * RADIO_DECODER_BUFFER_BYTES and the I2S pin block below for what changed
 * and why - short version: ESP-ADF's own audio_calloc()/audio_malloc()
 * (esp-adf/components/audio_sal/audio_mem.c) unconditionally allocate from
 * MALLOC_CAP_SPIRAM whenever CONFIG_SPIRAM_BOOT_INIT is set, which is exactly
 * what every audio_element ring buffer in this pipeline (http_reader's,
 * fmp4_bridge's, the AAC decoder's) is created through - so enabling PSRAM
 * moves all of them off the ~170KB internal DRAM this project spent its
 * entire pre-migration history fighting over, onto an 8MB pool, for free.
 * CONFIG_SPIRAM_USE_MALLOC (see sdkconfig.defaults) does the same for plain
 * malloc()/calloc() above its size threshold, which is what mbedTLS's own
 * 16KB IN_CONTENT_LEN record buffer and esp_aac_dec_open()'s internal ~51KB
 * allocation both use - the two allocations most responsible for this
 * project's whole "PK verify failed with error 0x4290" / "Dynamic Impl:
 * alloc(...) failed" OOM history. None of that history is deleted below -
 * it's the reason the new numbers are sized generously rather than
 * arbitrarily, and it still applies verbatim if this project is ever built
 * for a non-PSRAM target again.
 *
 * Do not paste serial logs containing signed TuneIn URLs into public issues.
 */
#define RADIO_WIFI_SSID             "Ashish"
#define RADIO_WIFI_PASSWORD         "kgf@4646"
/* Multi-station: the station is no longer fixed at compile time - see
 * station_list.h for the catalog (sourced from
 * ../../tunein_music_stations.csv's streamable Apple-branded entries) and
 * the NVS-backed selection/next/prev/persistence logic. This macro now only
 * supplies the FIRST-BOOT default, before anything has ever been saved to
 * NVS - it must be one of the ids in RADIO_STATIONS; station_list.c logs an
 * error and falls back to index 0 if it somehow isn't. */
#define RADIO_TUNEIN_STATION_ID     "s345724" /* Apple Music Hits */

/* AVRCP next/previous-station bridge: a one-way UART link FROM the
 * companion esp32_bt_speaker chip, which is the AVRCP target for the paired
 * Bluetooth speaker and forwards the transport commands it receives as
 * ASCII lines (see avrcp_uart.h for the wire format). That chip transmits on
 * its own GPIO16 (RADIO_AVRC_UART_TX_GPIO in its app_config.h); wire it to
 * the RX pin below, plus a COMMON GROUND between the two boards. This chip
 * only ever receives on this link - no TX pin is configured.
 *
 * GPIO16 is free on this board by the same reasoning already applied to the
 * I2S pins above: clear of the ESP32-S3-WROOM-1 N16R8's reserved in-package
 * flash/PSRAM block (GPIO26-37), clear of the strapping pins (0, 3, 45, 46),
 * clear of GPIO19/20 (native USB D-/D+, the console), and clear of the I2S
 * pins (4/5/6). UART2 is used rather than UART0, which is the console.
 * Unlike the I2S pins, nothing in ESP-ADF's audio_board layer overrides
 * this - it is a plain IDF UART, so what is written here is what the
 * hardware actually uses. */
#define RADIO_AVRCP_UART_PORT       UART_NUM_2
#define RADIO_AVRCP_UART_RX_GPIO    16
#define RADIO_AVRCP_UART_BAUD       115200

/* On-board addressable RGB LED (WS2812/SK6812), used as a single-pixel
 * audio visualiser - see led_viz.h. GPIO48 is where the ESP32-S3-DevKitC-1
 * v1.0 and most N16R8 clones put it; v1.1 boards moved it to GPIO38, so
 * check the board before assuming. One data line, no other pins involved,
 * and clear of everything else this project claims: the I2S bus (4/5/6),
 * the AVRCP UART RX (16), the reserved in-package flash/PSRAM block
 * (26-37), the strapping pins (0/3/45/46) and native USB (19/20). */
#define RADIO_LED_GPIO              48

/* TuneIn client fields observed from the web player control plane. */
#define RADIO_TUNEIN_PARTNER_ID     "RadioTime"
#define RADIO_TUNEIN_VERSION        "7.38.0"
#define RADIO_TUNEIN_FORMATS        "mp3,aac,ogg,flash,html,hls,wma"

/* This chip runs no Bluetooth stack at all - none of Bluedroid's RAM
 * footprint or SBC-encode buffer churn exists here, so these buffers can be
 * meaningfully larger than the single-chip project's without repeating the
 * "malloc failed"/heap-fragmentation history documented there. Still no
 * PSRAM, so not unbounded.
 *
 * 2026-08-20, MEASURED - and one round of getting this wrong. First attempt
 * raised these to 48K/24K/32K reasoning from "free heap once running
 * (MEM Total) = 114752 bytes, so ~114KB is spare". That was wrong: it spent
 * 64KB and left 49316 bytes, at which point the segment could not be
 * demuxed at all. Real hardware log:
 *   E FMP4_BRIDGE: OOM allocating 751 sample sizes    (only 3004 bytes!)
 *   E Dynamic Impl: alloc(16749 bytes) failed
 *   E esp-tls-mbedtls: SSL - Memory allocation failed
 * ~49KB free was not enough for even a 3KB allocation, because free heap is
 * not the same as contiguous heap.
 *
 * What actually has to fit *transiently*, on top of these steady buffers,
 * every single segment (all measured, not estimated):
 *   moof box buffer .......... ~9 KB  (audio_malloc, per fragment)
 *   trun sample-size table ... ~3 KB  (751 samples x 4, per fragment)
 *   mbedTLS input record ..... ~17 KB (CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN is
 *                                      16384 for this CDN; dynamic-buffer
 *                                      mode allocates it on demand)
 *   -> ~29 KB of churn needing large contiguous blocks, repeatedly.
 *
 * So the working figure to protect is not "some margin" but ~29KB of
 * repeated large allocations plus fragmentation slack. 114752 free was the
 * only value ever observed demuxing successfully, so these stay near the
 * config that produced it, with a single modest increase where it buys the
 * most: HTTP. Do not raise these again without re-reading MEM Total from a
 * fresh log and confirming the fmp4/mbedTLS allocations above still succeed.
 *
 * Depth math (the "256" AAC-LC variant is ~252 kbps = ~32 KB/s compressed,
 * confirmed by ffprobe on a real captured segment; decoded PCM at 48 kHz
 * stereo 16-bit = ~192 KB/s):
 *   HTTP    32KB compressed = ~1.0 s  <- best value per byte: compressed
 *                                        bytes buy ~6x more buffered time
 *                                        than PCM, and this is what absorbs
 *                                        network/TLS/segment-fetch stalls.
 *   fMP4     8KB ADTS       = ~0.25 s (ESP-ADF's own default)
 *   decoder 16KB PCM        = ~0.09 s <- expensive per second; the I2S
 *                                        underrun guard, kept smallest.
 *
 * 2026-08-20 (again): dropped HTTP back 32768 -> 16384. Adding the
 * esp_aac_dec-based decoder element (its own in/pcm buffers plus whatever
 * esp_aac_dec_open() allocates internally) pushed mbedTLS over the edge -
 * "Dynamic Impl: alloc(15399 bytes) failed" / "SSL - Memory allocation
 * failed" - even though ~200KB was free after teardown. The scarce resource
 * is a single CONTIGUOUS ~15KB block for one TLS record, not total free
 * heap. Every KB left unclaimed here is a KB mbedTLS can still find in one
 * piece, so this buffer stays modest until the AAC_DEC heap log (which now
 * prints largest-free-block before/after decoder open) shows real headroom.
 *
 * 2026-08-21: cut again (16/8/16 -> 8/4/8). esp_aac_dec_open() costs a
 * measured 51532 bytes - roughly half of everything free at pipeline start -
 * and after it runs there was not enough left even for RSA-4096 certificate
 * verification: "PK verify failed with error 0x4290", which decodes as
 * MBEDTLS_ERR_RSA_PUBLIC_FAILED (-0x4280) + MBEDTLS_ERR_MPI_ALLOC_FAILED
 * (-0x0010) - an allocation failure inside the bignum code, not a bad
 * certificate. Free heap was 34012 / largest block 29696 at that point.
 * Buffering is worth nothing while nothing plays, so these go small until
 * playback is proven; revisit only afterwards, from a fresh heap log.
 *
 * 2026-08-22 (HTTP-only, +8K, since superseded - see the next comment):
 * raised to 16384 first, reasoning that a bigger look-ahead margin would
 * narrow the ~1-2s live-window-boundary silence (the manifest re-GET +
 * next-segment GET at that boundary being two sequential blocking requests
 * with nothing else to feed the pipeline while they run - see
 * radio_pipeline.c's HTTP_STREAM_FINISH_PLAYLIST handling /
 * http_stream_hook()). Hardware-confirmed it also fixed a separate, real
 * bug - audio speed/pitch instability - at the same time.
 *
 * 2026-08-22 (playlist prefetch): the boundary silence itself needed a
 * different fix in the end - see playlist_prefetch.h for why buffering
 * alone can never fully hide it (a ~100KB buffer would be needed for a ~3s
 * stall, far past what this chip can spare) and what fetches the next
 * window's segment list ahead of time instead. That feature adds its own
 * memory cost on top of everything above, and getting the split between
 * "this buffer" and "that feature's cost" right took two real hardware
 * failures to actually nail down:
 *
 *   1. Left this at 16384, assuming prefetch's own (then-permanent) buffer
 *      was a small enough addition on top. Wrong: first boot after adding
 *      prefetch crashed the pipeline on the very FIRST segment open, exact
 *      "PK verify failed with error 0x4290" RSA-verify-OOM signature this
 *      file already documents above - at 39168 free/38912 largest block,
 *      MORE headroom than the earlier 34012/29696 failure, still not
 *      enough. HTTP(16384) + esp_aac_dec_open()(~51KB) + prefetch's then-
 *      permanent read buffer(8KB, claimed at boot) all landed on the same
 *      tight budget at pipeline start.
 *   2. Cut this to 4096 to compensate. Cleared that crash, but is untested
 *      against the pitch-instability symptom 16384 had fixed, and a
 *      separate bug in the prefetch retry logic itself (fixed since, see
 *      radio_pipeline.c's PLAYLIST_PREFETCH_RETRY_BACKOFF_MS) briefly made
 *      the heap picture look worse than either buffer setting was
 *      responsible for.
 *
 * Landed here instead: back at the proven-stable 8192 (no reason to trade
 * away a fix for a real, separate bug), and playlist_prefetch's read buffer
 * was made fully TRANSIENT instead - allocated only while a prefetch is
 * actually running (at most twice per ~150s window, backoff-limited - see
 * radio_pipeline.c) and freed right after, instead of a permanent claim
 * sitting there for the entire session. That removes prefetch's footprint
 * from the exact moment that crashed (pipeline start), where it now costs
 * nothing at all - a prefetch attempt itself may fail more often on an
 * already-tight mid-session heap (one more malloc alongside its own TLS
 * session), but that fails safe (falls back to the existing reactive
 * refresh), unlike a startup crash. Net standing cost of everything in this
 * file plus prefetch is back to what the original proven-stable 8/4/8 was,
 * before prefetch existed at all.
 *
 * 2026-08-22 (S3/PSRAM migration): everything above this line was measured
 * on the no-PSRAM WROOM-32U and no longer applies to how these buffers are
 * actually placed - see the file-header comment for the audio_calloc()/
 * MALLOC_CAP_SPIRAM mechanism. On the N16R8's 8MB PSRAM pool, three buffers
 * this small are not defending a scarce resource, they are just needlessly
 * narrow: RADIO_HTTP_BUFFER_BYTES in particular is the one lever that
 * directly shortens the live-window-boundary silence gap documented in
 * docs/tunein-hls-gapless-streaming.md (playlist_prefetch hides the
 * cross-host-reconnect stall, but this is what covers ordinary network/TLS
 * jitter within a window) - raised well past the old ~1s look-ahead. Depth
 * math unchanged from above (~32KB/s compressed AAC-LC, ~192KB/s decoded PCM
 * stereo 16-bit @48kHz):
 *   HTTP    256KB compressed = ~8 s   look-ahead, was ~1.0s
 *   fMP4     64KB ADTS       = ~2 s   (was ADF's 8KB default)
 *   decoder  64KB PCM        = ~0.33s (was the I2S underrun guard alone;
 *                                       still deliberately the smallest of
 *                                       the three - it's PCM, the most
 *                                       expensive stage to buffer per
 *                                       second, and the AAC decoder element
 *                                       itself still only ever produces one
 *                                       4096-byte frame at a time - see
 *                                       AAC_DEC_PCM_BYTES in
 *                                       aac_dec_element.c, unchanged)
 * Total ~384KB of PSRAM committed across all three, a rounding error against
 * 8MB and nowhere near enough to threaten esp_aac_dec_open()'s own PSRAM-
 * backed allocation or mbedTLS's. Revisit upward again once a fresh hardware
 * log from the S3 board confirms these are actually being used - this is a
 * generous first cut, not a measured one, precisely because getting it wrong
 * in this direction (too large) just wastes PSRAM instead of crashing the
 * chip, unlike every number in the history above. */
#define RADIO_HTTP_BUFFER_BYTES     (256 * 1024)
#define RADIO_FMP4_BUFFER_BYTES     (64 * 1024)
#define RADIO_DECODER_BUFFER_BYTES  (64 * 1024)

/*
 * TuneIn's signed HLS/CMAF URLs are undocumented and short-lived; the app
 * reacts to their expiry (an HTTP failure surfaces as a pipeline element
 * error), but this bounds worst case exposure with a periodic proactive
 * refresh too, and paces retries after real failures so a bad network/API
 * state doesn't turn into a tight request loop against TuneIn's API.
 */
#define RADIO_SESSION_MAX_MS        (20 * 60 * 1000)  /* proactive session refresh */
#define RADIO_RETRY_BASE_MS         2000              /* first retry delay after a failure */
#define RADIO_RETRY_MAX_MS          (5 * 60 * 1000)   /* retry backoff cap */

/*
 * 2026-09-04: a real failure (station API/stream error, not a user next/prev)
 * now auto-advances through RADIO_STATION_COUNT stations - one bad station
 * (an expired/broken TuneIn entry, a dead stream URL) should not stall
 * playback behind minutes of exponential backoff on that one station when
 * four other perfectly good ones are one hop away. This is the pause
 * between each of those automatic hops - short, since a different endpoint
 * is being tried each time (not hammering the same failing one), but not
 * zero, so a whole round through every station does not turn into a tight
 * loop against TuneIn's API. Once every station has had a turn in one
 * failure streak without one working, main.c falls back to the pre-existing
 * RADIO_RETRY_BASE_MS/_MAX_MS exponential backoff instead of continuing to
 * cycle - at that point the problem is almost certainly broader than any
 * single station (Wi-Fi, TuneIn itself, DNS), and cycling faster only means
 * hammering a broken network harder, not finding a working station sooner.
 */
#define RADIO_STATION_FAILOVER_DELAY_MS  1000

/* RESOURCE HEADROOM LOGGING (2026-08-22): verbose free-RAM/free-flash
 * reporting, added purely to gather real numbers before deciding whether
 * this chip has room for any further feature additions - see main.c's
 * log_ram_usage()/log_flash_usage(). RAM is logged once at boot and then on
 * this interval for as long as the chip runs, since that is the number that
 * actually moves at runtime (HTTP/TLS/decoder buffers coming and going);
 * flash/partition-table/NVS usage is logged once at boot only, since it does
 * not change while running. Runs on esp_timer's own shared timer-service
 * task rather than a new dedicated task/stack, so the logging itself does
 * not eat into the very headroom it exists to measure. */
#define RADIO_RESOURCE_LOG_INTERVAL_MS (30 * 1000)

/* Self-healing safety net, added 2026-09-04 after a hardware log showed
 * mbedtls_ssl_setup failing ("SSL - Memory allocation failed") with
 * DMA-capable free down to ~16.5KB - against a healthy baseline of
 * ~150KB seen in every other session - and staying that low for at
 * least 18 seconds across a successful playback boundary in between,
 * not just a brief spike. DMA-capable/internal RAM (MALLOC_CAP_DMA) is
 * the ~300KB-total pool TLS and Wi-Fi both need slices of to function at
 * all; unlike this board's 8MB of PSRAM (leak-tolerant for a long time -
 * see [[esp32-wifi-streamer-aac-heap-crash]]'s deliberate PSRAM leak on
 * decoder teardown), there is no slack here at all. Whatever the exact
 * source (suspected but NOT confirmed: some of that same deliberately-
 * abandoned decoder memory may not be purely PSRAM - PVMP4AudioDecoder's
 * own allocator is closed-source, so this cannot be verified from
 * project code), a leak in this specific pool has no other recovery path
 * short of a reboot, which reclaims everything unconditionally. This is
 * a mitigation for the SYMPTOM (silent TLS/Wi-Fi failures that never
 * crash outright, so nothing else here would ever notice or recover),
 * not a fix for whatever the underlying source turns out to be. */
#define RADIO_DMA_FREE_CRITICAL_BYTES   (24 * 1024)
/* Consecutive RADIO_RESOURCE_LOG_INTERVAL_MS ticks (so ~90s at the default
 * 30s interval) DMA-capable free must stay under the threshold above
 * before rebooting - gives a genuine brief peak (e.g. two overlapping TLS
 * connections at once: the main stream plus an opportunistic playlist
 * prefetch, both real and expected) a chance to clear on its own first,
 * rather than rebooting on a single momentary dip. */
#define RADIO_DMA_FREE_CRITICAL_STREAK  3

/* I2S output to the Bluetooth chip - this chip is the I2S MASTER (drives
 * BCLK+WS, sends data on DOUT). Must match ../../esp32_bt_speaker's
 * app_config.h pin numbers exactly - not by GPIO NUMBER (the two boards are
 * different chips), but by matching whatever physical wires actually connect
 * them; only this side's numbering had to change here.
 *
 * 2026-08-22 (S3/PSRAM migration): the old 26/25/27 choice was picked for a
 * classic ESP32-WROOM-32U and does NOT carry over. On an ESP32-S3-WROOM-1
 * N16R8, GPIO26-32 are the module's own in-package SPI flash pins and
 * GPIO33-37 are the in-package octal PSRAM pins (the extra 4 data lines +
 * DQS that quad-PSRAM modules don't need) - all reserved by the module
 * itself, not available as GPIO, and wiring anything external to them risks
 * the board not booting at all rather than just a wrong I2S signal. Moved to
 * 4/5/6: comfortably clear of that whole 26-37 block, clear of the strapping
 * pins (0, 3, 45, 46), and clear of GPIO19/20 (native USB D-/D+, used by the
 * board's USB-Serial-JTAG console - claiming those would fight with
 * flashing/logging over the same USB port). No MCLK wire needed for a direct
 * chip-to-chip link. VERIFY THIS against your specific N16R8 module's
 * datasheet before wiring - module vendors occasionally break out a
 * different pin subset, and this was not checked against real hardware
 * (no board in hand yet). */
/* !! THESE THREE ARE NOT WHAT ACTUALLY REACHES THE HARDWARE ON THEIR OWN !!
 * (2026-08-31, found by disassembling the built .elf, not by reading source)
 *
 * radio_pipeline.c does set i2s_cfg.std_cfg.gpio_cfg from these - and ESP-ADF
 * then silently discards them. On IDF >= 5.0 the audio_stream component
 * compiles i2s_stream_idf5.c (not i2s_stream.c), whose i2s_driver_startup()
 * calls the SELECTED BOARD's get_i2s_pins() and memcpy()s that straight over
 * tx_std_cfg.gpio_cfg, unconditionally, immediately before
 * i2s_channel_init_std_mode(). There is no public ADF API to override it or
 * to reach the i2s_chan_handle_t afterwards.
 *
 * So the pins are decided in TWO places and both must agree:
 *   1. here (what radio_pipeline.c asks for, and what humans read), and
 *   2. esp-adf/components/audio_board/m5stack_atoms3r/board_pins_config.c's
 *      get_i2s_pins() - the one that actually wins. It carries a LOCAL EDIT
 *      repointing it from real M5Stack AtomS3R pins (8/6/5) to these.
 * Change one without the other and the firmware quietly drives the wrong
 * GPIOs with no error at boot, which is exactly what it did until now: the
 * flashed binary was driving BCLK=8, WS=6, DOUT=5 - note 5 and 6 present but
 * in SWAPPED roles, so a scope probe looks almost-right and misleads.
 *
 * Verify after any change to either file, on the built ELF, rather than
 * trusting this comment:
 *   xtensa-esp32s3-elf-objdump -d --disassemble=get_i2s_pins \
 *       build/esp32_wifi_streamer.elf
 * and read the movi.n constants stored at struct offsets +4/+8/+12
 * (bck/ws/data_out). */
#define RADIO_I2S_BCLK_GPIO         4
#define RADIO_I2S_WS_GPIO           5
#define RADIO_I2S_DATA_GPIO         6
#define RADIO_I2S_SAMPLE_RATE       44100

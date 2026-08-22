#pragma once

/*
 * User configuration for the Wi-Fi/streaming chip - one half of the
 * two-ESP32 split of esp32_tunein_bt_radio. This chip does Wi-Fi + TuneIn
 * control-plane + HLS/fMP4/AAC decode, then hands decoded PCM to the other
 * chip (esp32_bt_speaker) over I2S. It has no Bluetooth stack at all - see
 * ../../esp32_bt_speaker for that half.
 *
 * Do not paste serial logs containing signed TuneIn URLs into public issues.
 */
#define RADIO_WIFI_SSID             "Ashish"
#define RADIO_WIFI_PASSWORD         "kgf@4646"
#define RADIO_TUNEIN_STATION_ID     "s345724" /* Apple Music Hits */

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
 * playback is proven; revisit only afterwards, from a fresh heap log. */
#define RADIO_HTTP_BUFFER_BYTES     8192
#define RADIO_FMP4_BUFFER_BYTES     4096
#define RADIO_DECODER_BUFFER_BYTES  8192

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

/* I2S output to the Bluetooth chip - this chip is the I2S MASTER (drives
 * BCLK+WS, sends data on DOUT). Must match ../../esp32_bt_speaker's
 * app_config.h pin numbers exactly, and the physical wiring between the two
 * boards. See the dual-ESP32-i2s-split-idea project memory for the
 * reasoning behind these specific pin choices (avoids strapping pins 0/2/5/
 * 12/15, the flash-connected pins 6-11, UART0 1/3, and the input-only pins
 * 34-39). No MCLK wire needed for a direct ESP32-to-ESP32 link. */
#define RADIO_I2S_BCLK_GPIO         26
#define RADIO_I2S_WS_GPIO           25
#define RADIO_I2S_DATA_GPIO         27
#define RADIO_I2S_SAMPLE_RATE       44100

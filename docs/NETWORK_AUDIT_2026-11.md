# Local network-stack audit — November 2026

The November 7 `r7`/`r8`/`r9` findings below retain their original buffer sizes and test scope as a historical engineering log. See the [2026-11-13 v1.1.0 resolution](#2026-11-13-resolution--v110-http--airplay-2-homepod-qualification) at the end of this document for the root causes isolated across the lwIP HTTP client, Internal DMA SRAM / PSRAM Wi-Fi TX staging queues, and RTP `PT=0xD4` NTP clock synchronization—along with the passing Workshop HomePod (`AudioAccessory5,1`) qualification metrics.

## Confirmed findings

The earlier data did **not** establish an over-the-air packet-loss rate.
`sendto()` was already reporting local `ENOMEM`, but the original packet
counter incremented and the method returned success after exhausted retries.
Repair sends also incremented their counter without checking the result.
Those attempts could be rejected before the driver accepted the frame.

The r7 live test isolated discovery from the audio interval. A requested
speaker scan stayed deferred, the speaker-scan count remained 1, no speaker
or off-channel Wi-Fi scan was active, and outgoing UDP-5353 submissions stayed
at 7 throughout the 34-second observation. Nevertheless, the last sample showed:

| Observation | Value | Meaning |
| --- | ---: | --- |
| Original RTP packets attempted | 4,123 | New audio packets constructed |
| Originals accepted by socket API | 3,816 | Local submission succeeded |
| Originals rejected after bounded retries | 307 | Local submission failed |
| Additional original-send retries | 1,320 | Repeated local attempts, not unique packets |
| Accepted repair submissions | 393 | Socket accepted the cached repair packet |
| Rejected repair submissions | 7 | Repair retained for a later paced attempt |
| All driver submissions since boot | 6,920 | Includes TCP, timing, mDNS, audio and retries |
| Driver-accepted submissions | 5,204 | Driver accepted the frame |
| Driver-rejected submissions | 1,716 | SDK returned an error before acceptance |
| Last driver error | 257 | `ESP_ERR_NO_MEM`; socket error was `ENOMEM` (12) |

These totals belong to different layers and include different traffic. They
must not be divided into a receiver loss percentage. Acceptance is not proof
of radio transmission, MAC acknowledgment, or speaker playback. The remaining
rejections occur without active discovery, so scans are not their sole cause.
Driver-buffer limits, allocation constraints and radio queue drain remain
unisolated; AP/RF faults have not been established.

From the 8.5-second sample onward, the source PCM buffer stayed at
11.860–11.882 seconds, with roughly 100 KB of compressed input retained and no underruns.
This is an improvement over the earlier rapid depletion after repair bursts
were bounded. It is a short software-path result, not general AirPlay or
acoustic qualification. Native Sonos playback was restored afterward.

## Changes

- Original sends report accepted/rejected/retry totals and return failure on
  rejection. The legacy `raop_pkts_sent` API field remains an attempt counter;
  the portal and knob instead show queued/accepted originals and local rejects.
- Repairs remain pending in the existing bounded packet cache. One repair is
  allowed per 8 ms, after original audio has had its turn, with a 20 ms hold
  after local rejection. A failed repair is neither counted as accepted nor
  suppressed as if successfully sent. Receive work is bounded per turn.
- Explicit and retry-triggered speaker scans, plus off-channel Wi-Fi scans,
  wait during controller audio playback. Initial discovery and missing-IP
  resolution still work; native Sonos-only playback can process queued scans.
  Active/connecting AirPlay and requested transport take precedence over stale
  native-connected telemetry. The r6 transition-race test failed and prompted
  this r7 correction; that failed observation is not presented as a pass.
- Count-only wrappers at both SDK transmit entry points expose driver acceptance
  and rejection. Frames are classified before submission, without retaining
  payloads. UDP destination port 5353 counts include hostname/service traffic
  as well as speaker discovery, so a scan counter is also exposed.

The pinned SDK uses `esp_wifi_internal_tx_by_ref` when
`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` is enabled; both that entry point and
`esp_wifi_internal_tx` are observed. See Espressif's
[network-interface implementation](https://github.com/espressif/esp-idf/blob/v5.5.4/components/esp_wifi/src/wifi_netif.c)
and [buffer-path documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-guides/wifi.html#wi-fi-buffer-configure).

## Artwork and shared resources

Artwork had been disabled while playing, station downloads were marked
complete before fetching, and local-PCM guards blocked native Sonos mode
because its local buffer is intentionally zero. The restored worker uses
native-aware eligibility, bounded retries, current-track checks, station
fallbacks, HTTP transfer decoding and baseline 160-pixel album JPEGs.

The SDK's `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC` routes TLS buffers to internal
RAM. Bulk allocations made specifically by the artwork task now use PSRAM;
an external-allocation failure cannot fall back into scarce network-driver
RAM. Other tasks' TLS and AirPlay pairing retain the existing allocator.
The SDK free hook supports either heap, as shown in Espressif's
[memory-port source](https://github.com/espressif/esp-idf/blob/v5.5.4/components/mbedtls/port/esp_mem.c).
In r8, art jobs require internal headroom, wait for a six-second source cushion
when the knob carries audio, and abort data writes below three seconds or
during scan/OTA/reconfiguration work. Native Sonos-only playback has no local
PCM requirement. Lower-priority artwork work does not hold the app-state mutex
across HTTP requests.

The device served an actual decoded album JPEG, returned HTTP 200 / decoder
OK, and reported external TLS allocations. Initial native artwork work had no
driver rejections and an internal-heap minimum of 42,108 bytes. The user
confirmed the cover is visible on the physical screen. Some optional requests
can be cancelled during handoff and retried; `artwork_failures` includes those
aborted jobs as well as network failures.

## Limits and next diagnostic

The Wi-Fi AP-selection/PHY configuration was not changed. TX remains bounded
at 16 dynamic buffers plus 8 cache buffers; static/dynamic RX remain 10/32.
The precompiled SDK has a six-datagram UDP receive mailbox and a 5,744-byte
default TCP send buffer. Increasing these blindly would trade memory for
buffering without isolating why the driver rejects submissions.

Control-channel writes, metadata and artwork forwarding still occur in the
sender task and may block on TCP writes; they are asynchronous with respect
to replies, not guaranteed nonblocking IO. Their timing and the driver's
buffer eligibility/queue drain are remaining software diagnostics before an
AP/RF conclusion. Broader receiver qualification remains open in
[issue #2](https://github.com/amcchord/AirPlayKnob/issues/2), at the user's
request. The working output remains native Sonos radio.

Run `python3 tests/test_network_policy.py` for host checks of gating,
contradictory transition states, truncated-frame parsing, local accounting,
task-specific allocation/overflow/OOM behavior, and the actual firmware repair
methods under burst requests and failed sends. Existing ALAC/RTSP checks and
credential-free firmware builds also run in GitHub Actions. Private live
observations and exact image/source manifests remain in ignored firmware
artifact directories.

## r9 HomePod follow-up — 512 KiB PCM

At the user's request, source `7e4158e` reduces PCM from 2 MiB to 512 KiB
(2,972 ms usable). MP3 remains 256 KiB. Playback/rebuffering requires 1 second
PCM with 16 KiB MP3 available, otherwise 1.5 seconds. Artwork starts at
2 seconds PCM plus 32 KiB MP3 and aborts payload writes below 1 second PCM.
Capacity labels and API diagnostics derive from the same configuration.
The ring is still PSRAM: this does not directly increase internal/DMA memory.

Deployed `pcm512-20261107-r9` over OTA into app0, preserving the user's
Workshop HomePod (AudioAccessory5,1), preset 19 and volume 24. A real
300.13-second monitor collected 126 successful snapshots. The first-to-last
samples span 293.478 device seconds; these are the exact interval deltas:

| Observation | Value |
| --- | ---: |
| New source underruns | 26 |
| Additional locally rejected original submissions | 5,908 |
| Additional driver rejections, all traffic/retries | 31,998 |
| Observed PCM range | 1–2,968 ms |
| Minimum compressed-input reserve | 0 bytes |
| Maximum original-audio send gap since connection | 12,984 ms |
| Maximum measured RTSP write since connection | 1 ms |
| Speaker scan count, start/end | 1 / 1 |
| Active speaker/Wi-Fi scan samples | 0 |
| Observed device/session resets or receiver changes | 0 |

The final underrun counter was 28 relative to an earlier post-boot checkpoint;
that checkpoint predates the timed monitor, so its deltas are not the exact
five-minute-window counts. One status request timed out. Album kind 2 was
observed and decoder error stayed zero. The user confirmed choppy/intermittent
audio. **The sustained-playback test failed; the smaller ring did not resolve
the stalls.** Both compressed input and PCM deplete, so the result does not
isolate MP3 decoder throughput as the cause. The RTSP write metric covers
writes only, not every sender operation or wait.

The reboot selected a different AP on the same channel. No AP/PHY policy was
changed, but that prevents a controlled buffer-size A/B conclusion. The local
rejection counts still cannot be treated as over-the-air loss rates. Further
diagnostics should hold AP/workload constant and distinguish source-input
starvation, sender scheduling, driver allocation eligibility and queue drain.
HomePod remains selected with the requested smaller buffer; native Sonos is
the previously verified fallback.

Host regressions and the credential-free firmware build passed, including an
eight-minute **simulated** actual-ring wraparound/stereo/saturation check;
GitHub validation passed for deployed source. These checks do not override
the failed live audio result. The exact image/ELF/source manifest and raw
private JSON/CSV are retained in ignored
`artifacts/firmware/pcm512-2026-11-07-r9/`.

## Unmodified original GitHub main HomePod comparison

The user then requested flashing the original GitHub version. Selected
pre-troubleshooting main `416c2ad`, rebuilt in `worktrees/og-main` without
source or configuration changes, credentials or added instrumentation.
Installed dependencies resolved to the same platform 55.03.38, Arduino 3.3.8 /
IDF 5.5.4 and LVGL 8.4.0 as r9. This is a rebuild of committed source, not
an identified historical listening-session binary. Original source uses
2 MiB PCM and 64 KiB MP3. Web OTA returned HTTP 200 / OK; the expected legacy
API appeared and connected Workshop HomePod, preserving preset 19 / volume 24.

**The original main version also failed sustained playback.** The real monitor
ran 307.24 seconds, collecting 70 successful snapshots spanning 297.462 seconds
and 27 failed requests. User confirmed choppy/intermittent audio.

| Observation | Original main result |
| --- | ---: |
| Observed PCM range | 0–10,115 ms |
| Observed MP3 range | 0–630 bytes |
| Observed positive underrun increments | At least 11 |
| Underrun-counter decreases | 1 |
| Packet-counter decreases | 1 |
| Disconnect / reconnect observed | 52 s / 71 s |
| Receiver changes | 0 |
| Wi-Fi RSSI range | -88 to -71 dBm |

The underrun endpoint difference was 6, but that omits events before the
counter decrease. The sum of observed positive increments is 11; failed
status requests make this a lower bound. Legacy API has no uptime/reset
reason, so these counter changes do not establish a hardware reboot. Its
packet/repair counts are attempts, not delivery or driver acceptance.

The original source can reproduce struggling HomePod playback without the
recent changes. This does not identify a cause or explain earlier Tree House
success: original-run signal was weaker than r9's -69 to -61 dBm, and no BSSID
field exists to verify AP identity. Buffer sizes, source behavior and network
conditions were not isolated variables. The old November 3 encoder regression
also remains present in this intentionally unmodified baseline.

Original firmware was left installed at the end of this comparison. The user
subsequently requested smart Sonos again; r9 was restored via USB into app1
and Workshop native playback was verified. See current state/runbook for the
latest deployment; this historical test remains a failed AirPlay result. Exact original source/dependency manifest,
image/ELF, build log and private JSON/CSV/assessment are in ignored
`artifacts/firmware/og-main-2026-11-07/`. Original image SHA-256:
`c9fb563e92e8c5a43f4ec37420f821a7e84988a09667bd07a1007366d9fa2a6d`.
The original API also lacks build/partition reporting: app1 is the expected
OTA destination from r9/app0, not an independently reported active slot.

---

## 2026-11-13 resolution — v1.1.0 HTTP & AirPlay 2 HomePod qualification

On November 13, 2026, source-level instrumentation isolated two distinct network/audio bottlenecks behind both local 3.5mm HTTP buffer starvation and HomePod AirPlay 2 dropouts, and resolved them in **v1.1.0**:

### 1. Root Causes Isolated & Fixed
1. **HTTP TCP Receive Window, SACK Reneging & Multi-AP BSSID Stall Failover (`IcyRawTcp`)**:
   - Arduino `WiFiClient` small read windows and SACK reneging across multi-AP Wi-Fi links caused periodic multi-second TCP stalls even when playing locally to the 3.5mm DAC without AirPlay TX load.
   - Replaced the stream reader in `src/audio/radio_streamer.cpp` with a custom raw lwIP TCP client (`IcyRawTcp`) running on `tcpip_thread` with a **`16 × MSS` (`22,976 B`) receive window**, `TF_SACK` explicitly cleared on PCB connect, immediate `tcp_output()` ACKs after `tcp_recved()`, cached DNS lookups, and automatic multi-AP BSSID stall scoring/failover (`pref_bssid` saved to NVS after 45 seconds of clean streaming).
2. **Internal DMA SRAM Headroom & PSRAM `cache_tx_buf` Wi-Fi Staging (`__wrap_esp_wifi_init`)**:
   - Because ESP32-S3 builds with `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`, outbound lwIP `PBUF_RAM` RTP datagrams reside in Octal PSRAM and are submitted via `esp_wifi_internal_tx_by_ref()`, which requires free **PSRAM `cache_tx_buf`** slots (`default = 8–32`) in addition to Internal DMA SRAM `dynamic_tx_buf` slots (`default = 16–32`). During transient 2.4 GHz MAC contention (`125.35 pkt/s` audio + HTTP ACKs), exhausting `cache_tx_buf` immediately triggered `ESP_ERR_NO_MEM` (`257` / `errno = ENOMEM`).
   - Reclaimed **~92 KB free Internal SRAM** (`~81.9 KB` largest contiguous DMA block) by moving `radio_http`, `raop_tx`, `raop_ntp`, and `art_fetch` task stacks, `mp3dec_t`, and `RaopClient` into PSRAM and tuning LVGL's ping-pong DMA strips to `16` lines (`23.0 KB`).
   - Configured `__wrap_esp_wifi_init()` (`src/main.cpp`, verified at boot via `custom_cfg_err=0`) with `static_rx_buf_num = 16`, `dynamic_rx_buf_num = 64`, `rx_ba_win = 12`, **`dynamic_tx_buf_num = 48`**, and **`cache_tx_buf_num = 64`** (`~500 ms` of PSRAM-to-MAC TX staging at `0 B` Internal SRAM cost), alongside **802.11b/g/n HT20** A-MPDU aggregation and `IP_TOS = 0xA0` (`DSCP CS5` -> `AC_VI` across both ESP-IDF and downstream RFC 8325 / enterprise APs).
3. **Monotonic Wall-Clock `PT=0xD4` RTP Sync Reference (`RaopClient::sendSyncPacket`)**:
   - Even after eliminating driver drops and adding lossless `flushUnsentPacket()` (`750 ms` local hold, `4 ms` probe) plus `4.0 ms` NACK repair (`2.5 ms` post-audio guard), `sendSyncPacket()` had been stamping `sync_rtp_ts = m_head_ts` against the live wall-clock `sync_ntp`. Whenever `m_head_ts` briefly lagged wall clock during Wi-Fi TX queuing (`e.g., 150–250 ms`) and then gently caught up (`6.0 ms` / `7.5 ms` pacing), `PT=0xD4` told HomePod to shift its playout clock backward by `-250 ms` and then snap forward `+250 ms` one second later—producing audible resync dropouts despite zero lost RTP sequence numbers.
   - Changed `sendSyncPacket()` (`src/audio/raop_AnySpeaker.cpp`) to compute `sync_rtp_ts = (uint32_t)ntp_to_ts(sync_ntp, 44100)` directly from monotonic wall-clock `sync_ntp` (`rtp_ts_latency = sync_rtp_ts - m_latency_frames`, `88,200` frames / `2.0 s` on AirPlay 2), keeping the RTP-to-NTP slope locked at `44,100 Hz`.
4. **Lock-Free `20 ms` `raop_tx` State Snapshot & Throttled Link Polling**:
   - Cached `g_app` playback/routing state every `20 ms` in `raop_sender_task` (with immediate atomic wakeups on user input) instead of taking `g_app.mutex` at `2,000 Hz`, and throttled `esp_wifi_sta_get_ap_info()` / negotiated PHY queries in `loop()` to `3 s`.

### 2. Verified Workshop HomePod (`AudioAccessory5,1`) Live Qualification
Measured on the battery-equipped ESP32-S3 Knob streaming SomaFM (*Groove Salad*) over AirPlay 2 to **Workshop (HomePod)** (`AudioAccessory5,1`):

| Metric | Nov 7 `r9` Baseline | Nov 13 `v1.1.0` Result |
| :--- | ---: | ---: |
| **Original RTP packets accepted / attempted** | `5,908` locally rejected | **`27,605 / 27,605` (`0` rejected)** |
| **Original RTP local send retries** | `>1,300+` | **`0`** |
| **All-traffic Wi-Fi driver TX accepted / submitted** | `31,998` driver rejects | **`30,923 / 30,923` (`0` rejected)** |
| **Audio send gap events (`> 50 ms`)** | Repeated stalls | **`0` events** |
| **Maximum original-audio send gap** | `12,984 ms` | **`16 ms`** |
| **Source stream underruns** | `26` | **`0`** |
| **Steady-state PCM / MP3 reserves** | `1–2,968 ms` / `0 B` min | **`2,955 ms` / `252,613 B`** |
| **Free Internal SRAM / Largest DMA block** | `~42 KB` min | **`91,648 B` / `81,908 B`** |

Following telemetry verification, the user ran continuous multi-hour listening on the Workshop HomePod and confirmed clean, uninterrupted playback.

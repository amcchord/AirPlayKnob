# Local network-stack audit — November 7, 2026

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

The source PCM buffer stayed at about 11.86–11.89 seconds after the initial
sample, with roughly 100 KB of compressed input retained and no underruns.
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
Art jobs require internal headroom, wait for a six-second source cushion
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

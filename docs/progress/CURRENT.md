# Current state — 2026-11-07

Device now runs the **unmodified original GitHub main source `416c2ad`**,
rebuilt and flashed at the user's request for a HomePod comparison. The
contained worktree is `worktrees/og-main`, branch `codex/test-og-homepod`;
root `codex/workshop-audio` remains the integration checkout. Original source
and platform configuration are clean. Installed dependencies resolved to
platform 55.03.38, Arduino 3.3.8 / IDF 5.5.4 and LVGL 8.4.0, matching r9.
This is a rebuild of the committed source, not an identified historical binary.

OTA returned HTTP 200 / OK and the expected legacy API appeared, connected
to **Workshop (HomePod)** with preset 19 / volume 24, local DAC and WhiteCap
off. Legacy API has no build ID, active slot, uptime/reset reason, BSSID or
local-send acceptance counters. app0/r9 was active before upload, making
app1 the expected OTA target; that slot is not independently API-confirmed.
Original source allocates 2 MiB PCM and 64 KiB MP3.

The real **307.24-second HomePod test failed audibly**. User confirmed
choppy/intermittent audio. Seventy successful snapshots span 297.462 seconds;
27 status requests failed. Both buffers reached zero; sampled PCM peaked at
10,115 ms and MP3 at 630 bytes. At least 11 underrun increments were observed.
The underrun counter dropped once, so its endpoint difference of 6 undercounts
observed events. A packet-counter decrease and disconnect were observed at
52 seconds, with reconnection at 71 seconds. Legacy telemetry cannot establish
whether a hardware reboot occurred. Receiver selection stayed HomePod.

The original source also struggles under current conditions; recent changes
are not required to reproduce it. This does not identify the cause or prove
what changed since earlier Tree House listening. Wi-Fi RSSI was -88 to -71 dBm,
versus -69 to -61 during r9. Original API lacks BSSID, so AP identity could not
be verified. This is not a controlled AP/workload A/B. Old packet counters
measure attempts, not delivery, and cannot replace r9 acceptance telemetry.

Original firmware SHA-256:
`c9fb563e92e8c5a43f4ec37420f821a7e84988a09667bd07a1007366d9fa2a6d`.
Exact image/ELF, source/dependency manifest, build log and private JSON/CSV
plus bounded assessment: `artifacts/firmware/og-main-2026-11-07/` (ignored).
The original source built successfully without credentials or instrumentation.
No source changes were made and no modern regression suite was imposed on
the intentionally historical source. This live test is a failed qualification.

Retained r9 restoration image:
`artifacts/firmware/pcm512-2026-11-07-r9/firmware.bin`, source `7e4158e`,
SHA-256 `215b77a783a4d05b01dbaf7deba78f65cb7f998930a6667e09ff7af06177d7b1`.
That version has 512 KiB PCM / 256 KiB MP3, revised startup/artwork thresholds
and local-send diagnostics. Its five-minute HomePod run also failed, despite
passing host/build checks. r8/native Sonos fallback and original flash backups
remain retained. The original version remains installed for user comparison;
its artwork/UI/network fixes from the current branch are absent.

Changes/results are recorded in [PR #1](https://github.com/amcchord/AirPlayKnob/pull/1)
and [issue #2](https://github.com/amcchord/AirPlayKnob/issues/2); main is unmerged.
See the [network audit](../NETWORK_AUDIT_2026-11.md) and
[historical audit](../AIRPLAY_REGRESSION_2026-11.md).

Next safe action: preserve this baseline pending user direction. For another
comparison, hold AP/workload constant and isolate input starvation, sender
scheduling and driver allocation/queue drain. Restore r9 through the same OTA
procedure when desired; check active OTA metadata before any USB flash.

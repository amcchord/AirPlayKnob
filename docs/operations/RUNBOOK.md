# Device diagnostics and deployment

Updated on 2026-11-13 for **AirPlayKnob v1.1.0**. Read
[the current state](../progress/CURRENT.md) before deploying again.

## Build and verify

```sh
/Users/austinmcchord/.platformio/penv/bin/pio run
/Users/austinmcchord/.platformio/penv/bin/python tests/test_alac_packets.py
/Users/austinmcchord/.platformio/penv/bin/python tests/test_rtsp_responses.py
/Users/austinmcchord/.platformio/penv/bin/python tests/test_network_policy.py
/Users/austinmcchord/.platformio/penv/bin/python tests/test_pcm_buffer.py
/Users/austinmcchord/.platformio/penv/bin/python tests/test_battery_monitor.py
git diff --check
```

The decoder regression check requires FFmpeg and a host C++ compiler.
The RTSP check runs 16 cases against the actual firmware transaction method.
The battery monitor suite validates LiPo SoC curve interpolation, hysteresis, CC/CV charge-lift compensation, USB host SOF vs. wall-brick slope/step detection, full-charge termination, and Home screen (`Tab 0`) visibility rules.
GitHub Actions runs all five host regression suites plus a clean firmware build without private credentials on pushes, PRs, and `v*` release tags.
Build dependencies are pinned in `platformio.ini`.
`STATUS\n` and `BATTERY\n` over the USB serial port at 115200 baud report the active speaker,
battery/charge telemetry, packet and retransmission counters, PCM/MP3 cushions, underruns, and available heap.
Note: opening `/dev/cu.usbmodem*` with default DTR/RTS signaling on ESP32-S3 `USB_SERIAL_JTAG` can trigger a hardware reset; prefer HTTP `GET /api/status` and `POST /api/control` (`{"preset": 0}` or `{"toggle_play": true}`) during uninterrupted streaming soaks.
The `RAOP-1S` diagnostics distinguish accepted sends, retries, and drops;
`Pkts` counts attempted original packets, and is not proof of delivery.

## USB deployment

Confirm the attached port before using these commands. This session used
`/dev/cu.usbmodem14201`, with app0 active at `0x10000` and OTA sequence 1.
Close serial monitors first. Inspect OTA metadata before assuming app0 is
active after a future wireless update.

```sh
/Users/austinmcchord/.platformio/penv/bin/python -m esptool --chip esp32s3 --port /dev/cu.usbmodem14201 --baud 115200 --no-stub read-flash 0xe000 0x2000 scratch/otadata.bin
/Users/austinmcchord/.platformio/penv/bin/python -m esptool --chip esp32s3 --port /dev/cu.usbmodem14201 --baud 921600 --no-stub write-flash 0x10000 .pio/build/waveshare_knob_s3/firmware.bin
```

Use `--no-stub`: the stub flasher stopped reading after its first 4096 bytes
on this USB-Serial/JTAG connection; ROM-mode reads and writes completed and
writes passed flash-hash verification. Writing only the active application
preserves NVS, bootloader, partition table, and OTA selection. Do not erase flash.

The saved pre-fix application is
`scratch/workshop-fix/device-app0-backup.bin`. Its application prefix exactly
matched the preexisting build. Its full SHA-256 is
`3cf90844bf81654e862e5ed9366e7d54caa5437b249b0dd0ba85a9f3c17fa449`.
To restore this session's app0 image, use the same `write-flash` command with
that backup as the input, after confirming the active OTA slot. The flash
method was exercised; restoring the old firmware was not necessary.

After deployment, verify the flash hash, fresh boot, selected speaker,
AirPlay pairing and timing replies, a sustained PCM cushion, and no new
underruns. Verify audible playback with someone near the speaker. A connected
RTSP session alone is insufficient.

The web API currently supports `{"toggle_play":true}`; its documented `play`
field is not handled. Read `/api/status` before toggling. Pause/resume should
show `playing=false` and `raop_connected=false`, followed by a fresh handshake
and normal buffered playback. This session verified that sequence.

## Sonos-native playback (2026-11-07)

Current firmware and source provenance are recorded in the current-state file
and `artifacts/firmware/workshop-2026-11-07/manifest.json`. Backups from the
original session are retained; the rollback procedure above still applies.

For Sonos-only listening, use `also_local_dac=false` and close WhiteCap.
`sonos_native=true`, `local_monitor=false`, and advancing receiver metadata
indicate the direct path. The speaker owns audio buffering; local PCM/MP3
counters intentionally stay zero. The UI should say **Audio buffered by Sonos**.
Opening WhiteCap or enabling the local jack starts a local monitor, whose
buffers do not control the Sonos stream.

Read `/api/status` for build, uptime, reset reason, heap, UI heap, selected
output, local-monitor status, and track metadata. Confirm the receiver's
`AVTransport` state and URI as well. Station/play commands are asynchronous:
wait for the requested URI and PLAYING state instead of assuming completion
when `/api/control` returns. Volume can be checked with `RenderingControl`.
A controller network timeout alone should not stop a speaker-owned stream.

The final live test exercised volume, pause/resume, two stations, 54 screen
changes, visualizer monitoring, and a controller reboot. The receiver stayed
PLAYING across the reboot and the controller restored its selected station.
Normal non-Sonos AirPlay output and physical headphone output were not newly
validated against additional hardware in this session.

## Explicit AirPlay selection

The portal's **Use direct radio playback on Sonos** checkbox persists across
restarts. To select AirPlay even for a Sonos receiver, POST
`{"prefer_sonos_native":false}` to `/api/control`. Restore it with `true`.
Check both `prefer_sonos_native` (preference) and `sonos_native` (active path)
in `/api/status`; the transition can take several seconds. Turning native mode
off stops the speaker-owned stream and starts the normal AirPlay connection.
Retain the known-working native firmware/settings while diagnosing the relay.

See the [historical regression audit](../AIRPLAY_REGRESSION_2026-11.md) for
confirmed defects, unresolved Wi-Fi trouble, and the receiver test matrix.


## Retained r5 image

Source commit `1e692db`, build `airplay-compat-20261107-r5`, is retained in
`artifacts/firmware/airplay-compat-2026-11-07-r5/` with the image/ELF, source
hash manifest and integration-check results. The exact firmware SHA-256 is
`51529abf8b1258eac664a34856fff160003ac2d3abbbc281de57227d4b91a60f`.
The previous working native r2 image remains in
`artifacts/firmware/workshop-2026-11-07/firmware.bin` for rollback. Neither
artifact directory is tracked or published. Follow the app0/OTA checks above
before using any retained image.

The active output and intended native route are distinct: `sonos_native`
means connected playback; `sonos_radio_selected` also covers connecting and
recovery. Without local monitoring, the decoder stops its existing download
as soon as the native route is selected. r5 verified AirPlay-to-native handoff
in 4.48 seconds; standard AirPlay remains subject to the failure in issue #2.

For serial diagnostics, send a leading newline before `STATUS` or `REBOOT`
and flush the host writer, then verify the response. Never count an issued
command as a completed restart: check the acknowledged reboot, uptime decrease
and reset reason. The final r5 software reboot reported reset reason 3 while
Sonos stayed PLAYING across all 40 receiver-state observations.


## Artwork and local-stack diagnostics (r8 procedure)

Read `docs/progress/CURRENT.md` for the latest image/hash. The r8 update was
installed using the existing `/api/ota` multipart endpoint and ran in app1.
**The earlier app0 USB address is no longer the active-slot assumption.**
Check `running_partition` in `/api/status` and inspect OTA metadata before
any future direct flash. The updater chooses the inactive OTA partition;
NVS and the previous slot are retained. Kept images can be restored through
the same OTA upload procedure or through USB after verifying the active slot.

The validated web upload is `POST /api/ota`, multipart form field `firmware`,
filename `firmware.bin`, content type `application/octet-stream`. Confirm HTTP
200 / `OK`, then wait for the expected build, a fresh uptime, running partition,
saved preferences and actual receiver playback. Exact uploaded image hashes,
preflight/boot snapshots and final checks are retained in ignored artifacts.
An accepted upload is not a substitute for post-boot checks.

`/api/artwork.jpg` returns the current decoded/published cover JPEG. It returns
404 while no downloaded cover is available. `artwork_kind` is 0 for generated
fallback, 1 for station art and 2 for album art. Check JPEG/pixel sequences,
HTTP status, decoder error (0 means OK), attempts/failures and task-specific
external TLS allocation count. Optional jobs can abort and retry during busy
network work; a failed/cancelled job is not itself an audio underrun.

During controller AirPlay, speaker/Wi-Fi scan requests are queued. Read
`speaker_scan_deferred`, `wifi_scan_deferred`, scan count and active flags.
Pause playback to run queued scans; native Sonos-only mode can also process
them safely. Initial/missing-IP discovery remains available.

Use `raop_tx_accepted`, `raop_tx_rejected`, `raop_tx_retries`,
`raop_repair_rejected` and pending repairs alongside `driver_tx_*` and scan
telemetry. `raop_pkts_sent` remains a legacy original-attempt counter. The
knob and portal now show accepted/queued originals and local rejects. Driver
success is queue acceptance, not RF/receiver delivery. See the
[network audit](../NETWORK_AUDIT_2026-11.md) for tested values and limits.

## r9 PCM configuration and sustained checks

`pcm512-20261107-r9`, source `7e4158e`, was deployed through the validated OTA
upload above and ran in **app0** before the original-main comparison below. The previous r8 image is retained at
`artifacts/firmware/art-network-2026-11-07-r8/firmware.bin`. Restoring through
OTA uses the same multipart procedure; verify the resulting build, partition
and saved output. Do not use an old USB address without checking the slot.

Check `/api/status`: `pcm_capacity_bytes=524288`, `pcm_capacity_ms=2972`,
`mp3_capacity_bytes=262144`, `pcm_start_ms=1000`,
`artwork_start_pcm_ms=2000`, `artwork_start_mp3_bytes=32768`. Rebuffering uses
1.5 seconds instead of 1 second when compressed reserve is below 16 KiB.
Artwork payload writes yield below 1 second PCM; native-only Sonos still
does not require local PCM. Capacity is allocated in PSRAM.

For a live monitor, obtain a **fresh** connected-status baseline at the start
of the timed interval. Sample for at least five minutes and retain uptime,
selected receiver, PCM/MP3, underruns, socket/driver rejection, active/deferred
scans, artwork state and `raop_max_audio_gap_ms` /
`raop_max_control_write_ms`. Compare the first and last device uptime as well
as wall time; a saved boot checkpoint may predate monitoring. Maximum timing
fields cover the connection lifetime and are not necessarily window deltas.
Preserve selection/preset/volume and confirm audible playback with the user.
Record the AP before and after deployment to identify A/B confounders.

The r9 five-minute HomePod run failed audibly and recorded repeated source
underruns and local send rejections. Album art loaded and no reset was
observed. Exact bounded results are in the network audit and current-state
file; a successful build or connected session is not a sustained-audio pass.

## Original-main comparison and restoration

The user subsequently requested original GitHub main `416c2ad`. It was built
unchanged in `worktrees/og-main` and uploaded using the same multipart OTA
procedure. Accepted upload and transition from r9 to the expected legacy API
were observed, with the HomePod/preset/volume preserved. app1 is the expected
target after r9/app0, but original API lacks build, uptime and active-slot
fields. Inspect OTA metadata before a direct USB flash; do not infer the slot
from these historical commands.

At the end of that comparison, the original version was left installed. Its five-minute HomePod test
also failed audibly, with empty buffers and reconnect behavior. Original
packet counters do not measure accepted sends, and counter decreases prevent
simple endpoint subtraction for underruns. Sum observed positive increments
and report counter resets/missing samples explicitly. Original has no BSSID
reporting; AP identity and hardware-reset absence were not established.

To restore the current source's 512 KiB configuration and fixes, upload the
retained `artifacts/firmware/pcm512-2026-11-07-r9/firmware.bin` using the same
OTA form. Check expected `pcm512-20261107-r9` build, fresh uptime,
`running_partition`, PCM capacity, saved output/preset/volume and audible
playback. Exact restoration SHA-256 and prior tests are in current state.
The restoration was subsequently completed through USB after wireless OTA
timed out. OTA sequence 6 selected app1 at `0x610000`; writing only that
application passed flash-hash verification and the API reported restored r9 /
app1. Saved station/volume were preserved. **app1 was current at that restoration**, and a failed
OTA may have partially written app0: use verified host images for rollback.

Workshop Sonos was verified through its device description and actual mDNS
reply but missed ordinary knob scans. Relaying the genuine discovery records
during a paused scan made it available without modifying firmware. Existing
selection/API then saved Workshop normally. With native preference enabled,
wait for receiver PLAYING after any TRANSITIONING state before beginning a
health check. The restoration check verified Fluid URI, volume/unmute,
artwork and stable native state for 68.68 seconds. No generic AirPlay fix was
claimed.


## Recovery after flashing another project

The accidental-project recovery on 2026-11-07 changed the current slot to
**app0**. Always read hardware identity and the actual partition table first.
A foreign project may change the bootloader, app offsets and NVS location;
application-only flashing at a historical address may not restore this app.
The confirmed target S3 MAC is 28:84:85:4b:e3:44. Leave other USB devices alone.

Back up the first 64 KiB into protected ignored artifacts before recovery.
Compare the bootloader/table with the saved AirPlayKnob build. In this case
all former NVS pages were blank; rebuild settings only from trusted existing
project configuration and retained user settings, with credentials private.
Generated NVS must match the 0x5000 partition and pass integrity validation.
The recovery used Espressif's NVS generator in a project-contained environment.

Validated separate writes for this layout: bootloader at 0, partition table
at 0x8000, reconstructed NVS at 0x9000, OTA initialization at 0xe000 and the
saved application at 0x10000. Use matching segments from the same validated
factory build, check image hashes and let esptool verify every write. Do not
write a combined factory image over existing settings when preserving them.
The recovery manifest under `artifacts/firmware/accidental-reflash-2026-11-07/`
records exact segments/hashes and protected settings sources. Original erased
pairing identity is not recoverable from application images alone.

After reboot verify expected build/app0, network, Workshop native mode,
requested station/volume, receiver PLAYING/URI/volume/mute and artwork. Allow
normal volume adjustments during monitoring; compare receiver volume with
the knob's current request. This recovery passed 13 samples over 66.47 seconds
with zero new underruns/reset, exact Fluid URI and current volume 8.

---

## Building clean credential-free GitHub Release binaries (`v1.1.0+`)

Because local development checkouts may contain a gitignored `include/secrets.h`
(automatically picked up via `#if __has_include("secrets.h")` in `include/app_state.h`),
**never** upload `.pio/build/waveshare_knob_s3/firmware*.bin` built inside a working
tree that contains `include/secrets.h` to a public GitHub Release.

Always build public release assets from an isolated `git worktree` checked out at
the target tag/commit:

```sh
git worktree add scratch/release-build v1.1.0
test ! -f scratch/release-build/include/secrets.h
/Users/austinmcchord/.platformio/penv/bin/pio run -d scratch/release-build
cp scratch/release-build/.pio/build/waveshare_knob_s3/firmware.factory.bin scratch/release-build/AirPlayKnob-v1.1.0-factory.bin
cp scratch/release-build/.pio/build/waveshare_knob_s3/firmware.bin scratch/release-build/AirPlayKnob-v1.1.0-ota.bin
(cd scratch/release-build && shasum -a 256 AirPlayKnob-v1.1.0-factory.bin AirPlayKnob-v1.1.0-ota.bin > SHA256SUMS.txt)
git worktree remove scratch/release-build
```

- **`AirPlayKnob-<version>-factory.bin`**: Combined bootloader (`0x0`), partition table (`0x8000`), `boot_app0` (`0xe000`), and `app0` (`0x10000`) image for fresh ESP32-S3 boards (`esptool --chip esp32s3 --baud 921600 --no-stub write-flash 0x0 AirPlayKnob-<version>-factory.bin`).
- **`AirPlayKnob-<version>-ota.bin`**: Application-only image for wireless browser OTA (`POST /api/ota` at `http://airplayknob.local/`) or active-slot USB updates that preserve existing NVS Wi-Fi credentials, station presets, and AirPlay pairings.

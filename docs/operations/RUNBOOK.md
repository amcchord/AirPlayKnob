# Device diagnostics and deployment

Validated on 2026-11-06 with the attached ESP32-S3 knob. Read
[the current state](../progress/CURRENT.md) before deploying again.

## Build and verify

```sh
/Users/austinmcchord/.platformio/penv/bin/pio run
/Users/austinmcchord/.platformio/penv/bin/python tests/test_alac_packets.py
/Users/austinmcchord/.platformio/penv/bin/python tests/test_rtsp_responses.py
/Users/austinmcchord/.platformio/penv/bin/python tests/test_network_policy.py
git diff --check
```

The decoder regression check requires FFmpeg and a host C++ compiler.
The RTSP check runs 16 cases against the actual firmware transaction method.
GitHub Actions runs both checks and a firmware build without private credentials.
Build dependencies are pinned in `platformio.ini`.
`STATUS\n` over the USB serial port at 115200 baud reports the active speaker,
packet and retransmission counters, PCM cushion, underruns, and available heap.
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


## Current deployed r5 image

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


## Artwork and local-stack diagnostics (r8)

Read `docs/progress/CURRENT.md` for the latest image/hash. The r8 update was
installed using the existing `/api/ota` multipart endpoint and runs in app1.
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

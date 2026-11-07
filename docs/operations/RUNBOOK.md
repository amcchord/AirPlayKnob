# Device diagnostics and deployment

Validated on 2026-11-06 with the attached ESP32-S3 knob. Read
[the current state](../progress/CURRENT.md) before deploying again.

## Build and verify

```sh
/Users/austinmcchord/.platformio/penv/bin/pio run
/Users/austinmcchord/.platformio/penv/bin/python tests/test_alac_packets.py
/Users/austinmcchord/.platformio/penv/bin/python tests/test_rtsp_responses.py
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

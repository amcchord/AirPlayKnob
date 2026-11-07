# Current state — 2026-11-07

**Smart Sonos playback restored.** Device runs `pcm512-20261107-r9`, source
`7e4158e`, in **app1**. Selected **Workshop Sonos Five**, preset **19 (Fluid)**,
volume **24**, `prefer_sonos_native=true`, `sonos_native=true`, local monitoring
and WhiteCap off. Sonos fetches the station directly; zero local PCM/MP3 is
intentional. The screen/portal should show **Audio buffered by Sonos**.

Restored the exact retained/tested r9 image without rebuilding or changing
source. Wireless upload timed out after 240 seconds without acknowledgment;
completed through USB on the confirmed ESP32-S3 (MAC 28:84:85:4b:e3:44).
Read OTA metadata: sequence 6 selects app1. Application-only write at
`0x610000` passed flash-hash verification; NVS, bootloader, partition table
and OTA selection were preserved. Restored build/app1 and preset/volume were
then observed over the web API. Failed OTA may have partially written app0;
do not assume that inactive slot contains a usable fallback.

Workshop's Sonos description and genuine mDNS response were verified. The
knob missed it in ordinary scans; relayed the receiver's verified discovery
records during a paused scan to make selection available. No firmware seed
or source change. The selected receiver is now saved by the normal UI/API
selection path. This assistance is not a general discovery reliability fix.

After the receiver finished TRANSITIONING, observed 12 native/receiver checks
over **68.68 seconds**: every receiver sample PLAYING, exact Fluid stream URI,
volume 24 / unmuted, zero new source underruns, no observed reset, album art
kind 2 present. This is a short restoration health check; acoustic feedback
was requested separately. Generic AirPlay/HomePod failures remain unresolved.

Firmware SHA-256:
`215b77a783a4d05b01dbaf7deba78f65cb7f998930a6667e09ff7af06177d7b1`.
Retained image/ELF/source/build validation:
`artifacts/firmware/pcm512-2026-11-07-r9/` (ignored).
Restoration manifest, OTA failure, USB/OTA metadata, discovery assistance,
boot/native snapshots and checks:
`artifacts/firmware/sonos-smart-restore-2026-11-07/` (ignored).
Original main `416c2ad` rebuild remains retained in its linked worktree and
`artifacts/firmware/og-main-2026-11-07/`; it is no longer installed. Other
rollback images and original backups remain retained on the host.

The source still has 512 KiB PCM / 256 KiB MP3 and revised startup/artwork
thresholds for controller audio. Both r9 and rebuilt original main failed
five-minute HomePod tests; see the [network audit](../NETWORK_AUDIT_2026-11.md).
Host/build checks passed for the exact reused r9 image; no source changes
required a rebuild or repeated host suite during restoration.

Git: root `codex/workshop-audio`, [PR #1](https://github.com/amcchord/AirPlayKnob/pull/1),
main unmerged. Broader AirPlay/discovery work stays open in
[issue #2](https://github.com/amcchord/AirPlayKnob/issues/2).
Next safe action: leave Workshop on smart Sonos playback. Before another
USB flash, recheck `running_partition` and OTA metadata; app1 is current.

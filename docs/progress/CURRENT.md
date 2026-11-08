# Current state — 2026-11-07

**Recovered after an accidental flash of another project.** The knob again
runs `pcm512-20261107-r9`, source `7e4158e`, now in **app0**. Workshop Sonos
Five is selected in smart/native mode, preset **19 (Fluid)**, current volume
**8**, local monitoring and WhiteCap off. Sonos fetches audio directly;
zero local PCM/MP3 is intentional. Album art is active.

The foreign firmware replaced the bootloader/partition layout and erased the
old AirPlayKnob settings area. Confirmed the S3 MAC 28:84:85:4b:e3:44 on
`/dev/cu.usbmodem14201` before writing. Backed up the first 64 KiB privately.
Restored the exact saved r9 bootloader/partition table, reconstructed NVS at
0x9000, OTA initialization at 0xe000 and firmware at 0x10000. All five segment
hash checks passed; generated NVS passed the official integrity check.
The combined factory image was validated but not flashed over the settings.

Reconstructed Wi-Fi from existing private project configuration and the last
verified Workshop/Fluid settings. The cached 30 presets exactly match source
defaults. The saved speaker record uses genuine previously verified discovery
metadata, so it resumed native Sonos without a new discovery workaround or
source seed. Original erased pairing identity cannot be preserved; firmware
regenerates its controller identity normally. No firmware source change or
rebuild. Other attached USB devices were left untouched.

The initial restored volume was observed at 18, then set to last-known 24;
it moved to 8 during verification. Kept the resulting current value instead
of repeatedly forcing 24. An initial strict fixed-volume monitor stopped on
that change; it was not a playback failure. The completed monitor then ran
**66.47 seconds / 13 samples**, all receiver PLAYING, exact Fluid stream URI,
receiver volume matching the knob, unmuted, zero new underruns or observed
reset, and album art. Physical screen/listening confirmation was requested
separately. Earlier smart Sonos playback had been user-confirmed steady.

Exact firmware SHA-256:
`215b77a783a4d05b01dbaf7deba78f65cb7f998930a6667e09ff7af06177d7b1`.
Saved image/ELF/source validation: `artifacts/firmware/pcm512-2026-11-07-r9/`.
Recovery manifests, private backup/settings, flash/integrity logs and native
checks: `artifacts/firmware/accidental-reflash-2026-11-07/` (ignored, protected).
Previously retained images and original-main worktree remain available.

Generic AirPlay/HomePod reliability is still unresolved; both r9 and rebuilt
original main failed five-minute HomePod tests. The source retains 512 KiB
PCM / 256 KiB MP3 and revised startup/artwork thresholds for controller audio.
No source changes required repeated host tests; this recovery reused the
previously validated image and verified the actual flashed layout/settings.

Root integration branch: `codex/workshop-audio`, published through
[PR #1](https://github.com/amcchord/AirPlayKnob/pull/1), main unmerged.
Broader transport/discovery work remains in
[issue #2](https://github.com/amcchord/AirPlayKnob/issues/2).
Next safe action: leave Workshop in smart Sonos mode. Before any future flash,
recheck hardware identity, partition table and OTA selection; **app0 is now
current**, and another project's layout requires recovery before application
addresses from this project can be assumed.

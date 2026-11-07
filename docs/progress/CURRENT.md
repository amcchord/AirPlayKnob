# Current state — 2026-11-07

Active work: publish the Workshop fixes and historical AirPlay audit to GitHub.
Root integration checkout: `codex/workshop-audio`, based on `416c2ad`.

The attached knob currently runs `workshop-sonos-20261107-r2`. Workshop plays
Groove Salad at volume 16 using native Sonos radio, with local monitoring off.
The user responded positively after the native playback checks. Previous
control, UI, metadata, and controller-reboot checks passed. The original
spontaneous reset was not captured.

Build `airplay-compat-20261107-r3` is ready for device verification. It adds a
persisted explicit AirPlay selector, sequence-matched RTSP transactions,
pinned build dependencies, generic speaker selection, and GitHub validation.
It builds without private Wi-Fi configuration. Twelve bit-exact ALAC decoder
cases and sixteen firmware RTSP transaction cases pass.

The historical audit reproduced the November 3 ALAC regression: `416c2ad`
removed explicit sample counts. The prior encoder and current fix pass both
352- and 4096-frame decoder configurations; the November 3 encoder fails the
4096-frame configuration. This does not isolate all of Workshop's observed
packet loss or identify the exact earlier Tree House firmware. See
[the audit and compatibility matrix](../AIRPLAY_REGRESSION_2026-11.md).

Next: verify the new selector on Workshop, restore native radio, publish the
branch/PR, and record the exact source/image and GitHub check results.
Original work and images are retained in ignored `scratch/workshop-fix/`;
deployed r2 artifacts remain in `artifacts/firmware/workshop-2026-11-07/`.

# Current state — 2026-11-07

Active work: publish the Workshop fixes and historical AirPlay audit to GitHub.
Root integration checkout: `codex/workshop-audio`, based on `416c2ad`.

The attached knob currently runs `airplay-compat-20261107-r3`. Workshop plays
Groove Salad at volume 16 using native Sonos radio, with local monitoring off.
The user responded positively after the native playback checks. Previous
control, UI, metadata, and controller-reboot checks passed. The original
spontaneous reset was not captured.

Build `airplay-compat-20261107-r4` is ready for device verification. It adds a
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

Published source: `c7a4d55`, [PR #1](https://github.com/amcchord/AirPlayKnob/pull/1).
GitHub validation passed. r3 explicit AirPlay connected but produced nine new
underruns; see [issue #2](https://github.com/amcchord/AirPlayKnob/issues/2).
Next: verify r4's native handoff and final playback, then update the PR record.
Original work and images are retained in ignored `scratch/workshop-fix/`;
deployed r2 artifacts remain in `artifacts/firmware/workshop-2026-11-07/`.

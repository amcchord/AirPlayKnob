# Current state — 2026-11-07

Deployed: `airplay-compat-20261107-r5`, source commit `1e692db`, on the attached
ESP32-S3 knob. Workshop is playing Groove Salad at volume 16 using optional
native Sonos radio, with local monitoring off. Selected station, volume,
playback and transport preferences are persisted. The portal can explicitly
select standard AirPlay with `prefer_sonos_native=false`.

Historical audit reproduced the November 3 ALAC regression in `416c2ad`:
removing explicit sample counts breaks decoding with a 4096-frame codec
default. The older encoder and restored format pass both 352- and 4096-frame
configurations. RTSP transactions also now match CSeq, and busy state reads
retry instead of disconnecting, clearing buffers, or saving default settings.
Location-specific speaker overrides were removed; build dependencies are pinned.
See [the audit](../AIRPLAY_REGRESSION_2026-11.md) for evidence and limits.

Standard AirPlay still suffers sustained UDP send failures and buffer depletion
on Workshop despite a successful AirPlay 2 handshake/timing. The exact driver,
RF or scheduling cause remains unresolved. Native radio bypasses that audio
relay; it is not universal AirPlay qualification. The earlier Tree House
Sonos/HomePod listening session cannot be mapped to an exact deployed binary
from Git alone. Remaining capability negotiation and receiver qualification
are tracked in [issue #2](https://github.com/amcchord/AirPlayKnob/issues/2).

Validation: 12 bit-exact ALAC cases, 16 actual firmware RTSP transaction cases,
portal JavaScript syntax, and a credential-free firmware build passed locally.
GitHub's Linux protocol/build checks passed for the deployed source. Live r5
checks passed 36 speaker/settings/home transitions, volume change/restore,
explicit AirPlay selection, return to native radio in 4.48 seconds, and optional
visualizer monitoring/stop. Local downloading stops before native connection
and during recovery. Sonos remained PLAYING in all 40 receiver polls across an
acknowledged software reboot; preferences restored and final 30-second native
observation had zero new underruns or resets. Original spontaneous UI reset
was not captured; these tests did not reproduce it. Final checks are receiver
and device observations, not a new listener acoustic confirmation.

Git: root integration branch `codex/workshop-audio`, published in
[PR #1](https://github.com/amcchord/AirPlayKnob/pull/1). Source commits:
`c7a4d55`, `b12dcc8`, `1e692db`; later record updates do not change firmware.
No merge to main has been performed.

Firmware SHA-256:
`51529abf8b1258eac664a34856fff160003ac2d3abbbc281de57227d4b91a60f`.
Exact image/ELF/source hashes and live results are retained in ignored
`artifacts/firmware/airplay-compat-2026-11-07-r5/`. Original preexisting work,
flash backup, deployed r2 source archive and private diagnostics remain in
ignored `scratch/workshop-fix/`. See [the runbook](../operations/RUNBOOK.md).

Next safe action: controlled AirPlay comparisons and receiver qualification
from issue #2, preserving native playback as the working Workshop setting.

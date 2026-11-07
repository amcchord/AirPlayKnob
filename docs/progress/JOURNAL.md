# Session journal

## 2026-11-06 — Workshop playback diagnosis and initial deployment

- Scope: attached ESP32-S3 knob to Workshop Sonos Five. Root checkout remained
  the control checkout; created branch `codex/workshop-audio` from `416c2ad`.
  Seven preexisting modified files were preserved in place and backed up as
  `scratch/workshop-fix/preexisting.patch`.
- Baseline: successful AirPlay 2 pairing, unmuted receiver, but severe UDP send
  failures (`ENOMEM`), retransmission requests, poor HTTP throughput, and
  repeated PCM underruns. A connected session did not imply audible playback.
- Found an ALAC interoperability issue: omitted frame lengths decode only when
  the decoder has matching frame-size configuration. Added explicit sample
  counts; all 12 independent FFmpeg decoder cases pass. Added UDP priority
  `IP_TOS=0xB8` and retained largest-free-block diagnostics.
- Reduced dynamic queues, reserved static queues, disabled AMPDU, and 802.11g
  experiments did not individually resolve the stall; experimental Wi-Fi mode
  changes were reverted. Successful audio coincided with selection of another
  AP, so QoS benefit was not isolated from that AP change.
- Production: multiple authorized USB app0-only test flashes. Wireless upload
  timed out. ROM-mode USB writes passed hash verification; stub-mode flash
  reads failed after their first chunk. NVS and OTA metadata were preserved.
  Saved the actual pre-task app0; its application prefix exactly matched the
  preexisting build. Recovery is documented in the runbook.
- Initial firmware SHA-256:
  `0f781becdb22a6b1f6d577e73c43ab86327fc9977f40a3a63338630b15e14c8c`.
  Source/image manifest retained in ignored `artifacts/firmware/workshop-2026-11-06/`.
- Verification: build and decoder checks passed; audible steady playback was
  confirmed. Pause/resume completed a fresh handshake and held ~11.9 seconds
  of PCM with no underruns over the observation. Some dropped packets were
  recovered by retransmission. Subsequently the user reported a UI restart
  and recurrent buffering loss; this supersedes the initial short-term result.
- No commit or push. Next: diagnose AP selection/recovery and capture UI reset.

## 2026-11-07 — Buffer depletion and restart recovery (in progress)

- Confirmed recurrent packet stalls on the AP selected after reboot. The knob
  remains connected while input falls below playback consumption. Repeated
  serial tab switching did not reproduce the user-observed reset in that test.
- Added a tested loss hysteresis policy, alternate-AP selection with cooldown,
  persistence of a qualified healthy AP, smaller bounded TX allocations,
  speaker-discovery scheduling changes, preference snapshot guard, and runtime
  reset/memory/stack diagnostics. No hardcoded AP selection is introduced.
- Host tests cover transient versus sustained packet loss, healthy qualification,
  reset behavior, AP signal/preference ranking, and both ALAC decoder defaults.
- Production verification is active. Do not mark the objective complete until
  live recovery, UI interaction, and restart behavior have been checked.

### 2026-11-07 — Final Sonos-native implementation and deployment

- Alternate-AP recovery, AP preference persistence, and voice-priority trials
  did not resolve the weak Wi-Fi path consistently. Those experimental roaming
  changes were removed; the earlier video-priority setting remains for AirPlay.
- Verified native `AVTransport` playback on Workshop, then added
  `SonosRadioClient`: manufacturer verification, URI/metadata, volume/unmute,
  play/stop, receiver-state and track polling, and reuse of an already-playing
  matching URI. Native rejection can fall back to AirPlay; explicit PIN pairing
  still selects AirPlay. No external codec dependency was added.
- Removed duplicate MP3 downloading for Sonos when local jack and visualizer
  monitoring are off. Selected Sonos-only output on the device; enabling the
  visualizer still activates optional local decoding. UI/portal distinguish
  speaker-owned buffering from local monitoring and show the correct 256 KB
  MP3 capacity.
- Fixed busy application-state snapshots in decoder, sender, UI, and preference
  persistence. A missed mutex acquisition must not pause playback, clear the
  ring buffer, display a false disconnected state, or save default settings.
  Limited TX allocations to preserve UI/recovery memory. Opening the speaker
  page no longer schedules automatic discovery while playing.
- Validation passed: PlatformIO firmware build; 12 bit-exact FFmpeg ALAC
  cases; native receiver volume change/restore, pause/resume, station
  change/restore; 54 UI transitions; optional visualizer activation; 80 seconds
  of continued receiver playback with advancing metadata and no new underruns.
  An intentional controller reboot left the Sonos receiver PLAYING in all 99
  half-second samples and restored controller state afterward.
- An initial integration assertion checked the new station before Sonos had
  completed its asynchronous transition. The station subsequently played;
  final tests wait for the requested URI and receiver PLAYING state. Temporary
  test stations/settings were restored. Original spontaneous UI reset was not
  captured, and was not reproduced during final interaction checks.
- Production: deployed app0 by USB ROM-mode write; flash hash verified, NVS and
  partition/OTA metadata retained. Final build `workshop-sonos-20261107-r2`,
  firmware SHA-256 `3ef3c756bae6e0ba7cd4c4bd1e714609c538c8c7e29fb2a169630b598c24a7cb`.
  Image/ELF/source-hash manifest retained under
  `artifacts/firmware/workshop-2026-11-07/`; checks and private serial evidence
  remain in ignored task scratch. No commit or push; preexisting changes remain.
- Left playing Groove Salad on Workshop, restored volume 16, local jack off,
  home screen selected. Receiver/controls/metadata checks passed; final acoustic
  confirmation remains with the listener near Workshop.


## 2026-11-07 — Historical AirPlay audit and publication preparation

- User authorized updating Git/GitHub and requested investigation of previously
  successful Tree House Sonos/HomePod playback. Remained in the root
  `codex/workshop-audio` checkout; fetched origin, which remains at `416c2ad`.
- Independently compiled historical ALAC encoders: October 26 `412ad20` and
  current explicit-count output passed 6/6 352-frame tests across two decoder
  configurations; November 3 `416c2ad` passed the 352 configuration but failed
  all three 4096 configuration cases. Recorded findings, limits, protocol
  changes and receiver acceptance matrix in `docs/AIRPLAY_REGRESSION_2026-11.md`.
- Fixed RTSP request/response CSeq matching; added 16 cases running the actual
  firmware transaction method. Added persisted `prefer_sonos_native` portal
  and API control so native Sonos radio can be bypassed for standard AirPlay.
- Pinned the installed pioarduino 55.03.38-1 and LVGL 8.4.0 versions; clean
  build without private credentials passed. Added GitHub Actions to run the
  build and both protocol regression checks. Removed location-specific
  speaker overrides/seeds and UI placeholders; saved selection and discovery
  remain. The original preexisting patch and deployed r2 source archive were
  preserved locally before these portability edits.
- Validation so far: 12 ALAC cases, 16 RTSP cases, portal JavaScript syntax,
  firmware build and whitespace checks pass. Private configuration literal
  scan found no matches in files being published. Device remains on working
  r2 pending r3 verification. No claim of universal receiver qualification.

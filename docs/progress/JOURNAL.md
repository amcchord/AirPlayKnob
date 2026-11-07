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


### 2026-11-07 — GitHub publication and explicit AirPlay live check

- Committed source as `c7a4d55` and pushed `codex/workshop-audio`; opened
  [PR #1](https://github.com/amcchord/AirPlayKnob/pull/1). GitHub's protocol
  checks and credential-free firmware build passed on Linux.
- Deployed `airplay-compat-20261107-r3`, firmware SHA-256
  `67d7d4cc5126939948aec1e636e37fa24f220cd40204cc8ff7efb0f945a04b30`,
  by hash-verified app0 write after checking OTA metadata. Saved station and
  volume restored; 36 UI transitions, volume change/restore, explicit AirPlay
  selection, and paused AirPlay preference persistence across reboot passed.
- Standard AirPlay 2 completed setup and timing, but 20 live samples showed
  nine new buffer underruns and sustained UDP ENOMEM/retransmission pressure.
  This remains unresolved; tracked in
  [issue #2](https://github.com/amcchord/AirPlayKnob/issues/2). Native mode was
  restored. Its reconnect was slow while the local decoder kept downloading.
- An extra operator-issued REBOOT interrupted the final sustained native
  observation. That observation is not counted as passed and is being rerun.
  No spontaneous reset was inferred from this interruption.
- Added a native routing-intent state so local MP3 downloading stops before
  the Sonos handshake, including recovery from AirPlay. Active transport and
  selected route remain separate telemetry. r4 build/protocol checks pass;
  device handoff and final sustained verification are next.


### 2026-11-07 — Final publication and r5 verification

- Published native-routing recovery changes as `b12dcc8` and `1e692db`.
  An intermediate r4 candidate was flashed, then superseded before handoff
  qualification: the active HTTP download loop also needed the routing-intent
  guard. Final build is `airplay-compat-20261107-r5`, source `1e692db`.
- Deployed r5 by hash-verified app0 USB write, preserving NVS/OTA selection.
  Firmware SHA-256:
  `51529abf8b1258eac664a34856fff160003ac2d3abbbc281de57227d4b91a60f`.
  Source manifest, ELF/image and integration results are retained under ignored
  `artifacts/firmware/airplay-compat-2026-11-07-r5/`. No private compile-time
  credentials were included in this build; saved device Wi-Fi was preserved.
- Live r5 checks passed: 36 UI transitions without reset/new underruns, volume
  change/restore, explicit AirPlay connection/timing, native handoff with local
  downloading stopped (4.48 seconds after the control request), and optional
  local visualizer monitoring followed by stopping its decoder.
- The standard AirPlay experiment still showed UDP ENOMEM, retransmission
  pressure and buffer depletion; native playback was restored. Issue #2
  retains this unresolved failure and the generic receiver acceptance matrix.
- A first final-check serial REBOOT command was not acknowledged; uptime did
  not reset, so that attempt was not counted as a reboot. Retried on a fresh
  serial connection with a leading newline, then verified the reboot message,
  decreasing uptime, software reset reason 3, persisted transport/playback, and
  all 40 receiver polls remaining PLAYING. A subsequent 30-second native
  observation passed with no new underruns or reset. No spontaneous reset was
  observed or inferred from these harness interruptions.
- GitHub Linux validation passed all 12 ALAC and 16 RTSP checks plus the
  credential-free firmware build for source `1e692db`. Added historical decoder
  options and exact reproduction commands; independently confirmed the older
  source passes and the November encoder fails with a 4096-frame default.
- Final device state: Workshop, Groove Salad, volume 16, native radio on,
  local monitoring off. PR #1 contains the published fix/audit; main has not
  been merged. Generic AirPlay restoration remains explicitly open in issue #2.


## 2026-11-07 — Wall-power and physical separation comparison

- User confirmed wall USB power and placement several feet from the Mac and
  any USB hub. The knob remained reachable on Wi-Fi with the same r5 firmware,
  saved Workshop/Groove Salad/volume-16 settings, and previously tested AP.
  The initial reset reason was power-on (1); uptime advanced through the test.
- Briefly selected standard AirPlay, then restored native radio. The harness
  did not observe a stable connected status within its 50-second transition
  window, so a sustained AirPlay qualification was not completed. Counters
  advanced from 0 to 1,628 attempted packets, 7 timing replies and 1,525
  retransmissions. This supports continuing transport trouble after the move;
  it does not isolate the underlying RF/driver/scheduling cause or establish a
  retransmission percentage. No serial send-error diagnostics were available
  while on wall power, and buffer depletion was not newly established by this
  limited observation.
- Native mode was restored and verified PLAYING, with ten subsequent receiver
  and device observations over at least 20 seconds and no new underruns.
  Final state: native radio, Groove Salad, volume 16, local monitoring off.
  No firmware change, flash, or additional restart. Private evidence retained
  in ignored `scratch/workshop-fix/wall-power-comparison.json`.


## 2026-11-07 — Album art repair and local network-stack audit (in progress)

- User requested fixing album art and checking local packet drops/discovery
  before attributing the AirPlay failure to Wi-Fi/RF; broader receiver
  compatibility remains deferred. Root branch remains `codex/workshop-audio`.
- Found station JPEG downloads pre-marked complete, live album lookup disabled
  while playing, and PCM-cushion guards preventing all native Sonos artwork.
  Restored station/live lookup with native-aware gating, bounded retries,
  stale-result rejection, baseline 160-pixel JPEGs, proper HTTP transfer
  decoding, and safe fallback. Added a task-scoped TLS allocator: bulk artwork
  TLS buffers use PSRAM without internal-RAM fallback; other TLS/pairing keeps
  the pinned SDK's allocator. The SDK normally forces TLS into internal RAM.
- The sender already logged local sendto ENOMEM failures, but incremented
  “sent” and returned success even after rejection. Repair counters ignored
  send results too. Added accepted/rejected/retry accounting, driver-boundary
  submission counters, and discovery telemetry. These are queue-acceptance
  observations, not over-the-air delivery or receiver acknowledgments.
- Replaced eight-repair bursts with retained pending repairs, original-audio
  priority, pacing and backoff after local rejection. Queued explicit/retry
  speaker scans and off-channel Wi-Fi scans during controller audio playback;
  preserved initial discovery, missing-IP discovery, paused scans and native
  Sonos-only scans. No AP-selection or PHY changes.
- Host checks passed: existing 12 ALAC and 16 RTSP cases, plus network policy,
  frame classification, allocation overflow/OOM/context, and actual repair
  queue/backpressure methods. Credential-free firmware build, portal syntax,
  private-configuration scan, and whitespace checks passed. r6 is ready for
  on-device artwork/driver-counter verification over OTA; r5 rollback images
  and private evidence remain retained locally.


### 2026-11-07 — r6 artwork verified; transition scan guard corrected

- Published `78c8528`; GitHub host checks and credential-free build passed.
  Deployed r6 over the existing web OTA endpoint into app1; image accepted and
  native playback restored. Firmware SHA-256
  `b109373a4c2881c683c39fba76eb3fc6566119d2650293e661646ef49e2b0e02`.
- Verified a live album JPEG downloaded from the knob, HTTP 200, decoder OK,
  album kind 2, and four task-scoped external TLS allocations. Initial native
  artwork work had zero driver rejections and internal-heap minimum 42,108
  bytes. User confirmed the album cover is visible on the physical screen.
- Live scan isolation caught a transition race: native-connected telemetry
  could remain true after standard AirPlay was requested, allowing a manual
  scan before the sender completed handoff. r6 scan-isolation check failed;
  it was not counted as passed. Native mode was restored with no new native
  underruns during the final observation.
- r7 uses the requested transport as well as active state and makes active or
  connecting AirPlay override stale native flags. Added regression cases for
  both contradictory-state transitions. Clean build and host policy/repair
  checks pass; repeating the live isolation test next.


### 2026-11-07 — Local rejection proven with scans deferred; artwork deployed

- Published transition correction `3b6244b`. Its 34-second live AirPlay audit
  passed scan isolation: scan count remained 1, scan-active flags stayed false,
  and UDP-5353 driver submissions remained 7 despite a queued explicit scan.
  The queue completed once native playback resumed.
- Last raw sample: 4,123 original attempts, 3,816 accepted, 307 rejected after
  bounded retries, 1,320 extra original retries, 393 accepted repairs and 7
  rejected repair attempts. Driver totals since boot were 6,920 submissions,
  5,204 accepted and 1,716 rejected (including all traffic and retries), with
  ESP_ERR_NO_MEM 257; socket errno was ENOMEM 12. These are local-layer counts,
  not an RF loss percentage. Active discovery is not the sole rejection cause.
- From the 8.5-second sample onward PCM stayed 11.860–11.882 seconds and the
  compressed-input cushion remained substantial; no source underruns occurred.
  This is a short buffering improvement, not acoustic/general AirPlay release
  qualification. Native mode was restored and observed for a further 24 seconds.
- Published `c1fb82c` to correct physical Queued/Reject/Repair labels and record
  the audit. r8 preserves the exact validated r7 artwork/network source hashes.
  Deployed via accepted web OTA, observed expected boot/build in app1. Image
  SHA-256 `10beac7ec2db1b580b4ee44b70fb5d382e6f893e6c9c7a9f96bec0c8b3e1716d`. Source/image manifest, boot/preflight,
  final JPEG and checks retained in ignored r8 firmware artifacts.
- User confirmed the artwork on the physical screen. Final r8 downloaded and
  published an album cover (decoder OK), used external TLS allocations and had
  no driver rejections since boot. Final 30-second native Sonos receiver/device
  observation passed with zero new underruns or reset. Preserved selected
  Groove Salad, Workshop, volume 20, local monitoring off.
- GitHub Linux host regressions and credential-free firmware build passed for
  deployed source. No AP selection, PHY or antenna changes. Broader AirPlay
  remains open/deferred in issue #2; local driver queue/allocation and blocking
  control writes are the next software diagnostics. No universal/RF conclusion.


## 2026-11-07 — User HomePod test and PCM-sizing question (read-only)

- User selected Workshop HomePod (AudioAccessory5,1), preset 19, volume 24,
  and reported struggling playback after roughly a minute. Preserved the
  user's selection/playback; firmware remains r8 in app1.
- Snapshot interval 98.913 seconds: PCM 11,855 -> 11,865 ms; MP3 76,218 ->
  79,937 bytes at 128 kbps; four new underruns, 10,190 new original attempts,
  2,626 additional locally rejected originals and 14,413 all-traffic driver
  rejections. These layer totals are not an RF loss rate. Both snapshots being
  full does not rule out the observed intervening underruns.
- Reviewed ring allocation/copies and thresholds. PCM uses fixed 2 MiB PSRAM;
  the consumer copies 352-frame chunks, not the whole ring. Reducing capacity
  can save external memory and queued latency but does not directly free
  internal/DMA driver memory. A ~512 KiB PCM target would give 2.97 seconds;
  retain compressed buffering for network jitter and revise 3-/6-second
  startup/artwork assumptions together if implementing it. No buffer or
  production configuration change was made in response to this question.
- Private snapshots remain in ignored scratch. Software send-queue/control
  scheduling investigation remains open; no new cause or fix is claimed.


## 2026-11-07 — 512 KiB PCM change and sustained HomePod test (in progress)

- User explicitly requested reducing PCM to 512 KB, adjusting startup/artwork
  thresholds, deploying, and testing HomePod for several minutes beyond the
  reported one-minute failure. Preserving Workshop HomePod, preset 19,
  volume 24; baseline is r8, active app1. Native Sonos is not selected for this
  experiment. Raw preflight evidence remains in ignored task scratch.
- Centralized capacity and timing policy: PCM 524,288 bytes / 131,072 frames
  (2,972 ms usable); MP3 remains 262,144 bytes. Connection needs 1 second PCM
  or 0.5 seconds with compressed reserve/settled Wi-Fi; playback/rebuffering
  needs 1 second with 16 KiB compressed input, otherwise 1.5 seconds.
- Artwork starts at 2 seconds PCM plus 32 KiB compressed reserve, aborting
  body writes below 1 second PCM. Native-only/paused behavior remains intact.
  Physical/portal capacities and API telemetry derive from the same config.
  Added count-only audio-gap and control-write timing to observe later stalls.
- Host checks pass: ALAC/RTSP, network/threshold policy, and actual 512-KiB PCM
  ring across eight simulated minutes with wraparound/stereo/saturation checks.
  Credential-free firmware build, portal syntax and whitespace checks passed.
  Candidate r9 is ready; deployment and five-minute live qualification follow.


## 2026-11-07 — r9 deployed; five-minute HomePod test reproduces interruptions

- Published source `7e4158e` on `codex/workshop-audio`; GitHub Linux validation
  passed (run 42404567449). Deployed `pcm512-20261107-r9` through accepted web
  OTA, observed expected boot in app0 and verified 524,288-byte PCM capacity /
  2,972 ms, unchanged 262,144-byte MP3 reserve and saved HomePod/preset/volume.
  Firmware SHA-256:
  `215b77a783a4d05b01dbaf7deba78f65cb7f998930a6667e09ff7af06177d7b1`.
- Real HomePod monitoring ran 300.13 seconds with 126 successful snapshots;
  first-to-last device uptime spans 293.478 seconds. That bounded interval
  recorded 26 new underruns, 5,908 locally rejected original submissions and
  31,998 all-traffic/retry driver rejections. Final underruns were 28 since the
  earlier boot checkpoint; those checkpoint deltas include time before the
  monitor. PCM reached 1 ms, MP3 zero, maximum original-send gap 12,984 ms and
  maximum measured RTSP write 1 ms. Timing maxima cover the connection.
- User confirmed choppy/intermittent HomePod audio. Album artwork appeared;
  no device/session reset or output change was observed. One status request
  timed out. Scan count stayed 1 with no active scan observed. The reboot
  selected a different AP on the same channel, preventing a controlled
  buffer-size A/B. No AP/PHY/antenna configuration changed.
- The requested deployment and sustained test are complete, but sustained
  AirPlay qualification failed. Leave 512 KiB/HomePod selected; do not claim
  decoder, RTSP writes, RF or universal receiver compatibility isolated/fixed.
  Exact manifests and private JSON/CSV retained in ignored r9 artifacts;
  r8 rollback image retained. README/architecture, current state, audit and
  runbook updated; published results to PR #1 and kept issue #2 open.
- Next diagnostic: hold AP/workload constant while separating source-input
  starvation, sender scheduling, internal/DMA allocation and TX queue drain.


## 2026-11-07 — Original GitHub main HomePod comparison (in progress)

- User requested flashing the original GitHub version and testing HomePod.
  Selected pre-troubleshooting main `416c2ad` in contained linked worktree
  `worktrees/og-main`, branch `codex/test-og-homepod`; integration checkout
  remains `codex/workshop-audio`. GitHub commit confirmed.
- Original source and platform configuration are unchanged, with no copied
  credentials. Firmware build passed using installed platform 55.03.38,
  Arduino 3.3.8 / IDF 5.5.4 and LVGL 8.4.0, matching current dependencies.
  Rebuilt image SHA-256:
  `c9fb563e92e8c5a43f4ec37420f821a7e84988a09667bd07a1007366d9fa2a6d`.
- Preserved current r9 rollback image and preflight HomePod/preset 19/volume
  24. Exact original image/ELF/source manifest and private snapshots are in
  ignored `artifacts/firmware/og-main-2026-11-07/`. OTA upload is in progress;
  expected legacy API lacks build, uptime, active-slot, BSSID and local-send
  acceptance telemetry. Test buffers/connections and actual audible playback
  without interpreting historical packet-attempt counters as delivery.


## 2026-11-07 — Original main flashed; HomePod also struggles

- OTA accepted unmodified `416c2ad` image, SHA-256
  `c9fb563e92e8c5a43f4ec37420f821a7e84988a09667bd07a1007366d9fa2a6d`.
  Observed legacy API and connected Workshop HomePod, preset 19 / volume 24,
  local DAC and WhiteCap off. No source/config/credential changes. r9 app0
  preceded OTA, so app1 is expected; original API cannot confirm slot/build.
- Real monitor ran 307.24 seconds; 70 successful snapshots span 297.462 seconds
  with 27 failed status requests. Both buffers reached zero; PCM range
  0–10,115 ms and MP3 0–630 bytes. User confirmed choppy/intermittent sound.
  At least 11 underrun increments were observed; one counter decrease makes
  the endpoint delta of 6 an undercount. Packet-counter reset/disconnect at
  52 seconds, reconnected at 71. Hardware reset cannot be established without
  uptime/reset telemetry. HomePod selection stayed unchanged.
- The original main source also fails under current conditions. Signal was
  -88 to -71 dBm versus r9's -69 to -61. Original lacks BSSID reporting, so
  this is not a controlled AP A/B and cannot isolate a particular code change.
  Historical attempt/repair counts cannot establish delivery. Rebuilt source
  is not proof of the exact binary used during earlier Tree House listening.
- Original firmware left installed as requested; r9 rollback image verified
  and retained. Exact original image/ELF/source/dependency manifest, build log,
  baseline and private JSON/CSV/assessment retained in ignored original-test
  artifact directory. Current state, network audit and legacy-OTA runbook
  updated; published this comparison in PR #1 / issue #2. No firmware fixes or
  main merge in this comparison. Next experiment should hold AP/workload fixed.


## 2026-11-07 — Restoring smart Sonos firmware (in progress)

- User requested returning to firmware supporting smart Sonos mode. Restore
  the verified retained r9 image, source `7e4158e`, then select the verified
  Workshop Sonos Five with `prefer_sonos_native=true`, preserving NVS station
  and volume. Original main test image remains retained for rollback.
- Verified Workshop device description: Sonos, Inc. / Sonos Five. Original
  knob status requests time out despite HTTP response headers, so preserve
  NVS without overwriting unknown live station/volume; read restored values
  after boot. Reusing the exact previously built/tested r9 image, SHA-256
  `215b77a783a4d05b01dbaf7deba78f65cb7f998930a6667e09ff7af06177d7b1`.
  No source changes or rebuild. OTA upload and post-boot checks in progress;
  private evidence in `artifacts/firmware/sonos-smart-restore-2026-11-07/`.


## 2026-11-07 — Smart Sonos restored over USB and receiver checked

- Restored exact r9/source `7e4158e` image at the user's request. Wireless OTA
  timed out after 240 seconds with no acknowledgment; post-attempt status was
  unavailable. USB confirmed S3 MAC 28:84:85:4b:e3:44. OTA entries 5/6 (valid
  states/CRCs) select app1. Wrote only app1 at 0x610000 through ROM/no-stub,
  passed flash-hash verification, rebooted, then observed expected build and
  app1 over API. Preserved NVS and observed preset 19 / volume 24 intact.
- Ordinary scans missed the verified Workshop Sonos Five. Queried its actual
  mDNS advertisement directly, verified Workshop PTR / port 7000 / IP, then
  relayed those genuine records to the knob during a paused scan. Workshop
  appeared and was selected through the existing API. No source/firmware
  changes or built-in location seed; discovery reliability remains open.
- Enabled `prefer_sonos_native=true`, resumed Workshop and verified native
  route with local monitoring off. Initial SOAP state TRANSITIONING was not
  counted as a passing playback check; waited for receiver PLAYING first.
  Twelve checks over 68.68 seconds then verified PLAYING, exact Fluid URI,
  volume 24 / unmuted, zero new underruns, no observed reset and album art.
  Acoustic confirmation requested; no audible pass inferred from telemetry.
- Exact image hash remains
  `215b77a783a4d05b01dbaf7deba78f65cb7f998930a6667e09ff7af06177d7b1`.
  Reused previously passing host/build image without rebuild. Private USB,
  metadata, discovery and native health evidence retained in ignored
  `artifacts/firmware/sonos-smart-restore-2026-11-07/`. Original source/image
  retained for rollback. Failed OTA may have partially changed inactive app0;
  use retained host images, not an assumed usable alternate slot.
- Updated current state/runbook and published device-state follow-up to
  existing PR #1 / issue #2. Native Sonos remains the working route; broad
  AirPlay/HomePod remains unresolved. Next action: keep smart Sonos selected.

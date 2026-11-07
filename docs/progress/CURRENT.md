# Current state — 2026-11-07

Deployed: `pcm512-20261107-r9`, source `7e4158e`, via web OTA into **app0**.
Preserved the user's **Workshop (HomePod)** selection (AudioAccessory5,1),
preset 19 and volume 24. PCM is now **524,288 bytes / 2,972 ms** usable;
compressed MP3 remains 262,144 bytes. Startup/rebuffering uses 1–1.5 seconds.
Artwork waits for 2 seconds PCM plus 32 KiB MP3 and yields below 1 second PCM.
UI/portal capacity labels and new timing telemetry match the configuration.

The requested five-minute HomePod test **failed sustained playback**. The user
confirmed choppy/intermittent audio. The monitor ran 300.13 seconds with 126
successful snapshots; the first-to-last samples span 293.478 device seconds
and contain 26 new underruns, 5,908 rejected original submissions and 31,998
driver rejections. Final underruns were 28 since the earlier post-boot
checkpoint. PCM fell to 1 ms and MP3 to zero. Maximum original-audio send gap
reached 12,984 ms; maximum measured RTSP write was 1 ms. Album art appeared.
No reset, session-counter reset or receiver change was observed; one status
request timed out. Scan count stayed 1 and no scan-active sample was observed.

The smaller ring did not resolve the stalls. These are local acceptance and
buffering observations, not RF delivery measurements. RTSP write timing does
not cover every sender operation. The reboot joined a different AP on the
same channel, so this is not a controlled buffer-size A/B. No AP-selection,
PHY or antenna configuration was changed. Broader receiver qualification
remains open in [issue #2](https://github.com/amcchord/AirPlayKnob/issues/2).

Validation: 12 bit-exact ALAC and 16 actual RTSP cases; network/allocator/repair
and new threshold policy checks; actual PCM ring wraparound/stereo/saturation
checks over eight **simulated** minutes; credential-free firmware build;
portal syntax and whitespace checks. GitHub Linux validation passed for
deployed source `7e4158e` ([run](https://github.com/amcchord/AirPlayKnob/actions/runs/42404567449)).
The live HomePod test is a failed qualification, not a passing build check.

Git: root `codex/workshop-audio`, published in
[PR #1](https://github.com/amcchord/AirPlayKnob/pull/1); main is not merged.
Firmware SHA-256:
`215b77a783a4d05b01dbaf7deba78f65cb7f998930a6667e09ff7af06177d7b1`.
Exact image/ELF/source manifest, preflight/boot snapshots and five-minute
JSON/CSV are retained in ignored `artifacts/firmware/pcm512-2026-11-07-r9/`.
The r8 image in `artifacts/firmware/art-network-2026-11-07-r8/` is retained for
rollback; original flash backup and preexisting work are also retained.

Native Sonos radio remains the previously verified fallback; the current
selection is HomePod. Artwork was restored and physically user-confirmed
before this change, and live album art was observed again during r9 testing.
See the [network audit](../NETWORK_AUDIT_2026-11.md) and
[historical audit](../AIRPLAY_REGRESSION_2026-11.md) for prior findings.

Next safe action: preserve the requested 512 KiB/HomePod configuration while
isolating input starvation, driver-buffer eligibility/queue drain and sender
scheduling. Further experiments should hold the AP and workload constant.
Check `running_partition` and OTA metadata before any future USB flash.

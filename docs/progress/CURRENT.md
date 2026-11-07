# Current state — 2026-11-07

Deployed: `art-network-20261107-r8`, source `c1fb82c`, via web OTA into **app1**.
Workshop uses native Sonos radio, Groove Salad, volume 20, local monitoring
off. User-selected volume was preserved. Album artwork is restored: the user
confirmed it on the physical screen, and the final build served an actual
album JPEG with HTTP 200 / decoder OK and active external TLS allocations.

Latest scope: fix artwork and audit local network losses; broader AirPlay
compatibility remains deferred at the user's request. See the
[network audit](../NETWORK_AUDIT_2026-11.md) and
[historical audit](../AIRPLAY_REGRESSION_2026-11.md).

Confirmed: the old counters included failed local submissions. New socket and
SDK driver counters separate attempts, acceptance and rejection; none claim
RF/receiver delivery. Repairs are retained, paced and backed off after local
rejection. Artwork has bounded retries, correct native-buffer gating, supported
baseline JPEGs and task-scoped PSRAM TLS buffers. Other TLS/pairing allocation
policy is unchanged.

The controlled r7 AirPlay test kept explicit/retry speaker scans deferred,
with scan count 1 and UDP-5353 submissions fixed at 7 throughout 34 seconds.
It still observed 307 rejected original submissions among 4,123 originals,
and 1,716 driver rejections including retries/other traffic. Driver error was
ESP_ERR_NO_MEM (257), socket errno ENOMEM (12). From 8.5 seconds onward the PCM
cushion stayed 11.860–11.882 seconds, with no underruns. This improves source
buffering but does not qualify generic AirPlay or identify an AP/RF cause.
The queued scan completed after native mode was restored. r8 retains that
same artwork/network implementation and improves the physical counter labels.

Validation: 12 ALAC and 16 RTSP cases; network gating, contradictory transition
states, allocator overflow/OOM/task isolation, truncated-frame parsing, and
actual repair-queue methods; credential-free firmware builds and portal
syntax. GitHub Linux validation passed for deployed source `c1fb82c`. User
confirmed artwork; final r8 native/receiver observation lasted 30 seconds with
no new underruns/reset, album kind 2, and zero driver rejections since boot.
Original spontaneous UI reset and broader receiver compatibility are not newly
qualified. No AP/PHY/antenna configuration was changed.

Git: root `codex/workshop-audio`, published in
[PR #1](https://github.com/amcchord/AirPlayKnob/pull/1); main is not merged.
Remaining local queue/driver investigation and receiver qualification are in
[issue #2](https://github.com/amcchord/AirPlayKnob/issues/2).

Firmware SHA-256: `10beac7ec2db1b580b4ee44b70fb5d382e6f893e6c9c7a9f96bec0c8b3e1716d`.
Exact image/ELF/source hashes and final checks:
`artifacts/firmware/art-network-2026-11-07-r8/` (ignored). The scan-isolation
trace is retained under the r7 artifact directory. r6's failed transition test
is recorded separately and was not counted as passed. r5 rollback images,
preexisting work, original flash backup and private evidence remain retained.

Next safe action: investigate driver-buffer eligibility/queue drain and
control-write scheduling using the new counters, while leaving native Sonos
playback as the working output. Verify OTA selection before any USB flash;
app0 is no longer the current active-slot assumption.

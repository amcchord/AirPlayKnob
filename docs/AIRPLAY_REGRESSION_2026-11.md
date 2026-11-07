# November 2026 AirPlay regression audit

## Findings

The November 3 commit `416c2ad` introduced a reproducible ALAC interoperability
regression. It replaced the encoder in `412ad20` and removed the explicit
32-bit sample count from each uncompressed ALAC frame (`has_size=1` became
`has_size=0`). Payloads shrank from 1,416 to 1,412 bytes. A receiver must then
get the 352-frame packet length from its external codec configuration.

The audit extracted and compiled the actual encoder from each revision and
decoded its output with FFmpeg. Each cell covers silence, signed extremes,
and independent stereo noise, at 352 stereo frames per packet:

| Encoder | Decoder configured for 352 frames | Decoder configured for 4096 frames |
| --- | --- | --- |
| `412ad20` (October 26) | 3/3 bit-exact | 3/3 bit-exact |
| `416c2ad` (November 3) | 3/3 bit-exact | 0/3; decoding fails |
| Current explicit-count encoder | 3/3 bit-exact | 3/3 bit-exact |

This is a source-level regression with an independent decoder reproduction.
It can produce a successful control handshake followed by undecodable audio.
We have not inspected the Sonos decoder's internal codec configuration, so it
is **not proof that this alone caused Workshop's silence**. The earlier Tree
House listening session is user-confirmed; Git does not identify its exact
deployed binary. It must not be assigned to a particular commit by inference.

The fix restores explicit counts for all AirPlay outputs. The checked-in
`tests/test_alac_packets.py` also covers one-frame packets and runs 12
bit-exact decoder cases, protecting both decoder configurations.

To reproduce a historical cell with the same harness, extract its source
and choose the packet and decoder lengths explicitly (requires FFmpeg and
a host C++ compiler):

```sh
mkdir -p scratch
git show 412ad20:src/audio/raop_AnySpeaker.cpp > scratch/october-raop.cpp
python3 tests/test_alac_packets.py scratch/october-raop.cpp --frames 352 --decoder-frame-length 4096
git show 416c2ad:src/audio/raop_AnySpeaker.cpp > scratch/november-raop.cpp
python3 tests/test_alac_packets.py scratch/november-raop.cpp --frames 352 --decoder-frame-length 4096
```

The October command passes; the November command is expected to fail.
Selecting `--decoder-frame-length 352` passes for both revisions. The normal
test command without arguments validates the current encoder's full matrix.

## Other changes in the regression window

`f8a23d8` introduced HomePod AirPlay 2 support on October 26. `d5bbe90` enlarged
buffers and added asynchronous control later that day. The next source commit,
`416c2ad`, combined extensive UI, pairing, artwork, and streaming changes:

- Sonos model names and `et=4` began steering receivers toward AirPlay 2;
  the October 26 path normally used legacy RAOP for those Sonos advertisements.
  Changing protocol is a separate variable from changing the audio encoder.
- AirPlay 2 startup changed from session SETUP → stream SETUP → RECORD to
  session SETUP → RECORD → stream SETUP. The current retry logic came from
  subsequent uncommitted work. Receiver-specific startup expectations need
  explicit tests; neither order should be declared universally compatible.
- RTP timestamp correction became much more aggressive (100 ms versus the
  earlier 2-second lag threshold). The pre-session working tree had already
  relaxed it again and limited catch-up pacing.
- Metadata and artwork began sharing the control connection with startup,
  volume, and keepalive commands.

The later uncommitted work also changed Wi-Fi initialization, AP selection,
HTTP buffering, retransmission, and discovery. Its original patch and device
image were retained locally before modification. It is not a separately
identified historical release.

## Additional confirmed defects and observed failures

The RTSP client accepted the next complete response without matching its
`CSeq` to the request. Thus a late RECORD or asynchronous command response
could be accepted as a SETUP response. This defect predates November 3, but
the newer retry sequence exposes it more readily. Transactions now consume
stale replies and wait for their own sequence number. Sixteen host cases run
the actual firmware transaction method, including delayed, coalesced, split,
failed, truncated, and mismatched replies. This follows the transaction
matching rule in [RTSP 1.0 section 12.17](https://www.rfc-editor.org/rfc/rfc2326.html#section-12.17).

Busy state-mutex reads could also substitute default `playing=false` and
`wifi_connected=false`, clearing buffers or disconnecting during UI activity.
Preferences could save defaults after a failed snapshot. Decoder, sender, UI,
and playback preference code now retry failed snapshots. An actual spontaneous
hardware reset was not captured; these fixes must not be presented as a
proven reset diagnosis.

Workshop's direct AirPlay tests additionally showed sustained UDP `ENOMEM`,
retransmission pressure, and radio-input starvation below consumption. Both
available APs exhibited trouble. Smaller queues, QoS changes, AP changes,
and PHY experiments did not establish a reliable general Wi-Fi fix. Adding a
larger buffer cannot repair a sustained input-rate deficit. The exact driver,
RF, or scheduling cause remains unresolved.

The November 7 r3 live test explicitly disabled native radio on Workshop.
AirPlay 2 completed setup and timing, but 20 status samples recorded nine new
buffer underruns with sustained UDP send failures and retransmissions. The
remaining generic transport work is tracked in
[issue #2](https://github.com/amcchord/AirPlayKnob/issues/2).

The platform had been specified only as `espressif32`, which did not capture
the installed pioarduino fork. It is now pinned to the actual 55.03.38-1
release, with Arduino 3.3.8 / IDF 5.5.4 and LVGL 8.4.0. No evidence establishes
a framework upgrade as the cause of the regression.

## Compatibility design and acceptance criteria

AirPlay remains the common transport for generic receivers. Sonos native radio
is an optional optimization: a verified Sonos receiver fetches the original
MP3 station itself. It bypasses the controller's troubled high-bandwidth
relay, but **does not count as an AirPlay compatibility test**. Disable
`prefer_sonos_native` in the portal or API to exercise AirPlay on Sonos.
The preference is persisted; it does not affect other receiver families.

Location-specific speaker seeds, default-room selection, and network-dependent
speaker overrides were removed. Saved selection and mDNS discovery determine
the available output. Compile-time Wi-Fi credentials remain optional.

The current negotiation still contains model hints and an overly broad
AirPlay 2 feature heuristic. A complete capability-based negotiation redesign
is outstanding: preserve advertised encryption/codec capabilities, distinguish
pairing support from transient-pairing permission, try only compatible
transports, and retry on a clean session. In particular, `et=4` alone must not
be treated as proof that legacy RAOP is unusable. OwnTone's
[AirPlay implementation](https://github.com/owntone/owntone-server/blob/master/src/outputs/airplay.c)
and pyatv's [capability definitions](https://github.com/postlund/pyatv/blob/master/pyatv/protocols/airplay/utils.py)
are comparison references, not a replacement for receiver tests.

Release qualification should cover this matrix:

| Receiver path | Current evidence | Required acceptance |
| --- | --- | --- |
| Workshop Sonos native radio | Audibly working; controls, UI and reboot exercised | Continued receiver PLAYING state and advancing metadata |
| Sonos over AirPlay | Earlier user-confirmed Tree House playback; current relay still has a Wi-Fi failure history | Audible 30-minute playback with native radio disabled; pause/resume, volume, station and UI changes |
| HomePod over AirPlay 2 | Earlier user-confirmed Tree House playback; not newly requalified | Same sustained test, plus pairing and timing recovery |
| Legacy RAOP (e.g. AirPort/Shairport) | Decoder compatibility tested, receiver not newly tested | RSA/plain transport as advertised; correct audio and retransmission |
| Other AirPlay 2 / PIN receivers | Existing implementation; no new receiver qualification | Capability/authentication, PIN cancellation/retry, controls, reconnection |

For every AirPlay path, verify accepted versus dropped sends, continuous RTP
sample timestamps, timing replies, stable source buffers, actual audible
output, and recovery after network interruption. A connection flag, packet
attempt counter, or native Sonos success cannot establish universal AirPlay
compatibility. IPv6-only, password/authentication variants, grouping, and
receivers outside the tested transport/codec modes remain separate work.

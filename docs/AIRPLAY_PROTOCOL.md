# AirPlay 1 (RAOP) & AirPlay 2 Protocol Implementation

`AirPlayKnob` implements a self-contained C++ AirPlay transmitter (`src/audio/raop_AnySpeaker.cpp` and `include/raop_AnySpeaker.h`) for real-time 44.1 kHz 16-bit stereo audio, using legacy **AirPlay 1 (RAOP)** and **AirPlay 2** transports. Protocol coverage is broader than the currently qualified receiver set. See the [regression audit and test matrix](AIRPLAY_REGRESSION_2026-11.md) before interpreting this implementation description as a compatibility guarantee.

---

## 1. mDNS Discovery & Speaker Classification

Discovery queries `_raop._tcp.local` and `_airplay._tcp.local`. RAOP instances commonly use `<MAC>@<Friendly Speaker Name>`, with TXT records describing capabilities:

| TXT Key | Meaning | Values Handled |
| :--- | :--- | :--- |
| `am` | Apple Model identifier | `AudioAccessory1,1` / `5,1` / `6,1` (HomePod / mini), `AppleTV*`, `Arc` / `Era 300` / `Move` (Sonos), `AirPort*` |
| `et` | Supported encryption types | `0` (None), `1` (RSA + AES-128-CBC), `3` (FairPlay), `4` (MFi / `/auth-setup` Curve25519), `5` (FairPlay SAPv2.5) |
| `cn` | Supported audio codecs | `0` (PCM), `1` (ALAC - Apple Lossless), `2` (AAC), `3` (AAC-ELD) |
| `pk` | Ed25519 public key | Present on AirPlay 2 / HomeKit-enabled receivers |

### Automatic Protocol Selection
`RaopClient::connect()` probes `GET /info RTSP/1.0` on the advertised RTSP port.
The current code combines that reply with model/encryption hints to choose
AirPlay 2 or legacy RAOP. This heuristic is broader than the actual negotiated
capabilities: `statusFlags & 0x04` is not reliable evidence of transient pairing,
and `et=4` alone does not require AirPlay 2. Replacing those assumptions with
capability-based negotiation is recorded in the regression audit. Legacy RAOP
implements `/auth-setup` Curve25519 and optional RSA-OAEP/AES-128-CBC ANNOUNCE.

---

## 2. AirPlay 2 Pipeline (HomeKit Transient Pairing + ChaCha20-Poly1305)

Modern Apple HomePods and Apple TVs reject legacy RAOP `ANNOUNCE` handshakes (`403 Forbidden`) and require **HomeKit Transient Pair-Setup** followed by **ChaCha20-Poly1305** encrypted binary-plist `SETUP` phases.

```mermaid
sequenceDiagram
    participant ESP as AirPlayKnob (ESP32-S3)
    participant HP as HomePod / Apple TV (Port 7000)

    ESP->>HP: GET /info RTSP/1.0
    HP-->>ESP: 200 OK (Binary Plist capabilities)
    ESP->>HP: POST /pair-setup (TLV8 M1: Method=0, Flags=0x10 Transient)
    HP-->>ESP: 200 OK (TLV8 M2: 16B Salt + 384B SRP Server Public Key B)
    Note over ESP: SRP-6a (3072-bit Group, SHA-512, PIN="3939")<br/>Compute Client Public Key A (384B) & Proof M1 (64B)
    ESP->>HP: POST /pair-setup (TLV8 M3: A + M1)
    HP-->>ESP: 200 OK (TLV8 M4: Server Proof M2)
    Note over ESP,HP: HKDF-SHA-512 Session Key Derivation:<br/>Control-Write / Control-Read / Event-Write / Event-Read Keys
    Note over ESP,HP: === Switch RTSP Socket to Framed ChaCha20-Poly1305 ===
    ESP->>HP: SETUP rtsp://ip/sid (Phase 1 Bplist: timingPort, eventPort)
    HP-->>ESP: 200 OK (Bplist: server eventPort, timingPort)
    ESP->>HP: Connect TCP to server eventPort (Encrypted Event Channel)
    ESP->>HP: SETUP rtsp://ip/sid (Phase 2 Bplist: type=96, shk=32B, spf=352)
    HP-->>ESP: 200 OK (Bplist: server dataPort, controlPort, audioLatency)
    ESP->>HP: RECORD rtsp://ip/sid (RTP-Info: seq=..., rtptime=...)
    HP-->>ESP: 200 OK
    loop Every 352 Stereo Samples (7.98 ms)
        HP->>ESP: UDP NTP Timing Request (PT=0x52)
        ESP-->>HP: UDP NTP Timing Response (PT=0x53)
        ESP->>HP: UDP RTP ALAC + ChaCha20-Poly1305 Audio Frame (PT=0x60)
    end
```

### Key Cryptographic Details
1. **HomeKit Transient SRP-6a (`doAp2PairTransient`)**:
   - Uses the fixed transient PIN `"3939"` with the RFC 5054 3072-bit prime $N$ and generator $g = 5$.
   - Computes multiplier $k = \text{SHA512}(N \parallel \text{pad}(g))$, private key $x = \text{SHA512}(\text{salt} \parallel \text{SHA512}(\text{"Pair-Setup:"} \parallel \text{"3939"}))$, verifier $v = g^x \bmod N$, and shared secret $S = (B - k \cdot v)^{a + u \cdot x} \bmod N$ using hardware-accelerated `mbedtls_mpi_exp_mod`.
   - Derives the shared session key $K = \text{SHA512}(S)$ and verifies mutual proofs $M_1, M_2$.
2. **HKDF-SHA-512 Channel Keys**:
   - Control encryption keys use salt `"Control-Salt"` with info `"Control-Write-Encryption-Key"` (client $\to$ speaker) and `"Control-Read-Encryption-Key"` (speaker $\to$ client).
   - Event channel keys use salt `"Events-Salt"` with info `"Events-Write-Encryption-Key"` and `"Events-Read-Encryption-Key"`.
3. **Framed RTSP ChaCha20-Poly1305**:
   - Every encrypted RTSP block is prefixed with a 2-byte little-endian plaintext length (up to `1024` bytes per block), which is passed as the **Additional Authenticated Data (AAD)** to `mbedtls_chachapoly_encrypt_and_tag`, followed by the ciphertext and 16-byte Poly1305 tag.
   - The 96-bit nonce consists of 4 zero bytes followed by the 64-bit little-endian block counter (`counter++`).
4. **RTP Audio Packet ChaCha20-Poly1305**:
   - Each 352-sample stereo frame (`1,408` bytes of PCM) carries a 23-bit uncompressed ALAC CPE header with `has_size=1`, an explicit 32-bit sample count, big-endian PCM samples, a 3-bit `ID_END`, and byte padding: `1,416` bytes total. The explicit count avoids relying on the receiver changing its default ALAC frame length to match `spf=352`.
   - Encrypted with the 32-byte stream key `shk` sent during `SETUP` Phase 2, using bytes `4..11` of the 12-byte RTP header (`timestamp` + `SSRC`) as the 8-byte AAD, and appending the 16-byte Poly1305 tag + 8-byte little-endian packet nonce at the end of the UDP datagram (`12 + 1416 + 16 + 8 = 1,452` bytes total).

---

## 3. AirPlay 1 (RAOP) Pipeline (Sonos, AirPort Express, ShairPort)

For AirPlay 1 receivers:
1. **Sonos Curve25519 MFi Handshake (`POST /auth-setup`)**:
   - Sends a 33-byte payload (`0x01` + 32-byte ephemeral Curve25519 public key) so Sonos speakers accept unauthenticated RAOP sessions without requiring an Apple MFi hardware coprocessor.
2. **SDP `ANNOUNCE`**:
   - Negotiates `rtpmap:96 AppleLossless` (`fmtp:96 352 0 16 40 10 14 2 255 0 0 44100`).
   - If the receiver advertises `et=1`, generates a random 16-byte AES key and 16-byte IV, encrypts the AES key using the Apple Airport RSA public key (`RSA-OAEP-SHA1`), and includes `a=rsaaeskey:` and `a=aesiv:` in the SDP body.
3. **Transport `SETUP` & `RECORD`**:
   - Negotiates UDP `server_port`, `control_port`, and `timing_port` over `Transport: RTP/AVP/UDP;unicast;mode=record`.
   - Encrypts each 352-sample uncompressed ALAC frame with `AES-128-CBC` (resetting the IV to `m_aes_iv` per packet per the RAOP specification).

---

## 4. NTP Timing Synchronization & UDP Retransmission

- **Timing Sync (`PT=0x52` / `PT=0x53`)**: A dedicated high-priority FreeRTOS task (`timingTaskLoop`) listens on `m_local_time_port` and immediately echoes the speaker's 64-bit NTP originate timestamp along with the ESP32's current microsecond clock converted to NTP epoch (`0x83AA7E80` seconds offset).
- **RTP Sync Packets (`PT=0x54`)**: Sent every 1.0 second on the control socket, mapping the current RTP sample timestamp (`m_head_ts`) to the wall-clock NTP timestamp, with `m_latency_frames = 44100` (1.0s buffer) so the speaker has ample jitter margin.
- **UDP Retransmission (`PT=0x55` $\to$ `PT=0x56`)**: All outgoing RTP packets are stored in a 512-slot PSRAM circular buffer (`m_backlog`, covering ~4 seconds of audio). If the speaker reports a missed UDP sequence range via `PT=0x55` on the control port, `handleControlSocket()` immediately re-sends the requested packets wrapped in a `PT=0x56` retransmission header.

## 5. Packet Priority and Decoder Regression Check

Audio, control, and timing UDP sockets use `IP_TOS=0xB8` (IP precedence 5),
which the ESP32-S3 maps to the Wi-Fi video access category. This gives the
paced media traffic priority over best-effort traffic while retaining AMPDU
support. See [Espressif's Wi-Fi QoS mapping](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-guides/wifi.html#wi-fi-qos).

Run `python3 tests/test_alac_packets.py` with a host C++ compiler and FFmpeg
installed. It compiles the firmware encoder and checks bit-exact PCM recovery
for silence, signed sample extremes, and independent stereo samples with both
352-frame and standard 4096-frame decoder configurations.

## 6. Native Sonos Radio Output

This is an optional radio transport, not evidence of AirPlay interoperability.
Set `prefer_sonos_native=false` in `/api/control` (or use the portal checkbox)
to keep Sonos on the standard AirPlay path. The preference persists across
restarts. See the [November regression audit](AIRPLAY_REGRESSION_2026-11.md)
for the historical encoder regression and receiver qualification matrix.

Recognized Sonos receivers are verified against their device description on
port 1400. For these receivers, the preferred radio output uses native
`AVTransport` and `RenderingControl` SOAP actions: set the original station
URI and metadata, set volume/unmute, and play or stop. Existing matching URIs
are reused so reconnecting the controller does not replace an already-playing
stream. Sonos fetches and decodes the original station itself; no RTP audio is
relayed by the knob. The native radio URI/control behavior is also documented
by the [SoCo implementation](https://github.com/SoCo/SoCo/blob/master/soco/core.py).

The receiver's transport state and track metadata are polled every ten
seconds. Network timeouts alone do not stop a stream already playing on the
receiver. A receiver rejecting native control can use the existing AirPlay
fallback. Explicit PIN pairing requests continue to select AirPlay.

When both local DAC output and WhiteCap are off, the knob does not open a
duplicate MP3 connection. Its local PCM/MP3 counters are then intentionally
zero; the UI says that Sonos owns audio buffering. The visualizer or optional
local DAC enables local decoding without changing the Sonos stream. Metadata
comes from the Sonos receiver when local decoding is off.

Audio, decoder, and UI snapshots must succeed before using state values.
Failure to acquire the application mutex is a retry condition; it must not
be interpreted as pause/disconnect, an empty configuration, or permission to
clear buffered audio.

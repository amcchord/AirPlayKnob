# AirPlayKnob — SomaFM Internet Radio to AirPlay Streamer

An Internet Radio to AirPlay RAOP streamer and tactile controller built for the **[Waveshare ESP32-S3-Knob-Touch-LCD-1.8](https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8)** (360×360 round QSPI display, capacitive touch, mechanical rotary encoder, DRV2605 haptic motor driver, and PCM5100A stereo DAC).

---

## ✨ Features

- **30 Pre-Populated SomaFM Stations**: Comes pre-loaded with 30 curated SomaFM channels (*Groove Salad*, *Drone Zone*, *DEF CON Radio*, *Secret Agent*, *Lush*, *Space Station Soma*, *Underground 80s*, *Vaporwaves*, *Indie Pop Rocks!*, *Beat Blender*, and more) with automatic server fallback (`ice6`, `ice2`, `ice4`) and live ICY `StreamTitle` track metadata parsing.
- **Native AirPlay (RAOP) Streaming**:
  - Automatic mDNS discovery (`_raop._tcp.local`) of AirPlay speakers on the LAN (Sonos, Apple HomePod, Apple TV, AirPort Express, ShairPort, and standard RAOP receivers).
  - Full RTSP handshake (`OPTIONS`, `/auth-setup` Curve25519 for Sonos, `ANNOUNCE`, `SETUP`, `RECORD`, `SET_PARAMETER` volume & live DMAP track metadata, `FLUSH`, `TEARDOWN`).
  - Real-time 44.1 kHz 16-bit stereo MP3 decoding into a 256 KB PSRAM ringbuffer, framed as 352-sample RTP ALAC packets with UDP timing sync and retransmission buffer.
- **Simultaneous Local 3.5mm Stereo Output**: Also drives the onboard **PCM5100A** I2S stereo DAC via the `CH445P` analog switch so you can monitor or plug into powered speakers directly from the knob's 3.5mm jack.
- **Classy Minimalist 360×360 Round UI + DRV2605 Haptics (180° Inverted Orientation with USB at Top)**:
  - **Now Playing Screen**: Clean obsidian aesthetic focused on a `156×156` rounded **Album Art / Stream Art** centerpiece, bold **Track Name** marquee, **Artist**, and active **Station • Speaker** line, framed by a subtle `3px` 360° perimeter volume/tuner arc.
  - **Live Album Art & SomaFM Stream Art**: Automatically queries the iTunes Search API for live track album art (`160×160` baseline JPEG decoded via ESP32-S3 hardware ROM `tjpgd`), falling back to official SomaFM channel artwork (`https://somafm.com/img/<id>120.jpg`) and bespoke procedural vector station covers.
  - **Full-Screen "WhiteCap" Visualizer**: Inspired by the classic Winamp SoundSpectrum WhiteCap visualizer—renders 30 FPS phosphor-fading 3D wireframe wave meshes in PSRAM (`3D Ribbon Tunnel`, `Harmonic Coriolis Web`, `Aether Horizon`, and `Supernova Halo`) driven by real-time Left/Right stereo audio waveforms.
  - **Three Rotary Knob Modes**:
    - `VOL` (Volume): Smoothly adjusts remote AirPlay speaker + local DAC volume (`0–100%`) with tactile haptic clicks.
    - `TUNE` (Radio Dial): Turning the knob scrubs through stations like a classic analog radio dial with crisp haptic detents; stops and tunes after a brief pause.
    - `SPKR` (Speaker Selector): Turning the knob cycles through discovered AirPlay speakers on your network.
  - **Stations & AirPlay Sheets**: Minimalist scrollable sheets for station presets, discovered `_raop._tcp` speakers, and QR code Web UI access.
- **OTA Firmware Updates (`ArduinoOTA` + Web UI Upload)**:
  - Supports wireless `ArduinoOTA` pushes at `airplayknob.local:3232` (`pio run -t upload --upload-port airplayknob.local`) as well as browser-based `.bin` firmware uploads at `POST /api/ota`.
- **Embedded Web Interface (`http://<device-ip>/`)**:
  - Live Now Playing dashboard with ICY track title, bitrate, buffer fill, and volume slider.
  - Add, edit, delete, or restore default SomaFM stream presets (persisted in ESP32 NVS flash).
  - Switch AirPlay output speakers or scan & join a new Wi-Fi network from any browser.

---

## 🛠️ Hardware Pinout (Waveshare ESP32-S3-Knob-Touch-LCD-1.8)

| Subsystem | Controller / IC | ESP32-S3 Pins |
| :--- | :--- | :--- |
| **Display (360×360 QSPI)** | SH8601 / ST77916 | `CS=14`, `PCLK=13`, `D0=15`, `D1=16`, `D2=17`, `D3=18`, `RST=21`, `BL=47` |
| **Capacitive Touch** | CST816S (`0x15`) | `SDA=11`, `SCL=12`, `RST=10`, `INT=9` |
| **Haptic Motor Driver** | DRV2605 (`0x5A`) | `SDA=11`, `SCL=12` (shared `I2C_NUM_0`) |
| **Rotary Encoder** | SSCM110100 Pulse Switch | `ECA=8`, `ECB=7` |
| **Audio DAC (3.5mm Jack)** | PCM5100A + CH445P | `BCLK=39`, `WS=40`, `DOUT=41`, `MUX_SEL=0 (HIGH)` |

---

## 🚀 Building & Flashing

Requires [PlatformIO](https://platformio.org/):

```bash
# Build firmware
pio run

# Upload to connected Waveshare Knob
pio run -t upload --upload-port /dev/cu.usbmodem121101

# Monitor serial logs
pio device monitor -p /dev/cu.usbmodem121101 -b 115200
```

---

## 🌐 REST API Endpoints

The embedded web server on port `80` exposes JSON endpoints:
- `GET /api/status` — Current station, ICY track title, playback state, volume, Wi-Fi status, presets list, and discovered AirPlay speakers.
- `POST /api/control` — Control playback (`action`: `play`, `pause`, `toggle`, `volume`, `station`, `speaker`, `scan_speakers`).
- `POST /api/presets` — Manage stream presets (`action`: `add`, `delete`, `reset`).
- `POST /api/wifi` — Scan (`action`: `scan`) or join (`action`: `connect`, `ssid`, `password`) Wi-Fi networks.

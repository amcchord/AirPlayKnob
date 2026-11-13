<div align="center">

# 🎛️ AirPlayKnob

**SomaFM Internet Radio to AirPlay 1 & 2 Streamer + 60 FPS Tactile Controller**  
*Built for the [Waveshare ESP32-S3-Knob-Touch-LCD-1.8](https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8)*

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32--S3-orange?logo=platformio)](https://platformio.org/)
[![LVGL](https://img.shields.io/badge/LVGL-8.4-10b981)](https://lvgl.io/)
[![AirPlay](https://img.shields.io/badge/AirPlay-RAOP%20%26%20AirPlay%202-38bdf8?logo=apple)](docs/AIRPLAY_PROTOCOL.md)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

<img src="docs/images/hero_banner.png" alt="AirPlayKnob UI Showcase" width="100%" />

**[Features](#-features)** • **[Screenshots](#-ui--visualizer-screenshots)** • **[Controls](#-touch--rotary-controls)** • **[Quick Start](#-quick-start)** • **[Architecture](docs/ARCHITECTURE.md)** • **[AirPlay Protocol](docs/AIRPLAY_PROTOCOL.md)** • **[Hardware Pinout](docs/HARDWARE.md)**

</div>

---

## ✨ Overview

**AirPlayKnob** turns the **[Waveshare ESP32-S3-Knob-Touch-LCD-1.8](https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8)** (dual-core 240 MHz ESP32-S3, 8 MB Octal PSRAM, 360×360 round QSPI display, capacitive touch, mechanical rotary encoder ring, DRV2605 haptic motor, and PCM5100A 3.5mm stereo DAC) into a standalone **SomaFM Internet Radio to AirPlay streamer**.

No phone, companion app, or external server is required: the knob connects directly to SomaFM (or a custom MP3 radio stream), decodes stereo audio on the ESP32-S3, fetches live album art, monitors LiPo battery & USB charge state, renders a **60 FPS** circular UI and 3D wireframe audio visualizer, and streams **AirPlay 1 / 2** audio (plus native Sonos radio control and an onboard **3.5mm stereo jack**). Sustained playback has been verified on Apple HomePod (`AudioAccessory5,1` over AirPlay 2), Sonos (`Five` native & AirPlay), and the local 3.5mm DAC; see the [network audit](docs/NETWORK_AUDIT_2026-11.md) and [regression audit](docs/AIRPLAY_REGRESSION_2026-11.md) for engineering diagnostics and receiver notes.

---

## 📸 UI & Visualizer Screenshots

<div align="center">
<table>
  <tr>
    <td align="center" width="33%">
      <img src="docs/images/screen_home.png" width="204" alt="Now Playing Screen" /><br/>
      <b>Now Playing (Home)</b><br/>
      <sub>Full-bleed 360×360 Album Art, bottom gradient scrim, Space Grotesk Bold track typography, and 360° perimeter volume ring.</sub>
    </td>
    <td align="center" width="33%">
      <img src="docs/images/screen_stations.png" width="204" alt="Stations Wheel" /><br/>
      <b>Stations Dial (Swipe Left)</b><br/>
      <sub>60 FPS virtualized 5-slot dial wheel with 30 curated SomaFM channels, color-coded genre pills, and haptic detents.</sub>
    </td>
    <td align="center" width="33%">
      <img src="docs/images/screen_speakers.png" width="204" alt="AirPlay Speakers Wheel" /><br/>
      <b>AirPlay Speakers (Swipe Right)</b><br/>
      <sub>Live mDNS-discovered HomePods, Sonos, Apple TVs, MacBooks, and onboard 3.5mm stereo DAC output.</sub>
    </td>
  </tr>
  <tr>
    <td align="center" width="33%">
      <img src="docs/images/screen_whitecap_1.png" width="204" alt="WhiteCap 3D Visualizer (Gold/Green)" /><br/>
      <b>WhiteCap 3D Visualizer (Swipe Up)</b><br/>
      <sub>60 FPS 34×26 wireframe terrain with live stereo oscilloscope front edge and orbiting 3D camera.</sub>
    </td>
    <td align="center" width="33%">
      <img src="docs/images/screen_whitecap_2.png" width="204" alt="WhiteCap 3D Visualizer (Cyan/Emerald)" /><br/>
      <b>WhiteCap Color Drift & Presets</b><br/>
      <sub>Continuous HSV color drift + 4 tap-selectable palettes rendered directly in internal DMA SRAM.</sub>
    </td>
    <td align="center" width="33%">
      <img src="docs/images/screen_settings.png" width="204" alt="Settings & Web Portal QR" /><br/>
      <b>Settings & Web QR (Swipe Down)</b><br/>
      <sub>On-device Wi-Fi scanner with touch keyboard, AirPlay rescan, and QR code for the embedded Web Portal.</sub>
    </td>
  </tr>
</table>
</div>

---

## 🚀 Features

- **📻 30 Pre-Loaded SomaFM Channels + Custom Streams**:
  - Ships with 30 curated SomaFM stations (*Groove Salad*, *Groove Salad Classic*, *Drone Zone*, *DEF CON Radio*, *Secret Agent*, *Space Station Soma*, *Underground 80s*, *Lush*, *Vaporwaves*, *Indie Pop Rocks!*, *Beat Blender*, *Deep Space One*, *Synphaera*, and more).
  - Automatic stream server failover (`ice6.somafm.com` $\to$ `ice2` $\to$ `ice4`) and live ICY `StreamTitle` metadata extraction.
  - Add, edit, or reorder up to 48 custom MP3 radio streams via the embedded Web UI (persisted in ESP32 NVS flash).
- **🔊 AirPlay 1 (RAOP) & AirPlay 2 Streaming Engine**:
  - **AirPlay 2 (`HomePod` / `HomePod mini` / `Apple TV` on OS 17–27+)**: Full HomeKit Transient Pair-Setup (`POST /pair-setup` TLV8 `M1..M4`, 3072-bit SRP-6a with SHA-512, HKDF-SHA-512 session keys), framed `ChaCha20-Poly1305` encrypted RTSP control & event channels, two-phase binary plist (`bplist00`) `SETUP`, and `ChaCha20-Poly1305` encrypted RTP audio frames.
  - **AirPlay 1 / RAOP (`Sonos`, `AirPort Express`, `ShairPort`, legacy receivers)**: Supports both `/auth-setup` Curve25519 ECDH (required by Sonos) and RSA-OAEP `AES-128-CBC` SDP `ANNOUNCE`.
  - **Buffering & Async Control**: **512 KiB PSRAM PCM ringbuffer** (~2.97 seconds of 44.1 kHz 16-bit stereo audio), **256 KiB MP3 buffer**, raw lwIP ICY TCP client (16-MSS window, immediate ACK, SACK-reneging guard, and DNS cache), automatic multi-AP BSSID stall failover with verified-healthy AP memory, 802.11n HT20 A-MPDU TX aggregation (`DSCP CS5` / `AC_VI`, 64-slot PSRAM `cache_tx_buf` staging), monotonic NTP wall-clock `PT=0xD4` RTP sync reference, lossless local-backpressure RTP hold (`flushUnsentPacket`), 512-packet UDP retransmission backlog with fast 4 ms NACK repair, PSRAM-stacked Core 0 NTP timing responder task, and asynchronous RTSP control with sequence-matched synchronous responses. Startup/rebuffering uses 1–1.5 seconds; artwork waits for >=2.0 seconds of PCM plus 32 KiB compressed reserve and yields below 1 second.
  - **Alphabetical Full-LAN Speaker Discovery**: Two-pass PSRAM mDNS discovery (`_airplay._tcp` + `_raop._tcp`) captures large multi-room speaker fleets and sorts AirPlay receivers case-insensitively (`A` -> `Z`, keeping `Local 3.5mm Jack` last).
- **Artwork and Network Diagnostics**:
  - Live album covers and station fallbacks work during native Sonos playback, with bounded background requests and large artwork HTTPS buffers in PSRAM.
  - Local socket/driver rejections are counted separately from attempts. Repairs are paced, and scan requests wait during AirPlay playback. See the [local network audit](docs/NETWORK_AUDIT_2026-11.md) for evidence and remaining limits.
- **Sonos Native Radio Playback**:
  - Sonos speakers normally fetch the selected station directly. The knob controls the station, volume, and play/pause, and reads track information from the speaker without relaying uncompressed audio over Wi-Fi.
  - For Sonos-only listening, disable the local 3.5mm output in the Web Portal. The buffer panel then shows **Audio buffered by Sonos**. Opening the visualizer or enabling the local jack starts an optional local audio monitor; the Sonos stream remains independent.
  - Other receivers continue using AirPlay. Sonos receivers that reject native control can fall back to the existing AirPlay path.
  - Turn off **Use direct radio playback on Sonos** in the portal, or send `{ "prefer_sonos_native": false }` to `/api/control`, to select standard AirPlay on Sonos. This preference is saved across restarts. Native radio success is separate from AirPlay qualification.
- **🎧 Simultaneous Onboard 3.5mm Stereo DAC Output**:
  - Drives the onboard **TI PCM5100A** I2S stereo DAC via the `CH445P` analog mux (`GPIO 0`) so you can plug headphones or powered speakers directly into the knob's 3.5mm jack alongside—or instead of—AirPlay streaming.
- **🔋 Battery & Charge State Telemetry**:
  - Samples `BATT_ADC` (`GPIO 1` / `ADC1_CHANNEL_0`) with ESP-IDF curve-fitting calibration and `USB_SERIAL_JTAG` SOF + voltage-step/slope charge detection.
  - Keeps the Home screen clean by showing the top-center battery pill only while actively charging (hidden once charged) or when battery drops below `20%` (tap when visible for detailed voltage/slope HUD, animated fill while charging, haptic plug/unplug and `15%`/`5%` low-battery alerts), plus full-time battery telemetry on Settings, the Web Portal, and `/api/status`.
- **🎨 60 FPS Circular Touchscreen UI & Old-School "WhiteCap" 3D Visualizer**:
  - **Full-Bleed 360×360 Live Album Art**: Queries the Apple iTunes Search API for live track cover art (`160×160` baseline JPEG decoded via hardware ROM `tjpgd`), falls back to official SomaFM channel art, and performs a smooth 2D bilinear upscale to `360×360` with a pre-multiplied bottom gradient scrim.
  - **60 FPS "WhiteCap" 3D Wireframe Terrain**: Renders an `884`-vertex (`34×26`) 3D mountain waterfall mesh driven by real-time signed stereo PCM samples on the front edge, Automatic Gain Control (AGC), an orbiting 3D perspective camera, and subtle HSV palette drift—rasterized directly into 240 MHz internal DMA SRAM with zero PSRAM bandwidth overhead.
  - **Custom `Space Grotesk` 4-bpp Anti-Aliased Typography**: Five custom-compiled LVGL font weights (`13px`, `15px`, `17px`, `20px`, `22px`) including full Latin punctuation and smart quotes.
  - **DRV2605 Haptic Feedback**: Physical tactile clicks and detents on every knob rotation, station tune, and gesture.
  - **180° Inverted Cable Orientation**: Display (`SH8601`), touch (`CST816S`), and rotary encoder are oriented so the USB-C power cable exits cleanly from the **top (12 o'clock)**.
- **🌐 Embedded Web Portal & Wireless OTA Updates**:
  - Dark-mode responsive Web UI at `http://airplayknob.local/` (or device IP) for live playback control, station preset management, speaker switching, Wi-Fi configuration, and `.bin` firmware upload.
  - Native wireless `ArduinoOTA` support on port `3232`.

---

## 🧭 Touch & Rotary Controls

### 4-Way Swipe Navigation (from Home Screen)

| Gesture / Input | Action |
| :--- | :--- |
| **Swipe Left** (`←`) | Open **Stations Dial** (30 SomaFM channels; turn knob or drag to scroll, tap center pill to tune) |
| **Swipe Right** (`→`) | Open **AirPlay Speakers Dial** (turn knob or drag to scroll, tap center pill to route audio) |
| **Swipe Up** (`↑`) | Launch full-screen **60 FPS WhiteCap 3D Visualizer** (tap screen to cycle color palettes; swipe down to exit) |
| **Swipe Down** (`↓`) | Open **Settings & Web Portal QR** (scan Wi-Fi networks with on-screen keyboard or rescan AirPlay speakers) |
| **Rotate Outer Knob** | Adjusts **Volume** (`0–100%`) on Home/Visualizer (with floating HUD pill), or scrolls the **Stations / Speakers** dial at 60 FPS |
| **Tap Center Artwork** | Toggle **Play / Pause** with haptic click |
| **Tap Bottom Metadata Line** | Cycle rotary knob mode on Home screen (`VOL` $\to$ `TUNE STATION` $\to$ `AIRPLAY SPEAKER`) |

---

## 🛠️ Hardware Pinout

Built for the **[Waveshare ESP32-S3-Knob-Touch-LCD-1.8](https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8)**. See [`docs/HARDWARE.md`](docs/HARDWARE.md) for full electrical and driver details.

| Subsystem | Controller / IC | ESP32-S3 Pins |
| :--- | :--- | :--- |
| **Display (360×360 QSPI)** | SH8601 / ST77916 | `CS=14`, `PCLK=13`, `D0=15`, `D1=16`, `D2=17`, `D3=18`, `RST=21`, `BL=47` |
| **Capacitive Touch** | CST816S (`0x15`) | `SDA=11`, `SCL=12`, `RST=10`, `INT=9` |
| **Haptic Motor Driver** | DRV2605 (`0x5A`) | `SDA=11`, `SCL=12` (shared `I2C_NUM_0`) |
| **Rotary Encoder** | SSCM110100 Pulse Switch | `ECA=8`, `ECB=7` |
| **Audio DAC (3.5mm Jack)** | PCM5100A + CH445P | `BCLK=39`, `WS=40`, `DOUT=41`, `MUX_SEL=0 (HIGH)` |
| **Battery / Power Sense** | `ADC1_CHANNEL_0` (`2:1` divider) | `BATT_ADC=1` |

---

## ⚡ Quick Start

### 1. Prerequisites
Install **[PlatformIO](https://platformio.org/)** (Core 6.1.19 or later). The pioarduino platform is pinned to **55.03.38-1** (Arduino 3.3.8 / IDF 5.5.4), with **LVGL 8.4.0** and the ESP32-S3 partition table configured by [`platformio.ini`](platformio.ini). Wi-Fi credentials are optional and are never needed for a clean build.

### 2. Configure Wi-Fi (Optional Compile-Time or On-Screen)
You can connect to Wi-Fi in either of two ways:
1. **Directly on the Touchscreen (No code changes needed)**: Flash the firmware, **Swipe Down** on the screen, tap **Wi-Fi Scan**, select your network, and enter your password on the touch keyboard. Credentials are saved automatically to ESP32 NVS flash.
2. **Compile-Time (`include/secrets.h`)**: Copy [`include/secrets.example.h`](include/secrets.example.h) to `include/secrets.h` (which is gitignored) and set your SSID and password:
   ```bash
   cp include/secrets.example.h include/secrets.h
   ```
   ```cpp
   // include/secrets.h
   #define DEFAULT_WIFI_SSID  "YourNetworkSSID"
   #define DEFAULT_WIFI_PASS  "YourNetworkPassword"
   ```

### 3. Flash a Pre-Built Release or Build from Source

#### Option A: Pre-Built GitHub Release Binaries (No Toolchain Needed)
Download the latest release assets from **[GitHub Releases](https://github.com/amcchord/AirPlayKnob/releases)**:
- **Fresh Board (`AirPlayKnob-<version>-factory.bin`)** — Combined bootloader + partition table + `boot_app0` + application image flashed at `0x0`:
  ```bash
  esptool --chip esp32s3 --port /dev/cu.usbmodem* --baud 921600 --no-stub write-flash 0x0 AirPlayKnob-v1.1.0-factory.bin
  ```
- **Existing Board / Wireless Browser OTA (`AirPlayKnob-<version>-ota.bin`)** — Upload directly at `http://airplayknob.local/` (**Firmware OTA Update** card) to preserve saved Wi-Fi credentials, presets, and speaker pairings.

#### Option B: Build & Flash from Source via PlatformIO
```bash
# Run host protocol, buffer, network-policy, and battery regression tests
for f in tests/test_*.py; do python3 "$f" || exit 1; done

# Compile firmware
pio run

# Flash over USB-C (use --no-stub on ESP32-S3 USB-Serial/JTAG ports if needed)
pio run -t upload

# Monitor serial output & interactive debug CLI (115200 baud)
pio device monitor -b 115200
```

### 4. Wireless OTA Updates
Once connected to Wi-Fi, you can push firmware updates wirelessly via `ArduinoOTA` or upload `.bin` files in the browser at `http://airplayknob.local/`:
```bash
pio run -t upload --upload-port airplayknob.local
```

---

## 🌐 REST API & Serial Debug CLI

### HTTP JSON Endpoints (`Port 80`)
- **`GET /`** — Embedded responsive dark-mode Web Portal UI.
- **`GET /api/status`** — Live JSON state (build ID, running OTA partition, heap/PSRAM headroom, current station, ICY track title, album art state, playback status, volume, Wi-Fi IP/SSID/BSSID/PHY/channel, PCM & MP3 buffer fills, driver & RAOP TX/repair/gap counters, calibrated battery telemetry, presets array, and alphabetically sorted AirPlay speakers).
- **`POST /api/control`** — Control playback, routing, and UI:
  ```json
  { "preset": 4, "volume": 28, "speaker": 2, "toggle_whitecap": true }
  ```
- Use `{ "toggle_play": true }` to toggle playback, `{ "prefer_sonos_native": false }` to force AirPlay on Sonos, and `{ "also_local_dac": false }` for Sonos-only listening.
- **`POST /api/presets`** — Add, update, delete, or reset station presets (`action`: `"add" | "update" | "delete" | "reset"`).
- **`POST /api/wifi`** — Save and connect to a new Wi-Fi network (`{ "ssid": "...", "pass": "..." }`).
- **`GET /api/artwork.jpg`** — Returns the active decoded track/station cover JPEG (`404` when using procedural fallback).
- **`GET /api/screenshot.bmp`** — Streams a live `360×360` 24-bit BMP capture of the display framebuffer.
- **`POST /api/ota`** — Multipart `.bin` firmware upload for browser-based OTA updates.

### USB Serial Debug Commands (`115200 baud`)
Send newline-terminated commands over USB-Serial/JTAG:
- `STATUS` — Prints active tab, FPS, battery/charge summary, audio ringbuffer fill (`BufMs` / `Mp3B`), Wi-Fi link & TX counters, task stack high-water marks, heap/PSRAM free bytes, and all discovered AirPlay speakers.
- `BATTERY` (`BATT`) — Prints detailed calibrated battery telemetry (`state`, `pct`, `mv`, `raw_mv`, `raw_adc`, `slope`, `usb_host`, `low`/`crit` flags).
- `FPS` — Prints instantaneous display flush FPS and frame duration in microseconds.
- `TAB <0..3>` — Switches active UI screen (`0`=Home, `1`=Stations, `2`=Speakers, `3`=Settings).
- `SWIPE <L|R|U|D>` — Simulates a 4-way touch swipe gesture.
- `WHITECAP <0|1>` — Toggles the 60 FPS WhiteCap 3D visualizer.
- `ROTATE <delta>` — Simulates rotary encoder rotation (`e.g., ROTATE 1`, `ROTATE -1`).
- `VOL <0..100>` — Sets volume percentage.
- `PRESET <idx>` — Tunes and starts station preset `<idx>`.
- `SPEAKER <idx>` — Routes audio to discovered speaker index `<idx>`.
- `SCAN` — Queues an mDNS AirPlay speaker discovery scan (runs immediately when paused, deferred during active AirPlay).
- `TXPWR <8..84>` / `PROTO <bg|bgn>` / `BSSID <mac> [ch]` — Live Wi-Fi RF/AP diagnostic controls.
- `REBOOT` — Flushes pending NVS preferences and restarts the ESP32-S3 cleanly.
- `SCREENSHOT` — Dumps a Base64-encoded RGB565 screenshot over serial (`===SCREENSHOT_BEGIN 180 180===`).

---

## 📂 Project Structure & Deep-Dive Docs

```text
AirPlayKnob/
├── docs/
│   ├── ARCHITECTURE.md            # Dual-core FreeRTOS tasks, SRAM vs PSRAM, 60 FPS WhiteCap engine
│   ├── AIRPLAY_PROTOCOL.md        # AirPlay 1 (RAOP) & AirPlay 2 (HomeKit SRP-6a + ChaCha20-Poly1305)
│   ├── HARDWARE.md                # Waveshare ESP32-S3-Knob pinout, SH8601 QSPI, CST816S, DRV2605, DAC, Battery
│   ├── NETWORK_AUDIT_2026-11.md   # Wi-Fi MAC TX staging, RTP clock-sync, and sustained HomePod audit
│   ├── AIRPLAY_REGRESSION_2026-11.md # ALAC, RTSP CSeq, and receiver interoperability engineering log
│   ├── operations/RUNBOOK.md      # Build, test, USB/OTA deployment, and diagnostic runbook
│   └── images/                    # Live device screenshots & UI showcase banner
├── include/
│   ├── app_state.h                # Global state mutex, presets, speaker table, waveform & Wi-Fi state
│   ├── artwork_fetcher.h          # 360x360 live album art & SomaFM artwork pipeline
│   ├── audio_buffer_config.h      # Unified 512 KiB PCM / 256 KiB MP3 ring & gating thresholds
│   ├── battery_monitor.h          # Calibrated LiPo SoC curve, hysteresis, and USB/wall charge estimator
│   ├── knob_ui.h                  # Circular LVGL UI & 60 FPS WhiteCap 3D visualizer interface
│   ├── lv_conf.h                  # LVGL 8.4 configuration optimized for ESP32-S3 DMA + PSRAM
│   ├── minimp3.h                  # Single-header MP3 decoder
│   ├── network_diagnostics.h      # Lock-free Wi-Fi driver TX submission/rejection & UDP stats
│   ├── plist_bin.h                # Lightweight Apple Binary Plist (bplist00) parser & builder
│   ├── radio_streamer.h           # Raw lwIP ICY TCP client, MP3 decode task, and audio output task
│   ├── raop_AnySpeaker.h          # AirPlay 1 & AirPlay 2 RTSP/RTP client class
│   ├── secrets.example.h          # Template for optional compile-time Wi-Fi credentials
│   ├── somafm_presets.h           # 30 curated SomaFM station definitions & theme colors
│   ├── sonos_radio.h              # Direct UPnP/SOAP native radio controller for Sonos speakers
│   └── ui_fonts.h                 # Custom 4-bpp Space Grotesk LVGL font declarations
├── src/
│   ├── main.cpp                   # Boot sequence, Wi-Fi TX init, BSSID scoring, mDNS discovery, OTA, CLI
│   ├── network_diagnostics.cpp    # Linker-wrapped esp_wifi_internal_tx telemetry counters
│   ├── audio/
│   │   ├── artwork_fetcher.cpp    # iTunes Search API + PSRAM TLS + JPEG decode + bilinear 360x360 upscale
│   │   ├── plist_bin.c            # Binary plist encoder/decoder for AirPlay 2 SETUP handshakes
│   │   ├── radio_streamer.cpp     # IcyRawTcp client, 512 KiB PSRAM ringbuffer, lock-free RTP sender loop
│   │   ├── raop_AnySpeaker.cpp    # AirPlay 1 & 2 crypto, SRP-6a, ChaCha20-Poly1305, wall-clock NTP sync
│   │   └── sonos_radio.cpp        # Sonos AVTransport / RenderingControl SOAP client
│   ├── bsp/                       # Board Support Package (SH8601 QSPI, CST816S, DRV2605, I2S DAC, Battery ADC)
│   ├── crypto/                    # Curve25519 Donna implementation for Sonos /auth-setup
│   └── ui/
│       ├── knob_ui.cpp            # 4-way swipe UI, O(1) virtualized wheels, battery HUD, 60 FPS WhiteCap
│       ├── web_portal.cpp         # Embedded Web Server, dark-mode SPA, REST API, and BMP screenshot
│       └── fonts/                 # Custom Space Grotesk 4-bpp C font bitmaps (13px..22px)
├── tests/                         # Host C++/Python regression suites (ALAC, RTSP, PCM ring, network, battery)
├── partitions.csv                 # Custom 16MB flash layout with dual 6MB OTA app partitions
└── platformio.ini                 # PlatformIO build configuration for ESP32-S3 Octal PSRAM
```

---

## 📄 License

Released under the [MIT License](LICENSE). Feel free to fork, remix, and build your own tactile streaming controllers!

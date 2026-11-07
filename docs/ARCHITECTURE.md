# System Architecture & Performance Engineering

AirPlayKnob runs a simultaneous real-time internet radio MP3 decoder, an AirPlay 1 / AirPlay 2 cryptographic RTP transmitter, a live album art HTTP/JPEG pipeline, an embedded web server, and a **60 FPS** 360×360 circular touchscreen UI on a single **ESP32-S3** (240 MHz dual-core Xtensa LX7 with 512 KB internal SRAM and 8 MB Octal PSRAM).

This document describes CPU cores, memory, DMA buses and FreeRTOS tasks. Sustained AirPlay reliability remains under investigation; the latest HomePod test reproduced audible interruptions and source underruns.

---

## 1. Dual-Core FreeRTOS Task Partitioning

The current source creates these tasks (higher numbers have higher priority):

| Task | Core | Priority | Work |
| --- | ---: | ---: | --- |
| `radio_http` | 0 | 4 | HTTP/ICY ingest, MP3 decoding, PCM buffering |
| `raop_ntp` | 0 | 5 | Dedicated timing requests/replies |
| `net_mgr` | 0 | 3 | Connection management, discovery and ArduinoOTA |
| `art_fetch` | 0 | 1 | Bounded artwork lookup/download and JPEG decoding |
| `raop_tx` | 1 | 6 | RTP framing/pacing, repair queue, receiver control, local DAC |
| `LVGL` | 1 | 5 | UI rendering |
| Arduino `loop()` | 1 | Framework default | Input, web portal and preferences |

The framework also runs its own Wi-Fi/lwIP tasks. MP3 input uses a **256 KB
PSRAM compressed buffer** plus a **512 KiB PCM ring** (~2.97 seconds). Incoming
HTTP and outgoing audio therefore have separate tasks and bounded storage.
`include/audio_buffer_config.h` defines capacity and thresholds together.
Playback/rebuffering requires 1 second PCM with at least 16 KiB MP3 available,
otherwise 1.5 seconds. Artwork starts with 2 seconds PCM plus 32 KiB MP3 and
aborts payload writes below 1 second PCM. Native Sonos-only playback bypasses
these local buffer requirements. Reducing PCM releases PSRAM, not the driver's
internal/DMA memory, and did not resolve the sustained HomePod failure.

Timing uses its own socket/task so it can respond while the sender handles
control traffic. However, there is **no separate volume worker** in the
current implementation: RTSP writes, metadata and artwork forwarding run on
`raop_tx`. Commands can be asynchronous with respect to replies while their
TCP writes still block. The earlier architecture description overstated this
separation. Remaining scheduling/queue diagnostics and the measured local
rejections are documented in the [network audit](NETWORK_AUDIT_2026-11.md).

Speaker and off-channel Wi-Fi scans are deferred during controller audio
playback, including transitions from native Sonos radio. Native-only Sonos
playback owns its stream on the receiver, so its zero local PCM buffer does
not gate artwork and queued scans. Artwork bulk TLS allocations use PSRAM only
for `art_fetch`; other tasks retain the SDK allocator.

---

## 2. Memory Hierarchy: Internal SRAM vs. Octal PSRAM

On the ESP32-S3, the QSPI display bus (`SH8601`) and Octal PSRAM share internal bus bandwidth when PSRAM is accessed heavily. Placing framebuffers or high-frequency line drawing in PSRAM causes cache misses and limits framerate to ~18–25 FPS.

AirPlayKnob strictly separates allocations between **Internal SRAM** (zero wait-state, DMA-capable) and **8 MB PSRAM** (high capacity, sequential streaming):

| Allocation | Size | Memory Region | Purpose |
| :--- | :--- | :--- | :--- |
| **LVGL Double DMA Draw Buffers** | `2 × (360 × 40 × 2 B)` = `57.6 KB` | **Internal DMA SRAM** | Ping-pong 40-line strip buffers; CPU renders strip `N+1` while SPI DMA transmits strip `N` at 80 MHz |
| **WhiteCap 3D Mesh & Vertex Tables** | `~4.6 KB` (`34×26` grid) | **Internal SRAM** | `s_wc_wave`, `s_wc_vx`, `s_wc_vy`, row Y-bounds, and RGB565 palette LUTs |
| **Wi-Fi MAC & lwIP Buffers** | `~65 KB` | **Internal SRAM** | Initialized at boot before PSRAM/LVGL allocations to guarantee contiguous internal DMA memory |
| **Decoded PCM Ringbuffer** | `524,288 B` (`512 KiB`) | **Octal PSRAM** | 2.97 seconds of decoded 44.1 kHz 16-bit stereo audio; one frame reserved by the ring |
| **MP3 Stream Input Buffer** | `262,144 B` (`256 KiB`) | **Octal PSRAM** | Compressed network-jitter reserve feeding `minimp3` |
| **RAOP Retransmit Backlog** | `512 × 1,484 B` (`~760 KB`) | **Octal PSRAM** | Circular buffer of recent encrypted RTP audio packets for UDP `PT=0x55` retransmit requests |
| **360×360 Album Art Canvas** | `259,200 B` (`253 KB`) | **Octal PSRAM** | Bilinear-upscaled RGB565 cover art + pre-composited bottom typography scrim |
| **Screen Capture Mirror** | `259,200 B` (`253 KB`) | **Octal PSRAM** | Live copy of the 360×360 display updated during DMA flush for `/api/screenshot.bmp` and `SCREENSHOT` |

---

## 3. 60 FPS UI & "WhiteCap" 3D Visualizer Engine

### O(1) Virtualized 5-Slot Dial Wheels
Standard LVGL scrollable flex lists with 30+ items trigger layout recalculations and full-screen redraws on every scroll tick. Instead, both the **Stations** screen (`TAB_STATIONS`) and **AirPlay Speakers** screen (`TAB_SPEAKERS`) use a **virtualized 5-slot circular wheel** (`WHEEL_SLOTS = 5`):
- Only 5 fixed `lv_obj_t` pill containers exist in memory, pre-positioned along the circular display curvature (`widths = {252, 282, 300, 282, 252}`).
- Rotating the physical encoder or dragging vertically updates `s_station_cursor` / `s_speaker_cursor` in **O(1) time** with zero heap allocations and zero layout recalculations, maintaining a locked **60 FPS** scroll rate.

### Zero-PSRAM "WhiteCap" 3D Terrain Renderer
Inspired by the classic early-2000s SoundSpectrum *WhiteCap* wireframe visualizer, the visualizer renders a `34 × 26` 3D terrain mesh (`884` vertices, `~1,700` wireframe segments) at **60 FPS**:
1. **Once-Per-Frame Simulation (`LV_EVENT_DRAW_MAIN_BEGIN`)**:
   - Shifts the 26-row terrain waterfall history back by one row (`r = 0 -> 1 -> ... -> 25`).
   - Computes **Row 0** directly from live signed stereo PCM samples (`g_app.wave_l`, `g_app.wave_r`) scaled by an Automatic Gain Control (AGC) envelope so the front-most green line acts as a real-time oscilloscope that propagates backward into the 3D mountain ridges.
   - Projects all `34 × 26` 3D coordinates through a smoothly orbiting 3D camera (`yaw` + `pitch` + perspective divide) into internal SRAM screen coordinates (`s_wc_vx`, `s_wc_vy`) and records per-row vertical bounding boxes (`s_wc_row_ymin[r]`, `s_wc_row_ymax[r]`).
2. **Per-Strip Direct SRAM Drawing (`LV_EVENT_DRAW_MAIN`)**:
   - As LVGL sweeps its 40-line internal-SRAM DMA strip down the dirty region (`Y = 92..319`), the renderer checks each row's `[ymin, ymax]` against the strip's `[by1, by2]`.
   - Rows outside the 40-line strip are rejected in a single integer comparison; intersecting segments are rasterized with a 2px-thick Bresenham line drawer (`wc_strip_line`) directly into internal SRAM at 240 MHz before DMA pushes the strip over 80 MHz QSPI.

---

## 4. Live Album Art Pipeline

`src/audio/artwork_fetcher.cpp` runs as a background task on Core 0:
1. **Track Change Detection**: Watches `g_app.track_title` (`Artist - Track`) and current SomaFM preset ID.
2. **Two-Tier Artwork Resolution**:
   - **Tier 1 (Live Track Art)**: Queries the Apple iTunes Search API (`https://itunes.apple.com/search?term=...&media=music&limit=1`), extracts `artworkUrl100`, and rewrites it to `160x160bb.jpg` (**baseline JPEG**—critical because Apple's `300x300bb.jpg` CDN serves progressive JPEGs, which the ESP32 ROM `tjpgd` decoder rejects).
   - **Tier 2 (SomaFM Channel Art)**: Falls back to official SomaFM station art (`https://somafm.com/img/<id>120.jpg`), and finally to a procedural vector vinyl/geometric cover generator.
3. **Bilinear 360×360 Upscale + Bottom Scrim**:
   - Decodes the JPEG into a temporary RGB565 buffer using `esp_rom_tjpgd`.
   - Performs a smooth 2D bilinear upscale to fill the entire `360×360` round screen (`ARTWORK_SIZE = 360`).
   - Pre-multiplies a smooth dark gradient scrim across the bottom `Y = 175..359` directly into the RGB565 pixel buffer so the white `Space Grotesk` track typography stays legible with zero alpha-blending overhead during UI rendering.

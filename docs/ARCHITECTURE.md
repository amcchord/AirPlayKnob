# System Architecture & Performance Engineering

AirPlayKnob runs a simultaneous real-time internet radio MP3 decoder, an AirPlay 1 / AirPlay 2 cryptographic RTP transmitter, a live album art HTTP/JPEG pipeline, an embedded web server, and a **60 FPS** 360×360 circular touchscreen UI on a single **ESP32-S3** (240 MHz dual-core Xtensa LX7 with 512 KB internal SRAM and 8 MB Octal PSRAM).

This document explains how CPU cores, memory hierarchies, DMA buses, and FreeRTOS tasks are partitioned so UI animations never stutter and audio never drops a frame.

---

## 1. Dual-Core FreeRTOS Task Partitioning

```
+-----------------------------------------------------------------------------------+
|                               ESP32-S3 (240 MHz)                                  |
+-----------------------------------------+-----------------------------------------+
|            CORE 0 (Network/IO)          |         CORE 1 (Audio DSP & UI)         |
+-----------------------------------------+-----------------------------------------+
| - Wi-Fi MAC / lwIP TCP/IP Stack         | - radio_decode_task (Priority 5)        |
| - raop_time (Priority 19, NTP UDP Sync) |   HTTP ICY read -> minimp3 -> PCM ring  |
| - radio_out_task (Priority 6)           |   -> stereo VU & waveform extraction    |
|   RTP ALAC framing, ChaCha20/AES enc,   |                                         |
|   UDP send + Local I2S DAC write        | - lvgl_port_task (Priority 4)           |
| - raop_vol_task (Priority 3)            |   60 FPS UI & WhiteCap 3D wireframe     |
|   Async RTSP SET_PARAMETER volume       |   DMA strip rendering                   |
| - net_mgr (Priority 3)                  |                                         |
|   Wi-Fi STA, mDNS (_raop._tcp), OTA     | - Arduino loop() (Priority 1)           |
| - artwork_task (Priority 1)             |   Rotary encoder delta, touch swipes,   |
|   iTunes Search + SomaFM JPEG decode    |   Web Portal HTTP polling, Serial CLI   |
+-----------------------------------------+-----------------------------------------+
```

### Why This Partitioning Matters
1. **Zero RTSP Blocking in the Audio Path**: Turning the physical rotary knob fires dozens of volume updates per second. Instead of sending synchronous RTSP `SET_PARAMETER` TCP packets from the audio output task, `radio_out_task` notifies a dedicated low-priority worker (`raop_vol_task`) that coalesces and transmits encrypted RTSP volume commands asynchronously.
2. **Microsecond-Accurate NTP Timing (`raop_time`)**: Apple HomePods (especially on HomePod OS 17–27+) send UDP NTP timing requests (`PT=0x52`) every ~3 seconds and tear down the stream if replies are delayed while TCP/HTTP is busy. A dedicated priority-19 task (`raop_time`) services the timing UDP socket with sub-millisecond turnaround.
3. **Decoupled MP3 Ingest (`radio_decode_task`) vs RTP Pacing (`radio_out_task`)**: Network jitter on the incoming SomaFM HTTP stream is absorbed by a **2 MB PSRAM PCM ringbuffer** (~11.6 seconds of 44.1 kHz 16-bit stereo audio) plus a **64 KB TCP socket receive buffer**, while `radio_out_task` clocks out exact 352-sample RTP frames locked to the `esp_timer_get_time()` microsecond clock.

---

## 2. Memory Hierarchy: Internal SRAM vs. Octal PSRAM

On the ESP32-S3, the QSPI display bus (`SH8601`) and Octal PSRAM share internal bus bandwidth when PSRAM is accessed heavily. Placing framebuffers or high-frequency line drawing in PSRAM causes cache misses and limits framerate to ~18–25 FPS.

AirPlayKnob strictly separates allocations between **Internal SRAM** (zero wait-state, DMA-capable) and **8 MB PSRAM** (high capacity, sequential streaming):

| Allocation | Size | Memory Region | Purpose |
| :--- | :--- | :--- | :--- |
| **LVGL Double DMA Draw Buffers** | `2 × (360 × 40 × 2 B)` = `57.6 KB` | **Internal DMA SRAM** | Ping-pong 40-line strip buffers; CPU renders strip `N+1` while SPI DMA transmits strip `N` at 80 MHz |
| **WhiteCap 3D Mesh & Vertex Tables** | `~4.6 KB` (`34×26` grid) | **Internal SRAM** | `s_wc_wave`, `s_wc_vx`, `s_wc_vy`, row Y-bounds, and RGB565 palette LUTs |
| **Wi-Fi MAC & lwIP Buffers** | `~65 KB` | **Internal SRAM** | Initialized at boot before PSRAM/LVGL allocations to guarantee contiguous internal DMA memory |
| **Decoded PCM Ringbuffer** | `2,097,152 B` (`2.0 MB`) | **Octal PSRAM** | 11.6 seconds of decoded 44.1 kHz 16-bit stereo audio |
| **MP3 Stream Input Buffer** | `65,536 B` (`64 KB`) | **Octal PSRAM** | Sliding frame buffer for `minimp3` |
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

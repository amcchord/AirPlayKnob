# System Architecture & Performance Engineering

AirPlayKnob runs a simultaneous real-time internet radio MP3 decoder, an AirPlay 1 / AirPlay 2 cryptographic RTP transmitter, a live album art HTTP/JPEG pipeline, a calibrated LiPo battery & charge-state monitor, an embedded web server, and a **60 FPS** 360×360 circular touchscreen UI on a single **ESP32-S3** (240 MHz dual-core Xtensa LX7 with 512 KB internal SRAM and 8 MB Octal PSRAM).

This document describes CPU cores, memory hierarchy, DMA buses, and FreeRTOS task design as shipped in **v1.1.0**.

---

## 1. Dual-Core FreeRTOS Task Partitioning

The firmware partitions real-time networking, audio decoding, RTP pacing, and 60 FPS UI rendering across both LX7 cores (higher numbers indicate higher FreeRTOS priority):

| Task | Core | Priority | Stack Allocation | Work |
| :--- | ---: | ---: | :--- | :--- |
| `raop_ntp` | 0 | 5 | `4 KiB` PSRAM stack + Internal TCB | Dedicated persistent AirPlay UDP NTP timing responder (`PT=0x52` / `0x53`) |
| `radio_http` | 0 | 4 | `28 KiB` PSRAM stack + Internal TCB | Raw lwIP `IcyRawTcp` HTTP/ICY ingest, `minimp3` decoding, 512 KiB PCM ring fill |
| `net_mgr` | 0 | 3 | `8 KiB` Internal SRAM | Wi-Fi connection, BSSID stall failover, two-pass PSRAM mDNS discovery, `ArduinoOTA` |
| `raop_tx` | 1 | 6 | `16 KiB` PSRAM stack + Internal TCB | Lock-free cached RTP framing/pacing, lossless packet hold, NACK repair, RTSP control, local I2S DAC |
| `LVGL` | 1 | 5 | `6 KiB` Internal SRAM | 60 FPS `SH8601` QSPI strip rendering & WhiteCap 3D wireframe engine |
| Arduino `loop()` | 1 | 1 | Framework Internal SRAM | Rotary/touch input, 1 Hz `battery_monitor`, Web Portal (`Port 80`), NVS preferences |
| `art_fetch` | 1 | 1 | `16 KiB` PSRAM stack + Internal TCB | Bounded iTunes/SomaFM HTTPS lookup/download (PSRAM TLS) & ROM `tjpgd` decode |

### HTTP Ingest, Pacing & Lock-Free Coordination
- **Raw lwIP HTTP Streamer (`IcyRawTcp` in `src/audio/radio_streamer.cpp`)**: Bypasses Arduino `WiFiClient` overhead and uses raw lwIP TCP callbacks on the `tcpip_thread` with a **`16 × MSS` (`22,976 B`) receive window**, `TF_SACK` cleared to avoid SACK-reneging stalls across lossy Wi-Fi hops, immediate `tcp_output()` ACKs after `tcp_recved()`, cached DNS resolution, and multi-AP BSSID stall scoring/failover (`pref_bssid` persisted in NVS after 45 seconds of clean streaming).
- **Decoupled PSRAM Audio Buffers (`include/audio_buffer_config.h`)**: Incoming compressed MP3 data lands in a **256 KiB PSRAM buffer**, feeding `minimp3` into a **512 KiB PSRAM PCM ringbuffer** (~2.97 seconds of 44.1 kHz 16-bit stereo audio). Startup/rebuffering requires 1.0 second of PCM with >=16 KiB MP3 reserve (or 1.5 seconds otherwise). Artwork starts only when >=2.0 seconds of PCM and >=32 KiB MP3 are buffered, yielding payload writes if PCM dips below 1.0 second.
- **Lock-Free `20 ms` State Caching on `raop_tx`**: `raop_sender_task` runs a `2,000 Hz` (`vTaskDelay(0/1)`) pacing loop to hit exact `7.98 ms` (`352`-frame) RTP deadlines and `4.0 ms` NACK repairs. Rather than acquiring `g_app.mutex` on every iteration (which previously risked stalling behind UI/Web Portal readers), `raop_tx` refreshes a local control snapshot every `20 ms` with a non-blocking `0`-tick `xSemaphoreTake` and responds immediately to lock-free atomic wake flags (`preset_changed`, `speaker_changed`, `play_Pause_req`).
- **Deferred Scans & Native Sonos Bypass**: Explicit speaker discovery and off-channel Wi-Fi scans are automatically queued while controller AirPlay audio is active. When streaming via native Sonos UPnP/SOAP (`SonosRadioClient`) with WhiteCap and local 3.5mm output off, the speaker fetches the radio stream directly and local MP3/PCM decoding stays idle.

---

## 2. Memory Hierarchy: Internal SRAM vs. Octal PSRAM

On the ESP32-S3, the QSPI display bus (`SH8601`), Wi-Fi MAC DMA descriptors, and Octal PSRAM contend if internal DMA SRAM is exhausted or if high-frequency pixel drawing targets PSRAM.

AirPlayKnob strictly partitions **Internal DMA SRAM** (`~92 KB` free at runtime, `~81.9 KB` largest contiguous block during full AirPlay 2 + UI load) from **8 MB Octal PSRAM**:

| Allocation | Size | Memory Region | Purpose |
| :--- | :--- | :--- | :--- |
| **Wi-Fi MAC RX/TX DMA Buffers** | `16` static RX, `64` dyn RX (`ba_win=12`), `48` dyn TX | **Internal DMA SRAM** | Tuned via `__wrap_esp_wifi_init` before LVGL/PSRAM init so bursty HTTP TCP + 125 pkt/s RTP traffic never starves DMA |
| **Wi-Fi PSRAM TX Staging (`cache_tx_buf`)** | `64` slots (`~96 KB` capacity) | **Octal PSRAM** | Stages outbound lwIP `PBUF_RAM` frames (`~500 ms` AirPlay RTP cushion) into MAC TX with `0 B` Internal SRAM cost |
| **LVGL Double DMA Draw Buffers** | `2 × (360 × 16 × 2 B)` = `23.0 KB` | **Internal DMA SRAM** | Ping-pong 16-line strip buffers; CPU renders strip `N+1` while SPI2 DMA transmits strip `N` at 80 MHz |
| **WhiteCap 3D Mesh & Vertex Tables** | `~4.6 KB` (`34×26` grid) | **Internal SRAM** | `s_wc_wave`, `s_wc_vx`, `s_wc_vy`, row Y-bounds, and RGB565 palette LUTs |
| **Audio/Art Task Stacks & Decoder Structs** | `~100 KB` (`radio_http`, `raop_tx`, `raop_ntp`, `art_fetch`, `mp3dec_t`, `RaopClient`) | **Octal PSRAM** | Frees >75 KB of Internal SRAM while keeping FreeRTOS TCBs in Internal SRAM |
| **Decoded PCM Ringbuffer** | `524,288 B` (`512 KiB`) | **Octal PSRAM** | 2.97 seconds of decoded 44.1 kHz 16-bit stereo audio |
| **MP3 Stream Input Buffer** | `262,144 B` (`256 KiB`) | **Octal PSRAM** | Compressed network-jitter reserve feeding `minimp3` |
| **RAOP Retransmit Backlog** | `512 × 1,484 B` (`~760 KB`) | **Octal PSRAM** | Circular buffer of recent encrypted RTP audio packets (~4.1 s) for UDP `PT=0x55` NACK repair |
| **360×360 Album Art Canvas** | `259,200 B` (`253 KB`) | **Octal PSRAM** | Bilinear-upscaled RGB565 cover art + pre-composited bottom typography scrim |
| **Screen Capture Mirror** | `259,200 B` (`253 KB`) | **Octal PSRAM** | Live copy of the 360×360 display updated during DMA flush for `/api/screenshot.bmp` and `SCREENSHOT` |

---

## 3. 60 FPS UI & "WhiteCap" 3D Visualizer Engine

### O(1) Virtualized 5-Slot Dial Wheels
Standard LVGL scrollable flex lists with 30+ items trigger layout recalculations and full-screen redraws on every scroll tick. Instead, both the **Stations** screen (`TAB_STATIONS`) and **AirPlay Speakers** screen (`TAB_SPEAKERS`) use a **virtualized 5-slot circular wheel** (`WHEEL_SLOTS = 5`):
- Only 5 fixed `lv_obj_t` pill containers exist in memory, pre-positioned along the circular display curvature (`widths = {252, 282, 300, 282, 252}`).
- Rotating the physical encoder or dragging vertically updates `s_station_cursor` / `s_speaker_cursor` in **O(1) time** with zero heap allocations and zero layout recalculations, maintaining a locked **60 FPS** scroll rate across 30+ stations and up to 48 alphabetically sorted AirPlay speakers.

### Zero-PSRAM "WhiteCap" 3D Terrain Renderer
Inspired by the classic early-2000s SoundSpectrum *WhiteCap* wireframe visualizer, the visualizer renders a `34 × 26` 3D terrain mesh (`884` vertices, `~1,700` wireframe segments) at **60 FPS**:
1. **Once-Per-Frame Simulation (`LV_EVENT_DRAW_MAIN_BEGIN`)**:
   - Shifts the 26-row terrain waterfall history back by one row (`r = 0 -> 1 -> ... -> 25`).
   - Computes **Row 0** directly from live signed stereo PCM samples (`g_app.wave_l`, `g_app.wave_r`) scaled by an Automatic Gain Control (AGC) envelope so the front-most line acts as a real-time oscilloscope that propagates backward into the 3D mountain ridges.
   - Projects all `34 × 26` 3D coordinates through a smoothly orbiting 3D camera (`yaw` + `pitch` + perspective divide) into internal SRAM screen coordinates (`s_wc_vx`, `s_wc_vy`) and records per-row vertical bounding boxes (`s_wc_row_ymin[r]`, `s_wc_row_ymax[r]`).
2. **Per-Strip Direct SRAM Drawing (`LV_EVENT_DRAW_MAIN`)**:
   - As LVGL sweeps its 16-line internal-SRAM DMA strip down the dirty region (`Y = 92..319`), the renderer checks each row's `[ymin, ymax]` against the strip's `[by1, by2]`.
   - Rows outside the 16-line strip are rejected in a single integer comparison; intersecting segments are rasterized with a 2px-thick Bresenham line drawer (`wc_strip_line`) directly into internal SRAM at 240 MHz before DMA pushes the strip over 80 MHz QSPI.

---

## 4. Live Album Art Pipeline

`src/audio/artwork_fetcher.cpp` runs as a low-priority background worker (`art_fetch`, Core 1, Priority 1) with a task-scoped mbedTLS PSRAM allocator so HTTPS handshakes never fragment Internal DMA SRAM:
1. **Track Change Detection**: Watches `g_app.track_title` (`Artist - Track`) and current SomaFM preset ID without holding `g_app.mutex` across network I/O.
2. **Two-Tier Artwork Resolution**:
   - **Tier 1 (Live Track Art)**: Queries the Apple iTunes Search API (`https://itunes.apple.com/search?term=...&media=music&limit=1`), extracts `artworkUrl100`, and rewrites it to `160x160bb.jpg` (**baseline JPEG**—critical because Apple's `300x300bb.jpg` CDN serves progressive JPEGs, which the ESP32 ROM `tjpgd` decoder rejects).
   - **Tier 2 (SomaFM Channel Art)**: Falls back to official SomaFM station art (`https://somafm.com/img/<id>120.jpg`), and finally to a procedural vector vinyl/geometric cover generator.
3. **Bilinear 360×360 Upscale + Bottom Scrim**:
   - Decodes the JPEG into a temporary RGB565 buffer using `esp_rom_tjpgd`.
   - Performs a smooth 2D bilinear upscale to fill the entire `360×360` round screen (`ARTWORK_SIZE = 360`).
   - Pre-multiplies a smooth dark gradient scrim across the bottom `Y = 175..359` directly into the RGB565 pixel buffer so the white `Space Grotesk` track typography stays legible with zero alpha-blending overhead during UI rendering.

---

## 5. Battery & USB Charge Telemetry Pipeline

`src/bsp/battery_monitor.cpp` (`include/battery_monitor.h`) tracks LiPo state-of-charge and charging state without requiring a dedicated charger-status GPIO:
- **1 Hz Calibrated Burst Sampling**: Samples `GPIO 1` (`ADC1_CHANNEL_0`) through the `2:1` `R62`/`R63` divider using ESP-IDF curve-fitting calibration (`12` samples at boot, `6` trimmed-mean samples once per second at runtime to avoid `SARADC` / Wi-Fi radio contention).
- **Hybrid USB Host + Wall-Brick Charge Detection**: Combines `USB_SERIAL_JTAG` 1 kHz Start-of-Frame (`sof_frame_index`) activity with a sliding 4-second step detector (`+38 mV` plug / `-38 mV` unplug) and 60-second linear-regression voltage slope (`mV/min`) so both Mac/PC USB ports and dumb 5V USB-C wall chargers are detected accurately.
- **CC/CV Compensation & Distraction-Free UI**: Compensates terminal voltage lift while charging, applies `1.5%` SoC hysteresis, triggers DRV2605 haptics on plug/unplug and `15%`/`5%` low-battery thresholds, and hides the Home screen (`Tab 0`) battery pill unless actively charging or `< 20%` (hiding automatically once full).

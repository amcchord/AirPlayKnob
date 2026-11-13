# Current state — 2026-11-13 (`v1.1.0`)

**AirPlayKnob `v1.1.0` shipped and verified on hardware.** Both the original and
newly attached battery-equipped Waveshare ESP32-S3-Knob-Touch-LCD-1.8 units are
supported by unified `v1.1.0` firmware on `main`:

- **Sustained HomePod AirPlay 2 & HTTP Streaming**:
  - Raw lwIP `IcyRawTcp` HTTP client (`16 × MSS = 22,976 B` receive window, `TF_SACK` cleared, immediate ACK, DNS cache, and multi-AP BSSID stall failover with NVS `pref_bssid`) keeps the 256 KiB MP3 and 512 KiB PCM buffers full (`~2,955 ms` PCM / `~252 KB` MP3) across both local 3.5mm DAC and AirPlay playback.
  - Reclaimed `~92 KB` free Internal DMA SRAM (`~81.9 KB` largest contiguous block) and configured ESP-IDF Wi-Fi (`__wrap_esp_wifi_init`, `custom_cfg_err=0`) with `64` PSRAM `cache_tx_buf` staging slots + `48` dynamic MAC TX buffers, `802.11b/g/n` HT20 A-MPDU aggregation, and `IP_TOS = 0xA0` (`DSCP CS5` / `AC_VI`).
  - Fixed `RaopClient::sendSyncPacket()` (`PT=0xD4`) to derive `sync_rtp_ts` monotonically from wall-clock `sync_ntp` (`ntp_to_ts(sync_ntp, 44100)` with `m_latency_frames = 88,200` on AirPlay 2) instead of `m_head_ts`, paired with lossless `flushUnsentPacket()` (`750 ms` hold), `4.0 ms` NACK repair pacing, and `20 ms` lock-free `raop_tx` state caching.
  - Verified on **Workshop (HomePod)** (`AudioAccessory5,1`): `27,605 / 27,605` RTP originals accepted (`0` local/driver rejects, `0` retries, `0` audio gap events `> 50 ms`, `16 ms` max gap, `0` underruns), followed by multi-hour uninterrupted listening confirmed by the user.
- **Battery & Charge State Telemetry**:
  - Calibrated `GPIO 1` (`ADC1_CHANNEL_0`) LiPo SoC + `USB_SERIAL_JTAG` SOF and voltage-step/slope charge estimator (`src/bsp/battery_monitor.cpp`).
  - Home player screen (`Tab 0`) keeps the artwork clean by displaying the top-center battery indicator only when actively charging or below `20%` (and hiding once full), while Settings (`Tab 3`), Web Portal, `/api/status`, and `BATTERY` CLI expose continuous telemetry.
- **Full-LAN Alphabetical AirPlay Speaker Discovery**:
  - Two-pass PSRAM mDNS scan (`_airplay._tcp` + `_raop._tcp`, up to 48 speakers) sorted case-insensitively (`A -> Z`, `Local 3.5mm Jack` last).
- **Release & CI**:
  - All five host test suites (`test_alac_packets.py`, `test_rtsp_responses.py`, `test_network_policy.py`, `test_pcm_buffer.py`, `test_battery_monitor.py`) and clean credential-free PlatformIO builds run in GitHub Actions.
  - Clean credential-free `AirPlayKnob-v1.1.0-factory.bin` and `AirPlayKnob-v1.1.0-ota.bin` assets published on GitHub Releases (`v1.1.0`).

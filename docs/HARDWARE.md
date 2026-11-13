# Hardware Reference: Waveshare ESP32-S3-Knob-Touch-LCD-1.8

AirPlayKnob is built for the **[Waveshare ESP32-S3-Knob-Touch-LCD-1.8](https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8)** development board, which integrates a dual-core ESP32-S3 (8 MB Octal PSRAM + 16 MB Flash), a 1.8" 360×360 round QSPI display, capacitive touch, a mechanical rotary encoder outer ring, a DRV2605 haptic motor driver, and a PCM5100A 3.5mm stereo audio DAC.

---

## 1. Complete GPIO Pinout

| Subsystem | Controller / IC | Signal | ESP32-S3 GPIO | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **QSPI Display (360×360)** | SH8601 / ST77916 | `LCD_CS` | `GPIO 14` | SPI2_HOST (80 MHz QSPI) |
| | | `LCD_PCLK` | `GPIO 13` | QSPI Clock |
| | | `LCD_DATA0` | `GPIO 15` | QSPI Data 0 |
| | | `LCD_DATA1` | `GPIO 16` | QSPI Data 1 |
| | | `LCD_DATA2` | `GPIO 17` | QSPI Data 2 |
| | | `LCD_DATA3` | `GPIO 18` | QSPI Data 3 |
| | | `LCD_RST` | `GPIO 21` | Active-low hardware reset |
| | | `LCD_BL` | `GPIO 47` | LEDC PWM backlight control |
| **Capacitive Touch** | CST816S (`0x15`) | `TP_SDA` | `GPIO 11` | Shared `I2C_NUM_0` (300 kHz) |
| | | `TP_SCL` | `GPIO 12` | Shared `I2C_NUM_0` (300 kHz) |
| | | `TP_RST` | `GPIO 10` | Active-low touch reset |
| | | `TP_INT` | `GPIO 9` | Touch interrupt |
| **Haptic Motor Driver** | DRV2605 (`0x5A`) | `I2C_SDA` | `GPIO 11` | Shared `I2C_NUM_0` with CST816S |
| | | `I2C_SCL` | `GPIO 12` | ERM ROM Library 1 waveform effects |
| **Rotary Encoder Ring** | SSCM110100 | `KNOB_ECA` | `GPIO 8` | Quadrature Channel A (Internal pull-up) |
| | | `KNOB_ECB` | `GPIO 7` | Quadrature Channel B (Internal pull-up) |
| **Stereo Audio DAC** | PCM5100A + CH445P | `I2S_BCLK` | `GPIO 39` | I2S Bit Clock (44.1 kHz stereo) |
| | | `I2S_WS` | `GPIO 40` | I2S Word Select (LRCLK) |
| | | `I2S_DOUT` | `GPIO 41` | I2S Serial Data Out |
| | | `PA_MUX_SEL` | `GPIO 0` | Set `HIGH` to route PCM5100A to 3.5mm jack |
| **Battery / Power Rail** | `ADC1_CHANNEL_0` (`R62`/`R63`) | `BATT_ADC` | `GPIO 1` | `10K` / `10K` (`2:1`) divider + `USB_SERIAL_JTAG` SOF |

---

## 2. 180° Inverted Mounting Orientation (USB-C at Top)

So the USB-C power/programming cable exits cleanly from the **top (12 o'clock)** of the knob when mounted on a desk stand or wall plate, the entire hardware BSP is configured for a **180° inverted orientation**:

1. **Display (`src/bsp/lcd_bsp.c`)**:
   - The `SH8601` / `ST77916` panel initialization sequence sets `MADCTL` (`0x36`) to `0xC0` (`MX | MY`, flipping both X and Y scan directions in hardware).
   - Because `SH8601` requires even-aligned column/row windows, LVGL uses partial rendering (`full_refresh = 0`) with an arearounder callback (`rounder_cb`) that aligns dirty rectangles to 2-pixel boundaries.
2. **Touch (`src/bsp/cst816.cpp`)**:
   - Raw touch coordinates `(raw_x, raw_y)` read from the `CST816S` over I2C are inverted to `(359 - raw_x, 359 - raw_y)` before both LVGL pointer reporting and 4-way swipe gesture classification.
3. **Rotary Encoder (`src/main.cpp`)**:
   - Quadrature callbacks (`KNOB_LEFT` / `KNOB_RIGHT`) map clockwise physical rotation to positive delta (`+1`) and counter-clockwise rotation to negative delta (`-1`).

---

## 3. DRV2605 Tactile Haptic Effects

The onboard `DRV2605` haptic controller (`0x5A` on `I2C_NUM_0`) is initialized at boot (`src/bsp/cst816.cpp`) using ERM ROM Library 1 (`src/bsp/lcd_config.h`) and triggered via `haptic_play_effect()` to give physical feedback for every interaction:
- **`HAPTIC_STRONG_CLICK` (Effect `1`) / `HAPTIC_SHARP_CLICK` (Effect `4`)**: Crisp mechanical click when stepping volume, toggling Play/Pause, or plugging into USB power.
- **`HAPTIC_LIGHT_TICK` (Effect `6`) / `HAPTIC_SOFT_BUMP` (Effect `7`)**: Subtle detent tick when scrolling through the 30 SomaFM stations or AirPlay speakers, or unplugging from USB power.
- **`HAPTIC_DOUBLE_CLICK` (Effect `10`) / `HAPTIC_TRIPLE_CLICK` (Effect `12`)**: Confirmation pulse when locking in a station/speaker or entering WhiteCap (`10`), and low/critical battery alerts (`10` at `15%`, `12` at `5%`).

---

## 4. Onboard 3.5mm Stereo DAC Output (`PCM5100A` + `CH445P`)

In addition to streaming over Wi-Fi to AirPlay speakers, the board includes a **TI PCM5100A** 32-bit I2S stereo DAC connected to the 3.5mm TRS audio jack through a **CH445P** analog multiplexer (`src/bsp/local_dac.c`):
- Driving `GPIO 0` `HIGH` switches the `CH445P` analog mux to connect the `PCM5100A` outputs to the 3.5mm jack.
- Can be used simultaneously alongside AirPlay streaming (via the *"Also output audio to onboard 3.5mm stereo jack"* toggle in the Web UI) or selected as the primary output (`Local 3.5mm Jack` in the Speakers wheel).

---

## 5. Battery & Charge Telemetry (`GPIO 1` / `ADC1_CHANNEL_0`)

Units equipped with the rear 3.7V LiPo battery connect the main battery/power rail (`CN1` Pin 1) to **`GPIO 1` (`ADC1_CHANNEL_0`)** through precision `R62 = 10K` / `R63 = 10K` (`2:1`) voltage-divider resistors (`src/bsp/battery_monitor.cpp`, `include/battery_monitor.h`):
- **Calibrated ADC Sampling**: Uses ESP-IDF `adc_oneshot` (`ADC_ATTEN_DB_12`, 12-bit) with eFuse curve-fitting calibration (`adc_cali_create_scheme_curve_fitting`) and trimmed-mean bursts (`12` samples at boot, `6` samples every `1000 ms` at runtime to minimize `SARADC` airtime during Wi-Fi streaming).
- **Charge & Unplug Detection**: Because the 10-pin `CN1` sub-board connector has no dedicated charger-status pin, firmware combines non-blocking `USB_SERIAL_JTAG` 1 kHz Start-of-Frame (`sof_frame_index`) host tracking with sliding 4-second step and 60-second slope detection (for dumb USB wall bricks), CC/CV charge-lift compensation, and constant-voltage (`>= 4.175V` flat or VBUS load-release step) full-charge termination. On the Home player screen (`Tab 0`), the battery indicator is hidden unless actively charging or strictly below `20%` (and hides once fully charged), while Settings (`Tab 3`) and the Web Portal show full telemetry continuously.

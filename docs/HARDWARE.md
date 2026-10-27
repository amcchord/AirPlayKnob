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
| | | `I2C_SCL` | `GPIO 12` | ERM Library B waveform effects |
| **Rotary Encoder Ring** | SSCM110100 | `KNOB_ECA` | `GPIO 8` | Quadrature Channel A (Internal pull-up) |
| | | `KNOB_ECB` | `GPIO 7` | Quadrature Channel B (Internal pull-up) |
| **Stereo Audio DAC** | PCM5100A + CH445P | `I2S_BCLK` | `GPIO 39` | I2S Bit Clock (44.1 kHz stereo) |
| | | `I2S_WS` | `GPIO 40` | I2S Word Select (LRCLK) |
| | | `I2S_DOUT` | `GPIO 41` | I2S Serial Data Out |
| | | `PA_MUX_SEL` | `GPIO 0` | Set `HIGH` to route PCM5100A to 3.5mm jack |

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

The onboard `DRV2605` haptic controller (`0x5A` on `I2C_NUM_0`) is initialized at boot (`src/bsp/cst816.cpp`) and triggered via `haptic_play_effect()` to give physical feedback for every interaction:
- **`HAPTIC_SHARP_CLICK` (Effect `1`)**: Crisp mechanical click when stepping volume or toggling Play/Pause.
- **`HAPTIC_LIGHT_TICK` (Effect `17`)**: Subtle detent tick when scrolling through the 30 SomaFM stations or AirPlay speakers.
- **`HAPTIC_DOUBLE_CLICK` (Effect `10`)**: Confirmation pulse when locking in a new station, switching AirPlay speakers, or entering the WhiteCap 3D visualizer.

---

## 4. Onboard 3.5mm Stereo DAC Output (`PCM5100A` + `CH445P`)

In addition to streaming over Wi-Fi to AirPlay speakers, the board includes a **TI PCM5100A** 32-bit I2S stereo DAC connected to the 3.5mm TRS audio jack through a **CH445P** analog multiplexer (`src/bsp/local_dac.c`):
- Driving `GPIO 0` `HIGH` switches the `CH445P` analog mux to connect the `PCM5100A` outputs to the 3.5mm jack.
- Can be used simultaneously alongside AirPlay streaming (via the *"Also output audio to onboard 3.5mm stereo jack"* toggle in the Web UI) or selected as the primary output (`Local 3.5mm Jack` in the Speakers wheel).

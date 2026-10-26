#ifndef LCD_CONFIG_H
#define LCD_CONFIG_H

#define EXAMPLE_LCD_H_RES              360
#define EXAMPLE_LCD_V_RES              360

#define LCD_BIT_PER_PIXEL              16

#define EXAMPLE_PIN_NUM_LCD_CS         14
#define EXAMPLE_PIN_NUM_LCD_PCLK       13
#define EXAMPLE_PIN_NUM_LCD_DATA0      15
#define EXAMPLE_PIN_NUM_LCD_DATA1      16
#define EXAMPLE_PIN_NUM_LCD_DATA2      17
#define EXAMPLE_PIN_NUM_LCD_DATA3      18
#define EXAMPLE_PIN_NUM_LCD_RST        21
#define EXAMPLE_PIN_NUM_BK_LIGHT       47

#define EXAMPLE_LVGL_BUF_HEIGHT        30
#define EXAMPLE_LVGL_TICK_PERIOD_MS    2
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 6
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 1
#define EXAMPLE_LVGL_TASK_STACK_SIZE   (8 * 1024)
#define EXAMPLE_LVGL_TASK_PRIORITY     5

#define EXAMPLE_TOUCH_ADDR             0x15
#define EXAMPLE_DRV2605_ADDR           0x5A
#define EXAMPLE_PIN_NUM_TOUCH_SCL      12
#define EXAMPLE_PIN_NUM_TOUCH_SDA      11
#define EXAMPLE_PIN_NUM_TOUCH_RST      10
#define EXAMPLE_PIN_NUM_TOUCH_INT      9

// Rotary Encoder (Bidirectional Pulse Switch SW2)
#define EXAMPLE_ENCODER_ECA_PIN        8
#define EXAMPLE_ENCODER_ECB_PIN        7

// Onboard PCM5100A I2S Stereo DAC + CH445P Switch
#define EXAMPLE_I2S_SWITCH_PIN         0
#define EXAMPLE_I2S_BCLK_PIN           39
#define EXAMPLE_I2S_WS_PIN             40
#define EXAMPLE_I2S_DOUT_PIN           41

// Haptic waveform presets (DRV2605 ROM Library 1)
#define HAPTIC_STRONG_CLICK            1
#define HAPTIC_SHARP_CLICK             4
#define HAPTIC_LIGHT_TICK              6
#define HAPTIC_SOFT_BUMP               7
#define HAPTIC_DOUBLE_CLICK            10
#define HAPTIC_TRIPLE_CLICK            12

#endif

#include "local_dac.h"
#include "lcd_config.h"
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"

static i2s_chan_handle_t s_tx_chan = NULL;
static bool s_dac_ready = false;

void local_dac_init(void) {
    if (s_dac_ready) return;

    gpio_config_t gpio_conf = {
        .pin_bit_mask = (1ULL << EXAMPLE_I2S_SWITCH_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gpio_conf);
    gpio_set_level((gpio_num_t)EXAMPLE_I2S_SWITCH_PIN, 1);

    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    tx_chan_cfg.dma_desc_num = 8;
    tx_chan_cfg.dma_frame_num = 352;
    if (i2s_new_channel(&tx_chan_cfg, &s_tx_chan, NULL) != ESP_OK) {
        ESP_LOGW("DAC", "Failed to create I2S channel");
        return;
    }

    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)EXAMPLE_I2S_BCLK_PIN,
            .ws = (gpio_num_t)EXAMPLE_I2S_WS_PIN,
            .dout = (gpio_num_t)EXAMPLE_I2S_DOUT_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    if (i2s_channel_init_std_mode(s_tx_chan, &tx_std_cfg) == ESP_OK &&
        i2s_channel_enable(s_tx_chan) == ESP_OK) {
        s_dac_ready = true;
        ESP_LOGI("DAC", "PCM5100A I2S DAC initialized at 44100Hz stereo");
    }
}

void local_dac_write_frames(const int16_t *stereo_pcm, size_t frames, int volume_pct) {
    if (!s_dac_ready || !s_tx_chan || !stereo_pcm || frames == 0) return;

    int16_t scaled[704]; // up to 352 stereo frames
    if (frames > 352) frames = 352;
    int32_t gain = (volume_pct * 256) / 100;
    size_t samples = frames * 2;
    for (size_t i = 0; i < samples; i++) {
        int32_t v = ((int32_t)stereo_pcm[i] * gain) >> 8;
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        scaled[i] = (int16_t)v;
    }
    size_t written = 0;
    i2s_channel_write(s_tx_chan, scaled, samples * sizeof(int16_t), &written, pdMS_TO_TICKS(15));
}

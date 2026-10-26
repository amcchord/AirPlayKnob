#include "cst816.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lcd_config.h"

#define TEST_I2C_PORT I2C_NUM_0
static SemaphoreHandle_t s_i2c_mux = NULL;
static bool s_drv2605_ready = false;

static uint8_t i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(TEST_I2C_PORT, addr, buf, 2, pdMS_TO_TICKS(50));
}

static uint8_t i2c_read_regs(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len) {
    return i2c_master_write_read_device(TEST_I2C_PORT, addr, &reg, 1, buf, len, pdMS_TO_TICKS(50));
}

static void drv2605_init_internal(void) {
    uint8_t status = 0;
    if (i2c_read_regs(EXAMPLE_DRV2605_ADDR, 0x00, &status, 1) != ESP_OK) {
        ESP_LOGW("DRV2605", "DRV2605 not detected at 0x5A");
        return;
    }
    i2c_write_reg(EXAMPLE_DRV2605_ADDR, 0x01, 0x00); // Mode: out of standby, internal trigger
    i2c_write_reg(EXAMPLE_DRV2605_ADDR, 0x02, 0x00); // No RTP
    i2c_write_reg(EXAMPLE_DRV2605_ADDR, 0x03, 0x01); // Library 1
    i2c_write_reg(EXAMPLE_DRV2605_ADDR, 0x04, 0x01); // Strong click
    i2c_write_reg(EXAMPLE_DRV2605_ADDR, 0x05, 0x00); // End sequence
    i2c_write_reg(EXAMPLE_DRV2605_ADDR, 0x0D, 0x00); // Overdrive 0
    i2c_write_reg(EXAMPLE_DRV2605_ADDR, 0x0E, 0x00);
    i2c_write_reg(EXAMPLE_DRV2605_ADDR, 0x0F, 0x00);
    i2c_write_reg(EXAMPLE_DRV2605_ADDR, 0x10, 0x00);
    i2c_write_reg(EXAMPLE_DRV2605_ADDR, 0x13, 0x64);

    uint8_t fb = 0x36;
    i2c_read_regs(EXAMPLE_DRV2605_ADDR, 0x1A, &fb, 1);
    i2c_write_reg(EXAMPLE_DRV2605_ADDR, 0x1A, fb & 0x7F); // ERM mode

    uint8_t c3 = 0xA0;
    i2c_read_regs(EXAMPLE_DRV2605_ADDR, 0x1D, &c3, 1);
    i2c_write_reg(EXAMPLE_DRV2605_ADDR, 0x1D, c3 | 0x20); // ERM open loop

    s_drv2605_ready = true;
    ESP_LOGI("DRV2605", "DRV2605 haptic driver initialized (status=0x%02x)", status);
}

void Touch_Init(void) {
    if (!s_i2c_mux) {
        s_i2c_mux = xSemaphoreCreateMutex();
    }
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = EXAMPLE_PIN_NUM_TOUCH_SDA,
        .scl_io_num = EXAMPLE_PIN_NUM_TOUCH_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = {.clk_speed = 300 * 1000},
        .clk_flags = 0,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_param_config(TEST_I2C_PORT, &conf));
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_driver_install(TEST_I2C_PORT, conf.mode, 0, 0, 0));

    xSemaphoreTake(s_i2c_mux, portMAX_DELAY);
    i2c_write_reg(EXAMPLE_TOUCH_ADDR, 0x00, 0x00); // Switch CST816 to normal mode
    drv2605_init_internal();
    xSemaphoreGive(s_i2c_mux);

    haptic_play_effect(HAPTIC_DOUBLE_CLICK);
}

uint8_t getTouch(uint16_t *x, uint16_t *y) {
    if (!s_i2c_mux) return 0;
    if (xSemaphoreTake(s_i2c_mux, pdMS_TO_TICKS(10)) != pdTRUE) return 0;

    uint8_t data[7] = {0};
    uint8_t ret = i2c_read_regs(EXAMPLE_TOUCH_ADDR, 0x00, data, 7);
    xSemaphoreGive(s_i2c_mux);

    if (ret == ESP_OK && data[2] > 0) {
        *x = ((uint16_t)(data[3] & 0x0f) << 8) + (uint16_t)data[4];
        *y = ((uint16_t)(data[5] & 0x0f) << 8) + (uint16_t)data[6];
        return 1;
    }
    return 0;
}

void haptic_play_effect(uint8_t effect_id) {
    if (!s_i2c_mux || !s_drv2605_ready || effect_id == 0) return;
    if (xSemaphoreTake(s_i2c_mux, pdMS_TO_TICKS(15)) != pdTRUE) return;
    i2c_write_reg(EXAMPLE_DRV2605_ADDR, 0x04, effect_id);
    i2c_write_reg(EXAMPLE_DRV2605_ADDR, 0x05, 0x00);
    i2c_write_reg(EXAMPLE_DRV2605_ADDR, 0x0C, 0x01); // GO
    xSemaphoreGive(s_i2c_mux);
}

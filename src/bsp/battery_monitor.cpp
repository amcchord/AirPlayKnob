#include "battery_monitor.h"
#include "app_state.h"
#include "bsp/lcd_config.h"
#include <Arduino.h>
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "soc/usb_serial_jtag_struct.h"

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_cali_handle = NULL;
static bool s_cali_ready = false;
static bool s_adc_ready = false;

static BatteryEstimator s_estimator;
static BatteryTelemetry s_latest = {};
static portMUX_TYPE s_batt_mux = portMUX_INITIALIZER_UNLOCKED;

// Non-blocking USB Full-Speed SOF (1 kHz frame counter) detector
static uint16_t s_prev_sof_frame = 0;
static uint32_t s_last_sof_check_ms = 0;
static uint32_t s_last_sof_advance_ms = 0;
static bool s_usb_host_active = false;
static uint32_t s_last_adc_poll_ms = 0;

static bool sample_usb_host_now(uint32_t now_ms, bool allow_brief_spin) {
    uint16_t f0 = (uint16_t)USB_SERIAL_JTAG.fram_num.sof_frame_index;
    if (allow_brief_spin) {
        // At boot only: wait up to 2.5 ms for a 1 ms USB SOF tick so the very first UI frame knows USB state
        uint32_t t0 = micros();
        while ((uint32_t)(micros() - t0) < 2500U) {
            uint16_t f1 = (uint16_t)USB_SERIAL_JTAG.fram_num.sof_frame_index;
            if (f1 != f0) {
                s_prev_sof_frame = f1;
                s_last_sof_check_ms = now_ms;
                s_last_sof_advance_ms = now_ms;
                s_usb_host_active = true;
                return true;
            }
        }
        s_prev_sof_frame = f0;
        s_last_sof_check_ms = now_ms;
        s_usb_host_active = false;
        return false;
    }

    // Non-blocking 60 ms check (60 mod 2048 != 0, so a 1 kHz USB SOF never aliases!)
    if ((uint32_t)(now_ms - s_last_sof_check_ms) >= 60U) {
        s_last_sof_check_ms = now_ms;
        if (f0 != s_prev_sof_frame) {
            s_prev_sof_frame = f0;
            s_last_sof_advance_ms = now_ms;
            s_usb_host_active = true;
        } else if ((uint32_t)(now_ms - s_last_sof_advance_ms) >= 750U) {
            s_usb_host_active = false;
        }
    }
    return s_usb_host_active;
}

static bool read_battery_burst_mv(int *out_rail_mv, int *out_raw_adc, size_t burst_n) {
    if (!s_adc_ready || !s_adc_handle) return false;

    constexpr size_t MAX_BURST_N = 12;
    if (burst_n < 4) burst_n = 4;
    if (burst_n > MAX_BURST_N) burst_n = MAX_BURST_N;
    int mv_samples[MAX_BURST_N] = {};
    int raw_samples[MAX_BURST_N] = {};
    size_t ok_cnt = 0;

    for (size_t i = 0; i < burst_n; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc_handle, ADC_CHANNEL_0, &raw) == ESP_OK) {
            int pin_mv = 0;
            if (s_cali_ready && adc_cali_raw_to_voltage(s_cali_handle, raw, &pin_mv) == ESP_OK) {
                mv_samples[ok_cnt] = pin_mv * 2; // R62=10K / R63=10K 2:1 divider
            } else {
                mv_samples[ok_cnt] = ((raw * 3300) / 4095) * 2;
            }
            raw_samples[ok_cnt] = raw;
            ok_cnt++;
        }
    }

    if (ok_cnt == 0) return false;
    *out_rail_mv = BatteryEstimator::trimmed_mean(mv_samples, ok_cnt);
    *out_raw_adc = BatteryEstimator::trimmed_mean(raw_samples, ok_cnt);
    return true;
}

static void publish_telemetry(const BatteryTelemetry &tel) {
    portENTER_CRITICAL(&s_batt_mux);
    s_latest = tel;
    portEXIT_CRITICAL(&s_batt_mux);

    if (g_app.mutex && xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        g_app.battery = tel;
        xSemaphoreGive(g_app.mutex);
    }
}

void battery_monitor_init(void) {
    adc_oneshot_unit_init_cfg_t unit_cfg = {};
    unit_cfg.unit_id = ADC_UNIT_1;
    unit_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc_handle) != ESP_OK) {
        Serial.println("[BATT] Failed to initialize ADC1 oneshot unit");
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.atten = ADC_ATTEN_DB_12;
    chan_cfg.bitwidth = ADC_BITWIDTH_12;
    if (adc_oneshot_config_channel(s_adc_handle, ADC_CHANNEL_0, &chan_cfg) != ESP_OK) {
        Serial.println("[BATT] Failed to configure ADC1_CHANNEL_0 (GPIO 1)");
        return;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id = ADC_UNIT_1;
    cali_cfg.chan = ADC_CHANNEL_0;
    cali_cfg.atten = ADC_ATTEN_DB_12;
    cali_cfg.bitwidth = ADC_BITWIDTH_12;
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle) == ESP_OK) {
        s_cali_ready = true;
    }
#endif

    s_adc_ready = true;
    uint32_t now_ms = millis();
    bool usb_host = sample_usb_host_now(now_ms, true);
    int rail_mv = 0, raw_adc = 0;
    if (read_battery_burst_mv(&rail_mv, &raw_adc, 12)) {
        BatteryTelemetry tel = s_estimator.update(now_ms, rail_mv, raw_adc, usb_host);
        publish_telemetry(tel);
        Serial.printf("[BATT] Init GPIO%d (ADC1_CH0 cali=%d): %d mV (raw=%d), pct=%d%%, state=%s, usb_host=%d\n",
                      EXAMPLE_PIN_NUM_BATT_ADC, (int)s_cali_ready,
                      tel.voltage_mv, tel.raw_adc, tel.percent,
                      battery_state_slug(tel.state), (int)tel.usb_host);
    }
    s_last_adc_poll_ms = now_ms;
}

void battery_monitor_poll(void) {
    if (!s_adc_ready) return;
    uint32_t now_ms = millis();
    bool usb_host = sample_usb_host_now(now_ms, false);

    if ((uint32_t)(now_ms - s_last_adc_poll_ms) < 1000U) {
        return;
    }
    s_last_adc_poll_ms = now_ms;

    int rail_mv = 0, raw_adc = 0;
    if (!read_battery_burst_mv(&rail_mv, &raw_adc, 6)) return;

    BatteryTelemetry tel = s_estimator.update(now_ms, rail_mv, raw_adc, usb_host);
    publish_telemetry(tel);
}

BatteryTelemetry battery_monitor_snapshot(void) {
    BatteryTelemetry snap = {};
    portENTER_CRITICAL(&s_batt_mux);
    snap = s_latest;
    portEXIT_CRITICAL(&s_batt_mux);
    return snap;
}

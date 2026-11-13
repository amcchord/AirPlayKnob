#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum BatteryChargeState {
    BATT_STATE_UNKNOWN     = 0,
    BATT_STATE_DISCHARGING = 1,
    BATT_STATE_CHARGING    = 2,
    BATT_STATE_CHARGED     = 3,
    BATT_STATE_USB_POWER   = 4,
};

struct BatteryTelemetry {
    bool valid;
    bool battery_present;
    bool usb_present;
    bool usb_host;
    bool charging;
    bool charged;
    bool low_battery;
    bool critical_battery;
    int percent;           // 0..100 when battery voltage is known, -1 when isolated behind 5V USB rail
    int voltage_mv;        // Smoothed battery / rail voltage in mV
    int raw_mv;            // Latest unsmoothed trimmed-mean burst voltage in mV
    int raw_adc;           // Latest 12-bit ADC count (0..4095)
    int slope_mv_per_min;  // 60-second voltage trend in mV/min
    BatteryChargeState state;
};

static inline const char *battery_state_slug(BatteryChargeState st) {
    switch (st) {
        case BATT_STATE_DISCHARGING: return "discharging";
        case BATT_STATE_CHARGING:    return "charging";
        case BATT_STATE_CHARGED:     return "charged";
        case BATT_STATE_USB_POWER:   return "usb_power";
        default:                     return "unknown";
    }
}

static inline const char *battery_state_label(BatteryChargeState st) {
    switch (st) {
        case BATT_STATE_DISCHARGING: return "On Battery";
        case BATT_STATE_CHARGING:    return "Charging";
        case BATT_STATE_CHARGED:     return "Charged";
        case BATT_STATE_USB_POWER:   return "USB / Charging";
        default:                     return "Checking...";
    }
}

// Pure C++ estimator for unit testing and ESP32-S3 runtime battery/charge state tracking.
class BatteryEstimator {
public:
    static constexpr int MIN_VALID_MV          = 2000;
    static constexpr int EXT_5V_ENTER_MV       = 4380;
    static constexpr int EXT_5V_EXIT_MV        = 4300;
    static constexpr int CHARGED_ENTER_MV      = 4175;
    static constexpr int CHARGED_FAST_MV       = 4195;
    static constexpr int CHARGED_EXIT_MV       = 4140;
    static constexpr int WALL_PLUG_STEP_MV     = 35;
    static constexpr int WALL_UNPLUG_STEP_MV   = -32;
    static constexpr int WALL_RISE_60S_MV      = 8;
    static constexpr int WALL_FALL_60S_MV      = -10;
    static constexpr int LOW_BATTERY_PCT       = 20;
    static constexpr int CRITICAL_BATTERY_PCT  = 10;

    static int trimmed_mean(const int *samples, size_t count) {
        if (!samples || count == 0) return 0;
        if (count < 4) {
            long sum = 0;
            for (size_t i = 0; i < count; i++) sum += samples[i];
            return (int)((sum + (long)(count / 2)) / (long)count);
        }
        int min_v = samples[0], max_v = samples[0];
        long sum = 0;
        for (size_t i = 0; i < count; i++) {
            int v = samples[i];
            if (v < min_v) min_v = v;
            if (v > max_v) max_v = v;
            sum += v;
        }
        sum -= (min_v + max_v);
        size_t rem = count - 2;
        return (int)((sum + (long)(rem / 2)) / (long)rem);
    }

    // Single-cell 3.7V/4.2V LiPo SoC curve with CC/CV charge-lift compensation.
    static int voltage_to_percent(int mv, bool is_charging) {
        int eff_mv = mv;
        if (is_charging && eff_mv <= 4350) {
            int lift_mv = 0;
            if (eff_mv <= 3950) {
                lift_mv = 75;
            } else if (eff_mv < 4200) {
                lift_mv = (75 * (4200 - eff_mv)) / (4200 - 3950);
            }
            eff_mv -= lift_mv;
        }

        struct Knot { int mv; int pct; };
        static constexpr Knot KNOTS[] = {
            {3300,   0},
            {3450,   5},
            {3580,  15},
            {3660,  30},
            {3720,  45},
            {3780,  60},
            {3860,  75},
            {3950,  88},
            {4060,  96},
            {4150, 100},
        };
        constexpr size_t N = sizeof(KNOTS) / sizeof(KNOTS[0]);
        if (eff_mv <= KNOTS[0].mv) return 0;
        if (eff_mv >= KNOTS[N - 1].mv) return 100;
        for (size_t i = 0; i + 1 < N; i++) {
            if (eff_mv >= KNOTS[i].mv && eff_mv <= KNOTS[i + 1].mv) {
                int span_mv = KNOTS[i + 1].mv - KNOTS[i].mv;
                int span_pct = KNOTS[i + 1].pct - KNOTS[i].pct;
                int num = (eff_mv - KNOTS[i].mv) * span_pct + (span_mv / 2);
                return KNOTS[i].pct + (num / span_mv);
            }
        }
        return 100;
    }

    BatteryTelemetry update(uint32_t now_ms, int sample_mv, int raw_adc, bool usb_host_active) {
        BatteryTelemetry out = {};
        out.raw_mv = sample_mv;
        out.raw_adc = raw_adc;
        out.usb_host = usb_host_active;

        if (sample_mv < MIN_VALID_MV) {
            out.valid = false;
            out.battery_present = false;
            out.usb_present = usb_host_active;
            out.percent = -1;
            out.voltage_mv = sample_mv;
            out.state = usb_host_active ? BATT_STATE_USB_POWER : BATT_STATE_UNKNOWN;
            return out;
        }

        // Fast-step-aware EMA smoothing so plug/unplug steps settle in ~1s while steady noise is filtered.
        if (!m_initialized) {
            m_initialized = true;
            m_prev_usb_host = usb_host_active;
            m_smooth_mv_x16 = sample_mv * 16;
            reset_trend_windows(sample_mv, now_ms);
        } else {
            int cur_mv = (m_smooth_mv_x16 + 8) / 16;
            int diff = sample_mv - cur_mv;
            if (diff >= 40 || diff <= -40) {
                m_step_streak++;
            } else {
                m_step_streak = 0;
            }
            // alpha = 10/16 on sustained step, 3/16 normally
            int alpha = (m_step_streak >= 2) ? 10 : 3;
            m_smooth_mv_x16 = m_smooth_mv_x16 + alpha * (sample_mv - cur_mv);
        }

        int smooth_mv = (m_smooth_mv_x16 + 8) / 16;
        out.valid = true;
        out.voltage_mv = smooth_mv;

        // If USB host SOF just disconnected, clear charge latches & re-baseline trend history immediately
        if (m_prev_usb_host && !usb_host_active) {
            m_wall_chg_latched = false;
            m_charged_latched = false;
            m_smooth_mv_x16 = sample_mv * 16;
            smooth_mv = sample_mv;
            out.voltage_mv = smooth_mv;
            reset_trend_windows(smooth_mv, now_ms);
        }
        m_prev_usb_host = usb_host_active;

        // Sliding 4-second step detector for dumb USB wall bricks without USB SOF frames
        size_t sec_oldest = (m_sec_count < SEC_LEN) ? 0 : ((m_sec_idx + 1) % SEC_LEN);
        uint32_t sec_span_ms = now_ms - m_sec_ms[sec_oldest];
        int short_delta_mv = smooth_mv - m_sec_mv[sec_oldest];
        if (!m_ext_5v_rail && !usb_host_active && sec_span_ms >= 1000) {
            if (short_delta_mv >= WALL_PLUG_STEP_MV) {
                m_wall_chg_latched = true;
            } else if (short_delta_mv <= WALL_UNPLUG_STEP_MV) {
                if (m_wall_chg_latched) {
                    m_wall_chg_latched = false;
                    m_charged_latched = false;
                    m_smooth_mv_x16 = sample_mv * 16;
                    smooth_mv = sample_mv;
                    out.voltage_mv = smooth_mv;
                    reset_trend_windows(smooth_mv, now_ms);
                }
            }
        }
        if ((uint32_t)(now_ms - m_last_sec_ms) >= 1000) {
            m_sec_idx = (m_sec_idx + 1) % SEC_LEN;
            m_sec_mv[m_sec_idx] = smooth_mv;
            m_sec_ms[m_sec_idx] = now_ms;
            if (m_sec_count < SEC_LEN) m_sec_count++;
            m_last_sec_ms = now_ms;
        }

        // 60-second history ring (16 slots * 4s = 60s span)
        if ((uint32_t)(now_ms - m_last_hist_ms) >= 4000) {
            m_hist_idx = (m_hist_idx + 1) % HIST_LEN;
            m_hist_mv[m_hist_idx] = smooth_mv;
            m_hist_ms[m_hist_idx] = now_ms;
            if (m_hist_count < HIST_LEN) m_hist_count++;
            m_last_hist_ms = now_ms;
        }

        size_t oldest_idx = (m_hist_count < HIST_LEN) ? 0 : ((m_hist_idx + 1) % HIST_LEN);
        uint32_t span_ms = now_ms - m_hist_ms[oldest_idx];
        int delta_window_mv = smooth_mv - m_hist_mv[oldest_idx];
        int slope_mv_min = 0;
        if (span_ms >= 8000) {
            slope_mv_min = (int)(((int64_t)delta_window_mv * 60000LL) / (int64_t)span_ms);
        }
        out.slope_mv_per_min = slope_mv_min;

        if (span_ms >= 40000 && !m_ext_5v_rail && !usb_host_active) {
            if (delta_window_mv >= WALL_RISE_60S_MV) {
                m_wall_chg_latched = true;
            } else if (delta_window_mv <= WALL_FALL_60S_MV) {
                m_wall_chg_latched = false;
            }
        }

        // External 5V Schottky OR-ing rail detection (> 4.38V enter, < 4.30V exit)
        if (!m_ext_5v_rail && smooth_mv >= EXT_5V_ENTER_MV) {
            m_ext_5v_rail = true;
            m_ext_5v_enter_ms = now_ms;
            m_ext_5v_start_pct = m_last_pct;
            m_ext_5v_base_mv = 0;
        } else if (m_ext_5v_rail && smooth_mv <= EXT_5V_EXIT_MV) {
            m_ext_5v_rail = false;
            m_ext_5v_base_mv = 0;
            // Reset smoothing history to cell range immediately on unplug from 5V rail
            m_smooth_mv_x16 = sample_mv * 16;
            smooth_mv = sample_mv;
            out.voltage_mv = smooth_mv;
            reset_trend_windows(smooth_mv, now_ms);
            m_wall_chg_latched = false;
            m_charged_latched = false;
        }

        if (m_ext_5v_rail) {
            out.slope_mv_per_min = 0;
            out.usb_present = true;
            out.battery_present = m_seen_cell_range;
            if (m_seen_cell_range && m_ext_5v_start_pct >= 0) {
                if (m_ext_5v_start_pct >= 99) {
                    m_charged_latched = true;
                } else {
                    uint32_t chg_ms = now_ms - m_ext_5v_enter_ms;
                    if (chg_ms >= 8000U && (m_ext_5v_base_mv == 0 || (chg_ms < 60000U && smooth_mv < m_ext_5v_base_mv))) {
                        m_ext_5v_base_mv = smooth_mv;
                    }
                    // Detect sub-board charger standby (+55 mV VBUS rise once charge current drops to ~0 mA)
                    if (m_ext_5v_base_mv > 0 && chg_ms >= 180000U && (smooth_mv - m_ext_5v_base_mv) >= 55 && m_hist_count >= 6) {
                        size_t idx_20s = (m_hist_idx + HIST_LEN - 5) % HIST_LEN;
                        int d20 = smooth_mv - m_hist_mv[idx_20s];
                        if (d20 <= 8 && d20 >= -8) {
                            m_charged_latched = true;
                        }
                    }
                    // Conservative CC/CV full-charge time bound so overnight charging transitions cleanly to Charged
                    uint32_t full_ms = (uint32_t)(100 - m_ext_5v_start_pct) * 90000U + 1500000U;
                    if (chg_ms >= full_ms) {
                        m_charged_latched = true;
                    }
                    if (!m_charged_latched) {
                        int est_pct = m_ext_5v_start_pct +
                                      (int)(((uint64_t)chg_ms * (uint64_t)(100 - m_ext_5v_start_pct)) / (uint64_t)full_ms);
                        if (est_pct > 99) est_pct = 99;
                        m_last_pct = est_pct;
                    }
                }
                if (m_charged_latched) {
                    m_last_pct = 100;
                    out.percent = 100;
                    out.charged = true;
                    out.charging = false;
                    out.state = BATT_STATE_CHARGED;
                } else {
                    out.percent = m_last_pct;
                    out.charged = false;
                    out.charging = true;
                    out.state = BATT_STATE_CHARGING;
                }
            } else {
                out.percent = -1;
                out.charging = false;
                out.charged = false;
                out.state = BATT_STATE_USB_POWER;
            }
            return out;
        }

        // LiPo cell voltage range (2.0V .. 4.35V)
        m_seen_cell_range = true;
        out.battery_present = true;
        bool usb_power = usb_host_active || m_wall_chg_latched;
        out.usb_present = usb_power;

        if (!usb_power) {
            m_charged_latched = false;
            int raw_pct = voltage_to_percent(smooth_mv, false);
            int pct = apply_pct_hysteresis(raw_pct, smooth_mv);
            out.percent = pct;
            out.charging = false;
            out.charged = false;
            out.low_battery = (pct < LOW_BATTERY_PCT);
            out.critical_battery = (pct <= CRITICAL_BATTERY_PCT);
            out.state = BATT_STATE_DISCHARGING;
            return out;
        }

        // USB power present on LiPo cell rail: distinguish Charging vs Charged (CV complete)
        bool flat_slope = false;
        if (m_hist_count >= 6) {
            size_t idx_20s = (m_hist_idx + HIST_LEN - 5) % HIST_LEN;
            uint32_t span_20s_ms = now_ms - m_hist_ms[idx_20s];
            int delta_20s_mv = smooth_mv - m_hist_mv[idx_20s];
            flat_slope = (span_20s_ms >= 20000 && delta_20s_mv <= 4 && delta_20s_mv >= -8);
        }
        if (!m_charged_latched) {
            if (smooth_mv >= CHARGED_FAST_MV || (smooth_mv >= CHARGED_ENTER_MV && flat_slope)) {
                m_charged_latched = true;
            }
        } else if (smooth_mv <= CHARGED_EXIT_MV) {
            m_charged_latched = false;
        }

        if (m_charged_latched) {
            m_last_pct = 100;
            m_last_pct_mv = smooth_mv;
            out.percent = 100;
            out.charging = false;
            out.charged = true;
            out.state = BATT_STATE_CHARGED;
        } else {
            int raw_pct = voltage_to_percent(smooth_mv, true);
            if (raw_pct > 99) raw_pct = 99; // Reserve 100% for CV termination when actively charging
            int pct = apply_pct_hysteresis(raw_pct, smooth_mv);
            if (pct > 99) pct = 99;
            out.percent = pct;
            out.charging = true;
            out.charged = false;
            out.state = BATT_STATE_CHARGING;
        }
        return out;
    }

private:
    static constexpr size_t SEC_LEN = 5;
    static constexpr size_t HIST_LEN = 16;

    void reset_trend_windows(int mv, uint32_t now_ms) {
        for (size_t i = 0; i < SEC_LEN; i++) {
            m_sec_mv[i] = mv;
            m_sec_ms[i] = now_ms;
        }
        m_sec_idx = 0;
        m_sec_count = 1;
        m_last_sec_ms = now_ms;

        for (size_t i = 0; i < HIST_LEN; i++) {
            m_hist_mv[i] = mv;
            m_hist_ms[i] = now_ms;
        }
        m_hist_idx = 0;
        m_hist_count = 1;
        m_last_hist_ms = now_ms;
    }

    int apply_pct_hysteresis(int raw_pct, int smooth_mv) {
        if (m_last_pct < 0) {
            m_last_pct = raw_pct;
            m_last_pct_mv = smooth_mv;
            return raw_pct;
        }
        int diff_pct = raw_pct - m_last_pct;
        int diff_mv = smooth_mv - m_last_pct_mv;
        if (raw_pct == 0 || raw_pct == 100 ||
            diff_pct >= 2 || diff_pct <= -2 ||
            (diff_pct != 0 && (diff_mv >= 14 || diff_mv <= -14))) {
            m_last_pct = raw_pct;
            m_last_pct_mv = smooth_mv;
        }
        return m_last_pct;
    }

    bool m_initialized = false;
    bool m_prev_usb_host = false;
    bool m_ext_5v_rail = false;
    bool m_seen_cell_range = false;
    bool m_wall_chg_latched = false;
    bool m_charged_latched = false;
    int m_step_streak = 0;
    int m_smooth_mv_x16 = 0;
    int m_last_pct = -1;
    int m_last_pct_mv = 0;
    uint32_t m_ext_5v_enter_ms = 0;
    int m_ext_5v_start_pct = -1;
    int m_ext_5v_base_mv = 0;

    int m_sec_mv[SEC_LEN] = {};
    uint32_t m_sec_ms[SEC_LEN] = {};
    size_t m_sec_idx = 0;
    size_t m_sec_count = 1;
    uint32_t m_last_sec_ms = 0;

    int m_hist_mv[HIST_LEN] = {};
    uint32_t m_hist_ms[HIST_LEN] = {};
    size_t m_hist_idx = 0;
    size_t m_hist_count = 1;
    uint32_t m_last_hist_ms = 0;
};

void battery_monitor_init(void);
void battery_monitor_poll(void);
BatteryTelemetry battery_monitor_snapshot(void);

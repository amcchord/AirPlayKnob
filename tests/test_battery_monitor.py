"""Host unit tests for BatteryEstimator (LiPo SoC curve, hysteresis, USB/wall-brick charge & CV termination).

Compiles include/battery_monitor.h directly on the host with -Wall -Wextra.
Run: python3 tests/test_battery_monitor.py
"""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]

CPP_TEST = r'''
#include "battery_monitor.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    // 1. Trimmed mean outlier rejection
    {
        int burst[12] = {2950, 3800, 3802, 3798, 3801, 3799, 3800, 3801, 3799, 3800, 3800, 4490};
        int avg = BatteryEstimator::trimmed_mean(burst, 12);
        assert(avg == 3800);

        int short_burst[3] = {3700, 3710, 3720};
        assert(BatteryEstimator::trimmed_mean(short_burst, 3) == 3710);
        assert(BatteryEstimator::trimmed_mean(nullptr, 0) == 0);
    }

    // 2. LiPo SoC curve & CC/CV charge-lift compensation
    {
        assert(BatteryEstimator::voltage_to_percent(3250, false) == 0);
        assert(BatteryEstimator::voltage_to_percent(3300, false) == 0);
        assert(BatteryEstimator::voltage_to_percent(3450, false) == 5);
        assert(BatteryEstimator::voltage_to_percent(3720, false) == 45);
        assert(BatteryEstimator::voltage_to_percent(4150, false) == 100);
        assert(BatteryEstimator::voltage_to_percent(4210, false) == 100);

        // At the same terminal voltage in CC charging, estimated SoC is compensated downward
        int dis_pct = BatteryEstimator::voltage_to_percent(3850, false);
        int chg_pct = BatteryEstimator::voltage_to_percent(3850, true);
        assert(chg_pct < dis_pct);
        assert(dis_pct - chg_pct >= 10);
    }

    // 3. Hysteresis stability against +/-5 mV ADC jitter & Low/Critical battery flags
    {
        BatteryEstimator est;
        uint32_t t = 1000;
        BatteryTelemetry tel = est.update(t, 3780, 2345, false); // knot boundary 60%
        assert(tel.valid && tel.battery_present && !tel.usb_present);
        assert(tel.state == BATT_STATE_DISCHARGING);
        assert(tel.percent == 60);
        assert(!tel.low_battery && !tel.critical_battery);

        for (int i = 0; i < 20; i++) {
            t += 500;
            int noisy_mv = (i % 2 == 0) ? 3775 : 3785;
            tel = est.update(t, noisy_mv, 2345, false);
            assert(tel.percent == 60);
        }

        // Boundary check: 20% is NOT low_battery; strictly below 20% (19%) IS low_battery
        assert(BatteryEstimator::voltage_to_percent(3607, false) == 20);
        assert(BatteryEstimator::voltage_to_percent(3601, false) == 19);
        for (int i = 0; i < 16; i++) {
            t += 500;
            tel = est.update(t, 3607, 2238, false);
        }
        assert(tel.percent == 20 && !tel.low_battery && !tel.critical_battery);

        for (int i = 0; i < 16; i++) {
            t += 500;
            tel = est.update(t, 3595, 2230, false);
        }
        assert(tel.percent < 20 && tel.low_battery && !tel.critical_battery);

        for (int i = 0; i < 16; i++) {
            t += 500;
            tel = est.update(t, 3480, 2160, false);
        }
        assert(tel.low_battery && tel.critical_battery);
    }

    // 4. Discharging -> USB Host Charging -> CV Charged -> Unplug to Discharging
    {
        BatteryEstimator est;
        uint32_t t = 0;
        BatteryTelemetry tel = est.update(t, 3760, 2330, false);
        assert(tel.state == BATT_STATE_DISCHARGING);
        assert(!tel.charging && !tel.charged);

        // Plug into USB host (SOF frames active): voltage lifts +75 mV
        for (int i = 0; i < 6; i++) {
            t += 500;
            tel = est.update(t, 3835, 2380, true);
        }
        assert(tel.state == BATT_STATE_CHARGING);
        assert(tel.charging && !tel.charged && tel.usb_present && tel.usb_host);
        assert(tel.percent > 0 && tel.percent <= 99);

        // Approach CV float (4182 mV) and hold flat for >20s -> CHARGED (100%)
        for (int i = 0; i < 60; i++) {
            t += 500;
            tel = est.update(t, 4182, 2595, true);
        }
        assert(tel.state == BATT_STATE_CHARGED);
        assert(tel.charged && !tel.charging && tel.percent == 100);

        // Unplug USB host -> immediately transitions to On Battery (DISCHARGING) and stays there over 50s
        for (int i = 0; i < 100; i++) {
            t += 500;
            tel = est.update(t, 4155, 2578, false);
            assert(tel.state == BATT_STATE_DISCHARGING);
            assert(!tel.charging && !tel.charged && !tel.usb_present);
        }
    }

    // 5. Dumb USB wall-brick (no USB SOF frames): step-up detection + 60s slope + unplug droop
    {
        BatteryEstimator est;
        uint32_t t = 0;
        BatteryTelemetry tel = {};
        for (int i = 0; i < 10; i++) {
            t += 500;
            tel = est.update(t, 3740, 2320, false);
        }
        assert(tel.state == BATT_STATE_DISCHARGING);

        // Plug into wall charger (no USB SOF frames, +65 mV step)
        for (int i = 0; i < 8; i++) {
            t += 500;
            tel = est.update(t, 3805, 2360, false);
        }
        assert(tel.state == BATT_STATE_CHARGING);
        assert(tel.charging && tel.usb_present && !tel.usb_host);

        // Unplug wall charger (-65 mV droop) -> clears wall-charge latch
        for (int i = 0; i < 8; i++) {
            t += 500;
            tel = est.update(t, 3740, 2320, false);
        }
        assert(tel.state == BATT_STATE_DISCHARGING);
        assert(!tel.charging && !tel.usb_present);

        // Slow steady charge rise (+14 mV over 50s without initial step) also latches charging
        int v = 3740;
        for (int i = 0; i < 110; i++) {
            t += 500;
            v = 3740 + (i * 15) / 110;
            tel = est.update(t, v, 2325, false);
        }
        assert(tel.state == BATT_STATE_CHARGING);
        assert(tel.slope_mv_per_min > 0);
    }

    // 6. External 5V Schottky OR-ing rail (> 4380 mV) with & without prior battery observation
    {
        // Cold boot on 5V rail without prior cell reading -> USB_POWER (-1%)
        BatteryEstimator cold_usb;
        BatteryTelemetry tel = cold_usb.update(1000, 4880, 3028, true);
        assert(tel.valid && tel.usb_present && !tel.battery_present);
        assert(tel.state == BATT_STATE_USB_POWER);
        assert(tel.percent == -1);

        // Unplug from 5V rail onto 3.86V battery -> immediate reset to cell reading (75%)
        tel = cold_usb.update(2000, 3860, 2395, false);
        assert(tel.state == BATT_STATE_DISCHARGING);
        assert(tel.battery_present && !tel.usb_present);
        assert(tel.voltage_mv == 3860);
        assert(tel.percent == 75);

        // Plug back into 5V rail after cell was observed -> preserves last known SoC & shows CHARGING
        uint32_t t5 = 3000;
        tel = cold_usb.update(t5, 4880, 3028, true);
        for (int i = 0; i < 6; i++) {
            t5 += 500;
            tel = cold_usb.update(t5, 4880, 3028, true);
        }
        assert(tel.state == BATT_STATE_CHARGING);
        assert(tel.battery_present && tel.charging && !tel.charged && tel.percent == 75);

        // Hold 5V rail at 4880 mV past 3 min to baseline VBUS, then simulate +60 mV load-release step (4940 mV) flat for 24s -> CHARGED
        for (int i = 0; i < 200; i++) {
            t5 += 1000;
            tel = cold_usb.update(t5, 4880, 3028, true);
        }
        assert(tel.state == BATT_STATE_CHARGING && tel.percent >= 75 && tel.percent <= 99);
        for (int i = 0; i < 30; i++) {
            t5 += 1000;
            tel = cold_usb.update(t5, 4940, 3065, true);
        }
        assert(tel.state == BATT_STATE_CHARGED);
        assert(tel.charged && !tel.charging && tel.percent == 100);
    }

    assert(std::strcmp(battery_state_slug(BATT_STATE_CHARGING), "charging") == 0);
    assert(std::strcmp(battery_state_label(BATT_STATE_CHARGED), "Charged") == 0);
    std::printf("PASS battery estimator trimmed mean, SoC curve, hysteresis, USB/wall charging, CV full, and 5V rail.\n");
    return 0;
}
'''

(ROOT / 'scratch').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='battery-test-', dir=ROOT / 'scratch') as directory:
    temp = pathlib.Path(directory)
    (temp / 'test.cpp').write_text(CPP_TEST)
    subprocess.run(
        ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'include'),
         str(temp / 'test.cpp'), '-o', str(temp / 'test')],
        check=True,
    )
    subprocess.run([str(temp / 'test')], check=True)

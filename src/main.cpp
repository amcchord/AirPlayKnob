#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include <Preferences.h>
#include <mdns.h>
#include <ArduinoOTA.h>
#include <sodium.h>
#include <esp_system.h>
#include <ctype.h>
#include <lvgl.h>

#include "app_state.h"
#include "bsp/lcd_bsp.h"
#include "bsp/cst816.h"
#include "bsp/lcd_bl_pwm_bsp.h"
#include "bsp/lcd_config.h"
#include "bsp/bidi_switch_knob.h"
#include "radio_streamer.h"
#include "artwork_fetcher.h"
#include "network_diagnostics.h"
#include "battery_monitor.h"
#include "knob_ui.h"
#include "web_portal.h"

AppState g_app = {};
static Preferences s_prefs;
static volatile int32_t s_knob_delta = 0;
static portMUX_TYPE s_knob_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_saved_speaker_name[48] = {0};
static char s_saved_speaker_ip[20] = {0};
static bool s_user_selected_local_dac = false;
static bool s_initial_speaker_autoselected = false;

// Deferred NVS persistence flags (flushed exclusively on loopTask's Internal SRAM stack!)
static volatile bool s_save_presets_dirty = false;
static volatile bool s_save_playback_dirty = false;
static volatile uint32_t s_save_playback_dirty_ms = 0;
static volatile bool s_save_wifi_dirty = false;
static volatile bool s_save_ap2_pair_dirty = false;
static volatile bool s_save_pref_bssid_dirty = false;

struct WifiBssidPenalty {
    uint8_t bssid[6];
    uint8_t strikes;
    uint32_t last_stall_ms;
};
static WifiBssidPenalty s_bssid_penalties[8] = {};
static uint8_t s_preferred_bssid[6] = {0};
static char s_preferred_bssid_ssid[36] = {0};
static bool s_has_preferred_bssid = false;
static volatile bool s_wifi_bssid_failover_requested = false;
static uint32_t s_last_bssid_failover_ms = 0;

// In-memory AirPlay 2 Ed25519 Client Identity & Paired Speaker IPs
static char s_ap2_client_id[37] = {0};
static uint8_t s_ap2_client_ltpk[32] = {0};
static uint8_t s_ap2_client_ltsk[64] = {0};
static char s_ap2_paired_ips[MAX_SPEAKERS][20] = {{0}};
static int s_ap2_paired_count = 0;
static char s_ap2_pending_ip[20] = {0};

static void knob_left_cb(void *arg, void *data) {
    (void)arg; (void)data;
    portENTER_CRITICAL(&s_knob_mux);
    s_knob_delta = s_knob_delta - 1;
    portEXIT_CRITICAL(&s_knob_mux);
}

static void knob_right_cb(void *arg, void *data) {
    (void)arg; (void)data;
    portENTER_CRITICAL(&s_knob_mux);
    s_knob_delta = s_knob_delta + 1;
    portEXIT_CRITICAL(&s_knob_mux);
}

void app_state_save_presets(void) {
    s_save_presets_dirty = true;
}

void app_state_reset_default_presets(void) {
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_app.preset_count = DEFAULT_SOMAFM_PRESET_COUNT;
        memcpy(g_app.presets, DEFAULT_SOMAFM_PRESETS, sizeof(DEFAULT_SOMAFM_PRESETS));
        g_app.current_preset = 0;
        g_app.preview_preset = 0;
        g_app.stream_reconfig_needed = true;
        xSemaphoreGive(g_app.mutex);
    }
    s_save_presets_dirty = true;
}

void app_state_save_playback(void) {
    s_save_playback_dirty_ms = millis();
    s_save_playback_dirty = true;
}

void app_state_save_wifi(const char *ssid, const char *pass) {
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        strncpy(g_app.wifi_ssid, ssid, sizeof(g_app.wifi_ssid) - 1);
        strncpy(g_app.wifi_pass, pass, sizeof(g_app.wifi_pass) - 1);
        if (s_has_preferred_bssid && strcasecmp(s_preferred_bssid_ssid, g_app.wifi_ssid) != 0) {
            s_has_preferred_bssid = false;
            s_save_pref_bssid_dirty = true;
        }
        g_app.wifi_reconnect_requested = true;
        xSemaphoreGive(g_app.mutex);
    }
    s_save_wifi_dirty = true;
}

void app_get_ap2_client_identity(char id_out[37], uint8_t ltpk_out[32], uint8_t ltsk_out[64]) {
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        memcpy(id_out, s_ap2_client_id, 37);
        memcpy(ltpk_out, s_ap2_client_ltpk, 32);
        memcpy(ltsk_out, s_ap2_client_ltsk, 64);
        xSemaphoreGive(g_app.mutex);
    }
}

bool app_is_ap2_speaker_paired(const char *ip) {
    if (!ip || !ip[0]) return false;
    bool found = false;
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < s_ap2_paired_count; i++) {
            if (strcmp(s_ap2_paired_ips[i], ip) == 0) {
                found = true;
                break;
            }
        }
        xSemaphoreGive(g_app.mutex);
    }
    return found;
}

void app_mark_ap2_speaker_paired(const char *ip) {
    if (!ip || !ip[0]) return;
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        bool exists = false;
        for (int i = 0; i < s_ap2_paired_count; i++) {
            if (strcmp(s_ap2_paired_ips[i], ip) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists && s_ap2_paired_count < MAX_SPEAKERS) {
            strncpy(s_ap2_paired_ips[s_ap2_paired_count], ip, sizeof(s_ap2_paired_ips[0]) - 1);
            s_ap2_paired_count++;
        }
        strncpy(s_ap2_pending_ip, ip, sizeof(s_ap2_pending_ip) - 1);
        s_save_ap2_pair_dirty = true;
        xSemaphoreGive(g_app.mutex);
    }
}

static void app_state_flush_prefs(void) {
    if (s_save_playback_dirty && (uint32_t)(millis() - s_save_playback_dirty_ms) >= 500) {
        int cur_p = 0, vol = 18, km = 0, wc_p = 0;
        bool playing = true, also_dac = true, wc_act = false, sonos_native = true;
        AirPlaySpeaker cur_sp = {};
        bool has_sp = false;
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            cur_p = g_app.current_preset;
            vol = g_app.volume;
            playing = g_app.playing;
            also_dac = g_app.also_local_dac;
            sonos_native = g_app.prefer_sonos_native;
            km = (int)g_app.knob_mode;
            wc_act = g_app.whitecap_active;
            wc_p = g_app.whitecap_preset;
            if (g_app.current_speaker >= 0 && g_app.current_speaker < g_app.speaker_count) {
                cur_sp = g_app.speakers[g_app.current_speaker];
                has_sp = true;
            }
            s_save_playback_dirty = false;
            xSemaphoreGive(g_app.mutex);
        } else {
            // A busy UI must not overwrite saved settings with defaults.
            return;
        }
        s_prefs.begin("airplayknob", false);
        s_prefs.putInt("cur_preset", cur_p);
        s_prefs.putInt("volume", vol);
        s_prefs.putBool("playing", playing);
        s_prefs.putBool("also_dac", also_dac);
        s_prefs.putBool("sonos_radio", sonos_native);
        s_prefs.putInt("knob_mode", km);
        s_prefs.putBool("wc_active", wc_act);
        s_prefs.putInt("wc_preset", wc_p);
        if (has_sp && cur_sp.name[0]) {
            s_prefs.putString("spk_name", cur_sp.name);
            s_prefs.putBool("use_loc_dac", cur_sp.is_local_dac);
            if (!cur_sp.is_local_dac) {
                if (cur_sp.ip[0]) {
                    s_prefs.putString("spk_ip", cur_sp.ip);
                }
                s_prefs.putBytes("saved_spk", &cur_sp, sizeof(AirPlaySpeaker));
            }
        }
        s_prefs.end();
    }

    if (s_save_presets_dirty) {
        s_save_presets_dirty = false;
        s_prefs.begin("airplayknob", false);
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_prefs.putInt("preset_cnt", g_app.preset_count);
            s_prefs.putBytes("presets", g_app.presets, sizeof(StationPreset) * g_app.preset_count);
            xSemaphoreGive(g_app.mutex);
        }
        s_prefs.end();
    }

    if (s_save_wifi_dirty) {
        s_save_wifi_dirty = false;
        char ssid[36] = {0}, pass[64] = {0};
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            strncpy(ssid, g_app.wifi_ssid, sizeof(ssid) - 1);
            strncpy(pass, g_app.wifi_pass, sizeof(pass) - 1);
            xSemaphoreGive(g_app.mutex);
        }
        s_prefs.begin("airplayknob", false);
        s_prefs.putString("wifi_ssid", ssid);
        s_prefs.putString("wifi_pass", pass);
        s_prefs.end();
    }

    if (s_save_ap2_pair_dirty) {
        s_save_ap2_pair_dirty = false;
        char ip_buf[20] = {0};
        char list_buf[512] = {0};
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            strncpy(ip_buf, s_ap2_pending_ip, sizeof(ip_buf) - 1);
            for (int i = 0; i < s_ap2_paired_count; i++) {
                if (i > 0) strncat(list_buf, ",", sizeof(list_buf) - strlen(list_buf) - 1);
                strncat(list_buf, s_ap2_paired_ips[i], sizeof(list_buf) - strlen(list_buf) - 1);
            }
            xSemaphoreGive(g_app.mutex);
        }
        Preferences ap2_prefs;
        ap2_prefs.begin("ap2pair", false);
        if (ip_buf[0]) {
            ap2_prefs.putBool(ip_buf, true);
        }
        ap2_prefs.putString("paired_ips", list_buf);
        ap2_prefs.end();
    }

    if (s_save_pref_bssid_dirty) {
        s_save_pref_bssid_dirty = false;
        uint8_t bssid_copy[6] = {0};
        char ssid_copy[36] = {0};
        bool has_pref = false;
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            has_pref = s_has_preferred_bssid;
            memcpy(bssid_copy, s_preferred_bssid, 6);
            strncpy(ssid_copy, s_preferred_bssid_ssid, sizeof(ssid_copy) - 1);
            xSemaphoreGive(g_app.mutex);
        }
        s_prefs.begin("airplayknob", false);
        if (has_pref && ssid_copy[0] != '\0') {
            s_prefs.putBytes("pref_bssid", bssid_copy, 6);
            s_prefs.putString("pref_bssid_s", ssid_copy);
        } else {
            s_prefs.remove("pref_bssid");
            s_prefs.remove("pref_bssid_s");
        }
        s_prefs.end();
    }
}

void app_report_wifi_stream_stall(void) {
    if (WiFi.status() != WL_CONNECTED) return;
    const uint8_t *bp = WiFi.BSSID();
    if (!bp) return;
    uint8_t cur[6];
    memcpy(cur, bp, 6);
    uint8_t new_strikes = 1;
    uint32_t now_ms = millis();
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        int slot = -1;
        int oldest_slot = 0;
        uint32_t oldest_age = 0;
        for (int i = 0; i < 8; i++) {
            if (s_bssid_penalties[i].strikes > 0 &&
                (now_ms - s_bssid_penalties[i].last_stall_ms) >= 1800000U) {
                s_bssid_penalties[i].strikes = 0;
            }
            if (s_bssid_penalties[i].strikes > 0 &&
                memcmp(s_bssid_penalties[i].bssid, cur, 6) == 0) {
                slot = i;
                break;
            }
            if (slot < 0 && s_bssid_penalties[i].strikes == 0) {
                slot = i;
            }
            uint32_t age = now_ms - s_bssid_penalties[i].last_stall_ms;
            if (age >= oldest_age) {
                oldest_age = age;
                oldest_slot = i;
            }
        }
        if (slot < 0) slot = oldest_slot;
        memcpy(s_bssid_penalties[slot].bssid, cur, 6);
        if (s_bssid_penalties[slot].strikes < 3) {
            s_bssid_penalties[slot].strikes++;
        }
        s_bssid_penalties[slot].last_stall_ms = now_ms;
        new_strikes = s_bssid_penalties[slot].strikes;

        if (s_has_preferred_bssid && memcmp(s_preferred_bssid, cur, 6) == 0) {
            s_has_preferred_bssid = false;
            s_save_pref_bssid_dirty = true;
        }
        s_wifi_bssid_failover_requested = true;
        xSemaphoreGive(g_app.mutex);
    }
    Serial.printf("[WIFI] Stream stall strike %u recorded on BSSID=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  (unsigned)new_strikes, cur[0], cur[1], cur[2], cur[3], cur[4], cur[5]);
}

void app_mark_wifi_stream_healthy(void) {
    if (WiFi.status() != WL_CONNECTED) return;
    const uint8_t *bp = WiFi.BSSID();
    if (!bp) return;
    uint8_t cur[6];
    memcpy(cur, bp, 6);
    bool updated_pref = false;
    char ssid_log[36] = {0};
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < 8; i++) {
            if (s_bssid_penalties[i].strikes > 0 &&
                memcmp(s_bssid_penalties[i].bssid, cur, 6) == 0) {
                s_bssid_penalties[i].strikes = 0;
            }
        }
        if (g_app.wifi_ssid[0] != '\0' &&
            (!s_has_preferred_bssid ||
             memcmp(s_preferred_bssid, cur, 6) != 0 ||
             strcasecmp(s_preferred_bssid_ssid, g_app.wifi_ssid) != 0)) {
            memcpy(s_preferred_bssid, cur, 6);
            strncpy(s_preferred_bssid_ssid, g_app.wifi_ssid, sizeof(s_preferred_bssid_ssid) - 1);
            s_preferred_bssid_ssid[sizeof(s_preferred_bssid_ssid) - 1] = '\0';
            s_has_preferred_bssid = true;
            s_save_pref_bssid_dirty = true;
            strncpy(ssid_log, g_app.wifi_ssid, sizeof(ssid_log) - 1);
            updated_pref = true;
        }
        xSemaphoreGive(g_app.mutex);
    }
    if (updated_pref) {
        Serial.printf("[WIFI] Marked BSSID=%02X:%02X:%02X:%02X:%02X:%02X verified-healthy for '%s'\n",
                      cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], ssid_log);
    }
}

void app_select_preset(int index, bool auto_play) {
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (index >= 0 && index < g_app.preset_count) {
            g_app.current_preset = index;
            g_app.preview_preset = index;
            if (auto_play) g_app.playing = true;
            g_app.stream_reconfig_needed = true;
        }
        xSemaphoreGive(g_app.mutex);
    }
    app_state_save_playback();
}

void app_select_speaker(int index) {
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (index >= 0 && index < g_app.speaker_count) {
            g_app.current_speaker = index;
            g_app.preview_speaker = index;
            g_app.pin_halted_ip[0] = '\0';
            g_app.pin_manual_request = false;
            if (g_app.pin_prompt_active) {
                g_app.pin_cancelled = true;
                g_app.pin_prompt_active = false;
            }
            s_initial_speaker_autoselected = true;
            s_user_selected_local_dac = g_app.speakers[index].is_local_dac;
            strncpy(s_saved_speaker_name, g_app.speakers[index].name, sizeof(s_saved_speaker_name) - 1);
            strncpy(s_saved_speaker_ip, g_app.speakers[index].ip, sizeof(s_saved_speaker_ip) - 1);
        }
        xSemaphoreGive(g_app.mutex);
    }
    app_state_save_playback();
}

void app_set_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_app.volume = vol;
        g_app.volume_dirty = true;
        xSemaphoreGive(g_app.mutex);
    }
    app_state_save_playback();
}

void app_toggle_play(void) {
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_app.playing = !g_app.playing;
        g_app.stream_reconfig_needed = true;
        if (g_app.playing) {
            g_app.pin_halted_ip[0] = '\0';
        } else if (g_app.pin_prompt_active) {
            g_app.pin_cancelled = true;
            g_app.pin_prompt_active = false;
        }
        xSemaphoreGive(g_app.mutex);
    }
    app_state_save_playback();
}

void app_submit_airplay_pin(const char *pin) {
    if (!pin) return;
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        strncpy(g_app.pin_code, pin, sizeof(g_app.pin_code) - 1);
        g_app.pin_code[sizeof(g_app.pin_code) - 1] = '\0';
        g_app.pin_submitted = true;
        g_app.pin_cancelled = false;
        xSemaphoreGive(g_app.mutex);
    }
}

void app_cancel_airplay_pin(void) {
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_app.pin_cancelled = true;
        g_app.pin_submitted = false;
        g_app.pin_prompt_active = false;
        g_app.pin_manual_request = false;
        if (g_app.pin_speaker_ip[0] != '\0') {
            strncpy(g_app.pin_halted_ip, g_app.pin_speaker_ip, sizeof(g_app.pin_halted_ip) - 1);
            g_app.pin_halted_ip[sizeof(g_app.pin_halted_ip) - 1] = '\0';
        } else if (g_app.current_speaker >= 0 && g_app.current_speaker < g_app.speaker_count) {
            strncpy(g_app.pin_halted_ip, g_app.speakers[g_app.current_speaker].ip, sizeof(g_app.pin_halted_ip) - 1);
            g_app.pin_halted_ip[sizeof(g_app.pin_halted_ip) - 1] = '\0';
        }
        snprintf(g_app.status_text, sizeof(g_app.status_text), "PIN Cancelled (%s)",
                 g_app.pin_speaker_name[0] ? g_app.pin_speaker_name : "AirPlay");
        xSemaphoreGive(g_app.mutex);
    }
}

void app_request_airplay_pin_pair(void) {
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (g_app.current_speaker >= 0 && g_app.current_speaker < g_app.speaker_count &&
            !g_app.speakers[g_app.current_speaker].is_local_dac) {
            g_app.pin_halted_ip[0] = '\0';
            g_app.pin_cancelled = false;
            g_app.pin_manual_request = true;
            g_app.playing = true;
        } else {
            g_app.pin_manual_request = false;
        }
        xSemaphoreGive(g_app.mutex);
    }
}

void app_notify_pin_toast(const char *title, const char *msg, uint32_t color) {
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        strncpy(g_app.pin_toast_title, title ? title : "AIRPLAY", sizeof(g_app.pin_toast_title) - 1);
        g_app.pin_toast_title[sizeof(g_app.pin_toast_title) - 1] = '\0';
        strncpy(g_app.pin_toast_msg, msg ? msg : "", sizeof(g_app.pin_toast_msg) - 1);
        g_app.pin_toast_msg[sizeof(g_app.pin_toast_msg) - 1] = '\0';
        g_app.pin_toast_color = color;
        g_app.pin_toast_pending = true;
        xSemaphoreGive(g_app.mutex);
    }
}

static bool speaker_should_use_ap2(const char *am, const char *et) {
    return (et && (strchr(et, '4') != NULL || strchr(et, '5') != NULL)) ||
           (am && (strcasestr(am, "AudioAccessory") != NULL ||
                   strcasestr(am, "AppleTV") != NULL ||
                   strcasestr(am, "HomePod") != NULL ||
                   strcasestr(am, "Mac") != NULL ||
                   strcasestr(am, "UPL") != NULL ||
                   strcasestr(am, "WiiM") != NULL ||
                   strcasestr(am, "Amp") != NULL ||
                   strcasestr(am, "Port") != NULL ||
                   strcasestr(am, "Sub") != NULL ||
                   strcasestr(am, "Five") != NULL ||
                   strcasestr(am, "One") != NULL ||
                   strcasestr(am, "Era") != NULL ||
                   strcasestr(am, "Arc") != NULL ||
                   strcasestr(am, "Beam") != NULL ||
                   strcasestr(am, "Ray") != NULL ||
                   strcasestr(am, "Roam") != NULL ||
                   strcasestr(am, "Move") != NULL ||
                   strcasestr(am, "SYMFONISK") != NULL ||
                   strcasestr(am, "Sonos") != NULL));
}

void app_state_init(void) {
    memset(&g_app, 0, sizeof(g_app));
    g_app.mutex = xSemaphoreCreateMutex();

    s_prefs.begin("airplayknob", true);
    int saved_cnt = s_prefs.getInt("preset_cnt", 0);
    size_t expected_len = (size_t)saved_cnt * sizeof(StationPreset);
    if (saved_cnt > 0 && saved_cnt <= MAX_PRESETS && s_prefs.getBytesLength("presets") == expected_len) {
        g_app.preset_count = saved_cnt;
        s_prefs.getBytes("presets", g_app.presets, expected_len);
    } else {
        g_app.preset_count = DEFAULT_SOMAFM_PRESET_COUNT;
        memcpy(g_app.presets, DEFAULT_SOMAFM_PRESETS, sizeof(DEFAULT_SOMAFM_PRESETS));
    }

    g_app.current_preset = s_prefs.getInt("cur_preset", 0);
    if (g_app.current_preset < 0 || g_app.current_preset >= g_app.preset_count) {
        g_app.current_preset = 0;
    }
    g_app.preview_preset = g_app.current_preset;
    g_app.volume = s_prefs.getInt("volume", 18);
    if (g_app.volume < 0 || g_app.volume > 100) {
        g_app.volume = 18;
    }
    g_app.playing = s_prefs.getBool("playing", true);
    int saved_km = s_prefs.getInt("knob_mode", (int)KNOB_MODE_VOLUME);
    if (saved_km < 0 || saved_km > 2) saved_km = (int)KNOB_MODE_VOLUME;
    g_app.knob_mode = (KnobMode)saved_km;
    g_app.also_local_dac = s_prefs.getBool("also_dac", true);
    g_app.prefer_sonos_native = s_prefs.getBool("sonos_radio", true);
    g_app.whitecap_active = s_prefs.getBool("wc_active", false);
    g_app.whitecap_preset = s_prefs.getInt("wc_preset", 0) & 3;

    String w_ssid = s_prefs.getString("wifi_ssid", DEFAULT_WIFI_SSID);
    String w_pass = s_prefs.getString("wifi_pass", DEFAULT_WIFI_PASS);
    String def_ssid = s_prefs.getString("def_ssid", "");
    bool update_def_wifi = (DEFAULT_WIFI_SSID[0] != '\0' && def_ssid != DEFAULT_WIFI_SSID);
#ifdef FALLBACK_WIFI_SSID
    update_def_wifi |= (DEFAULT_WIFI_SSID[0] != '\0' && w_ssid == FALLBACK_WIFI_SSID);
#endif
    if (update_def_wifi) {
        w_ssid = DEFAULT_WIFI_SSID;
        w_pass = DEFAULT_WIFI_PASS;
    }
    String spk_n  = s_prefs.getString("spk_name", "");
    String spk_ip = s_prefs.getString("spk_ip", "");
    bool use_loc_dac = s_prefs.getBool("use_loc_dac", false);
    uint8_t pref_bssid_tmp[6] = {0};
    String pref_bssid_s = s_prefs.getString("pref_bssid_s", "");
    bool has_pref_bssid = (s_prefs.getBytesLength("pref_bssid") == 6 &&
                           s_prefs.getBytes("pref_bssid", pref_bssid_tmp, 6) == 6 &&
                           pref_bssid_s.length() > 0);
    AirPlaySpeaker saved_spk = {};
    bool has_saved_spk_struct = false;
    if (s_prefs.getBytesLength("saved_spk") == sizeof(AirPlaySpeaker)) {
        s_prefs.getBytes("saved_spk", &saved_spk, sizeof(AirPlaySpeaker));
        if (!saved_spk.is_local_dac && saved_spk.name[0] != '\0') {
            has_saved_spk_struct = true;
        }
    }
    s_prefs.end();

    if (update_def_wifi) {
        s_prefs.begin("airplayknob", false);
        s_prefs.putString("def_ssid", DEFAULT_WIFI_SSID);
        s_prefs.putString("wifi_ssid", w_ssid);
        s_prefs.putString("wifi_pass", w_pass);
        s_prefs.end();
    }

    strncpy(g_app.wifi_ssid, w_ssid.c_str(), sizeof(g_app.wifi_ssid) - 1);
    strncpy(g_app.wifi_pass, w_pass.c_str(), sizeof(g_app.wifi_pass) - 1);
    if (has_pref_bssid && strcasecmp(pref_bssid_s.c_str(), g_app.wifi_ssid) == 0) {
        memcpy(s_preferred_bssid, pref_bssid_tmp, 6);
        strncpy(s_preferred_bssid_ssid, pref_bssid_s.c_str(), sizeof(s_preferred_bssid_ssid) - 1);
        s_preferred_bssid_ssid[sizeof(s_preferred_bssid_ssid) - 1] = '\0';
        s_has_preferred_bssid = true;
    }
    strncpy(s_saved_speaker_name, spk_n.c_str(), sizeof(s_saved_speaker_name) - 1);
    strncpy(s_saved_speaker_ip, spk_ip.c_str(), sizeof(s_saved_speaker_ip) - 1);
    s_user_selected_local_dac = use_loc_dac || (strcasecmp(s_saved_speaker_name, "Local 3.5mm Jack") == 0);

    // Load or generate AirPlay 2 persistent Ed25519 client identity & paired IPs on loopTask (Internal SRAM stack!)
    Preferences ap2_prefs;
    ap2_prefs.begin("ap2pair", false);
    String cid = ap2_prefs.getString("client_id", "");
    size_t sk_len = ap2_prefs.getBytesLength("client_sk");
    size_t pk_len = ap2_prefs.getBytesLength("client_pk");
    if (cid.length() == 36 && sk_len == 64 && pk_len == 32) {
        strncpy(s_ap2_client_id, cid.c_str(), 36);
        s_ap2_client_id[36] = '\0';
        ap2_prefs.getBytes("client_sk", s_ap2_client_ltsk, 64);
        ap2_prefs.getBytes("client_pk", s_ap2_client_ltpk, 32);
    } else {
        uint8_t rnd[16];
        esp_fill_random(rnd, sizeof(rnd));
        snprintf(s_ap2_client_id, sizeof(s_ap2_client_id),
                 "%02X%02X%02X%02X-%02X%02X-4%01X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                 rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6] & 0x0f, rnd[7],
                 (rnd[8] & 0x3f) | 0x80, rnd[9], rnd[10], rnd[11], rnd[12], rnd[13], rnd[14], rnd[15]);
        crypto_sign_ed25519_keypair(s_ap2_client_ltpk, s_ap2_client_ltsk);
        ap2_prefs.putString("client_id", s_ap2_client_id);
        ap2_prefs.putBytes("client_sk", s_ap2_client_ltsk, 64);
        ap2_prefs.putBytes("client_pk", s_ap2_client_ltpk, 32);
    }
    String paired_csv = ap2_prefs.getString("paired_ips", "");
    ap2_prefs.end();

    s_ap2_paired_count = 0;
    if (paired_csv.length() > 0) {
        char csv_copy[512];
        strncpy(csv_copy, paired_csv.c_str(), sizeof(csv_copy) - 1);
        csv_copy[sizeof(csv_copy) - 1] = '\0';
        char *saveptr = NULL;
        for (char *tok = strtok_r(csv_copy, ",", &saveptr);
             tok != NULL && s_ap2_paired_count < MAX_SPEAKERS;
             tok = strtok_r(NULL, ",", &saveptr)) {
            if (tok[0]) {
                strncpy(s_ap2_paired_ips[s_ap2_paired_count++], tok, sizeof(s_ap2_paired_ips[0]) - 1);
            }
        }
    }

    g_app.wifi_connect_start_ms = millis();
    g_app.wifi_speed_mbps = 0;
    strncpy(g_app.wifi_phy_mode, "802.11n", sizeof(g_app.wifi_phy_mode) - 1);

    // Pre-populate saved AirPlay speaker in slot 0 so boot resumes the exact speaker immediately without
    // starting on Local 3.5mm Jack; discover other receivers through mDNS.
    if (!s_user_selected_local_dac &&
        (has_saved_spk_struct ||
         (s_saved_speaker_name[0] != '\0' && strcasecmp(s_saved_speaker_name, "Local 3.5mm Jack") != 0))) {
        int cnt = 0;
        if (has_saved_spk_struct) {
            g_app.speakers[cnt] = saved_spk;
        } else {
            memset(&g_app.speakers[cnt], 0, sizeof(AirPlaySpeaker));
            strncpy(g_app.speakers[cnt].name, s_saved_speaker_name, sizeof(g_app.speakers[cnt].name) - 1);
            strncpy(g_app.speakers[cnt].ip, s_saved_speaker_ip, sizeof(g_app.speakers[cnt].ip) - 1);
            g_app.speakers[cnt].port = 7000;
            strncpy(g_app.speakers[cnt].et, "0,4", sizeof(g_app.speakers[cnt].et) - 1);
            strncpy(g_app.speakers[cnt].cn, "0,1", sizeof(g_app.speakers[cnt].cn) - 1);
            strncpy(g_app.speakers[cnt].md, "0,1,2", sizeof(g_app.speakers[cnt].md) - 1);
        }
        g_app.speakers[cnt].is_local_dac = false;
        g_app.speakers[cnt].is_ap2 = speaker_should_use_ap2(g_app.speakers[cnt].am, g_app.speakers[cnt].et);
        g_app.speakers[cnt].last_seen_ms = millis();
        cnt++;

        memset(&g_app.speakers[cnt], 0, sizeof(AirPlaySpeaker));
        strncpy(g_app.speakers[cnt].name, "Local 3.5mm Jack", sizeof(g_app.speakers[cnt].name) - 1);
        g_app.speakers[cnt].is_local_dac = true;
        g_app.speakers[cnt].last_seen_ms = millis();
        cnt++;

        g_app.speaker_count = cnt;
        g_app.current_speaker = 0;
        g_app.preview_speaker = 0;
        s_initial_speaker_autoselected = true;
    } else {
        g_app.speaker_count = 1;
        strncpy(g_app.speakers[0].name, "Local 3.5mm Jack", sizeof(g_app.speakers[0].name) - 1);
        g_app.speakers[0].is_local_dac = true;
        g_app.current_speaker = 0;
        g_app.preview_speaker = 0;
        if (s_user_selected_local_dac) {
            s_initial_speaker_autoselected = true;
        }
    }

    strncpy(g_app.status_text, "Joining Wi-Fi...", sizeof(g_app.status_text));
    strncpy(g_app.track_title, "SomaFM Internet Radio", sizeof(g_app.track_title));
}

struct PendingMdnsEntry {
    char instance[72];
    char hostname[64];
    char service_type[12]; // "_raop" or "_airplay"
    uint16_t port;
    bool is_raop;
    bool has_txt;
    size_t txt_count;
    char et[24];
    char cn[24];
    char md[24];
    char am[32];
    bool req_pin_pw;
    uint32_t sf_flags;
    bool has_sf_flags;
};

static constexpr int MAX_PENDING_MDNS = 72;

struct MdnsScanScratch {
    AirPlaySpeaker found[MAX_SPEAKERS];
    PendingMdnsEntry pending[MAX_PENDING_MDNS];
};

static void strip_trailing_local(const char *src, char *dst, size_t dst_sz) {
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!src || !src[0]) return;
    strncpy(dst, src, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
    size_t len = strlen(dst);
    while (len > 0 && dst[len - 1] == '.') {
        dst[--len] = '\0';
    }
    if (len > 6 && strcasecmp(dst + len - 6, ".local") == 0) {
        dst[len - 6] = '\0';
    }
}

static int score_lan_ipv4(const esp_ip4_addr_t &ip4, const IPAddress &my_ip) {
    uint8_t a = esp_ip4_addr1_16(&ip4);
    uint8_t b = esp_ip4_addr2_16(&ip4);
    uint8_t c = esp_ip4_addr3_16(&ip4);
    uint8_t d = esp_ip4_addr4_16(&ip4);
    if (a == 0 || a == 127 || d == 0 || d == 255) return -1;
    if (a == 169 && b == 254) return 1; // Link-local fallback only
    if (my_ip[0] != 0 && a == my_ip[0] && b == my_ip[1] && c == my_ip[2]) return 100;
    if (my_ip[0] != 0 && a == my_ip[0] && b == my_ip[1]) return 80;
    if (a == 192 && b == 168) return 60;
    if (a == 10) return 55;
    if (a == 172 && b >= 16 && b <= 31) return 50;
    return 20;
}

static int extract_best_ipv4(const mdns_ip_addr_t *addr_list, const IPAddress &my_ip, char *out_ip, size_t out_sz) {
    int best_score = -1;
    for (const mdns_ip_addr_t *a = addr_list; a != NULL; a = a->next) {
        if (a->addr.type != ESP_IPADDR_TYPE_V4) continue;
        int sc = score_lan_ipv4(a->addr.u_addr.ip4, my_ip);
        if (sc > best_score) {
            best_score = sc;
            snprintf(out_ip, out_sz, IPSTR, IP2STR(&(a->addr.u_addr.ip4)));
        }
    }
    return best_score;
}

static void parse_airplay_txt_items(const mdns_txt_item_t *txt, size_t txt_count,
                                    char *et, size_t et_sz,
                                    char *cn, size_t cn_sz,
                                    char *md, size_t md_sz,
                                    char *am, size_t am_sz,
                                    bool *req_pin_pw,
                                    uint32_t *sf_flags,
                                    bool *has_sf_flags) {
    for (size_t t = 0; t < txt_count && txt != NULL; t++) {
        const char *k = txt[t].key;
        const char *v = txt[t].value ? txt[t].value : "";
        if (!k) continue;
        if (strcasecmp(k, "et") == 0 && v[0]) strncpy(et, v, et_sz - 1);
        else if (strcasecmp(k, "cn") == 0 && v[0]) strncpy(cn, v, cn_sz - 1);
        else if (strcasecmp(k, "md") == 0 && v[0]) strncpy(md, v, md_sz - 1);
        else if ((strcasecmp(k, "am") == 0 || strcasecmp(k, "model") == 0) && v[0]) strncpy(am, v, am_sz - 1);
        else if (strcasecmp(k, "pw") == 0 && (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0)) {
            *req_pin_pw = true;
        } else if (strcasecmp(k, "sf") == 0 || strcasecmp(k, "flags") == 0) {
            *sf_flags = (uint32_t)strtoul(v, NULL, 0);
            *has_sf_flags = true;
        } else if (strcasecmp(k, "ft") == 0 || strcasecmp(k, "features") == 0) {
            unsigned long long low = 0, high = 0;
            if (sscanf(v, "%llx,%llx", &low, &high) == 2 && (high & 0x11C040ULL) != 0) {
                if (et[0] == '\0') {
                    strncpy(et, "0,4", et_sz - 1);
                } else if (strchr(et, '4') == NULL && strchr(et, '5') == NULL) {
                    strncat(et, ",4", et_sz - strlen(et) - 1);
                }
            }
        }
    }
}

static void clean_airplay_display_name(const char *raw_inst, const char *am, char *sp_name, size_t sp_name_sz) {
    const char *inst = (raw_inst && raw_inst[0]) ? raw_inst : "AirPlay Speaker";
    const char *at = strchr(inst, '@');
    strncpy(sp_name, (at && *(at + 1)) ? (at + 1) : inst, sp_name_sz - 1);
    sp_name[sp_name_sz - 1] = '\0';

    // Trim trailing whitespace
    size_t len = strlen(sp_name);
    while (len > 0 && sp_name[len - 1] == ' ') {
        sp_name[--len] = '\0';
    }

    // Strip Bonjour collision suffix like " (2)" .. " (9)" when Apple/HomePod devices re-register
    if (len >= 4 && sp_name[len - 4] == ' ' && sp_name[len - 3] == '(' &&
        sp_name[len - 2] >= '2' && sp_name[len - 2] <= '9' && sp_name[len - 1] == ')') {
        sp_name[len - 4] = '\0';
    }

    if (am && strcasestr(am, "AudioAccessory") != NULL &&
        strcasestr(sp_name, "HomePod") == NULL &&
        strcasestr(sp_name, "Home Pod") == NULL) {
        strncat(sp_name, " (HomePod)", sp_name_sz - strlen(sp_name) - 1);
    }
}

static void upsert_discovered_speaker(AirPlaySpeaker *found, int *count_inout,
                                      const char *ip_str, uint16_t port,
                                      const char *clean_host, const char *raw_inst,
                                      bool is_raop, size_t txt_count,
                                      const char *et, const char *cn, const char *md, const char *am,
                                      bool req_pin_pw, uint32_t sf_flags, bool has_sf_flags,
                                      uint32_t now_ms) {
    if (!ip_str || ip_str[0] == '\0' || strcmp(ip_str, "0.0.0.0") == 0) return;
    int count = *count_inout;

    int existing_idx = -1;
    for (int i = 0; i < count; i++) {
        if (!found[i].is_local_dac && found[i].ip[0] != '\0' && strcmp(found[i].ip, ip_str) == 0) {
            existing_idx = i;
            break;
        }
    }

    const char *eff_am = (am && am[0]) ? am : ((existing_idx >= 0) ? found[existing_idx].am : "");
    char sp_name[48] = {0};
    clean_airplay_display_name(raw_inst, eff_am, sp_name, sizeof(sp_name));

    if (existing_idx < 0 && clean_host && clean_host[0]) {
        char full_host[64];
        snprintf(full_host, sizeof(full_host), "%s.local", clean_host);
        for (int i = 0; i < count; i++) {
            if (!found[i].is_local_dac && found[i].host[0] != '\0' &&
                strcasecmp(found[i].host, full_host) == 0) {
                existing_idx = i;
                break;
            }
        }
    }
    if (existing_idx < 0) {
        for (int i = 0; i < count; i++) {
            if (!found[i].is_local_dac && strcasecmp(found[i].name, sp_name) == 0 &&
                (found[i].ip[0] == '\0' || (eff_am[0] && strcasecmp(found[i].am, eff_am) == 0))) {
                existing_idx = i;
                break;
            }
        }
    }

    bool req_pin = req_pin_pw;
    if (has_sf_flags) {
        if ((sf_flags & 0x08U) != 0) {
            req_pin = true;
        } else if ((sf_flags & 0x200U) != 0 && !app_is_ap2_speaker_paired(ip_str)) {
            req_pin = true;
        }
    }

    if (existing_idx >= 0) {
        AirPlaySpeaker &sp = found[existing_idx];
        sp.last_seen_ms = now_ms;
        strncpy(sp.ip, ip_str, sizeof(sp.ip) - 1);
        if (is_raop) {
            if (port > 0) sp.port = port;
            sp.has_raop = true;
        } else if (!sp.has_raop || sp.port == 0) {
            sp.port = port ? port : 7000;
        }
        if (clean_host && clean_host[0]) {
            snprintf(sp.host, sizeof(sp.host), "%s.local", clean_host);
        }
        if (sp_name[0] && strcmp(sp_name, "AirPlay Speaker") != 0) {
            if (is_raop || !sp.has_raop || sp.name[0] == '\0' || strcmp(sp.name, "AirPlay Speaker") == 0) {
                strncpy(sp.name, sp_name, sizeof(sp.name) - 1);
            } else if (strcasestr(eff_am, "AudioAccessory") != NULL &&
                       strcasestr(sp.name, "HomePod") == NULL &&
                       strcasestr(sp.name, "Home Pod") == NULL) {
                strncat(sp.name, " (HomePod)", sizeof(sp.name) - strlen(sp.name) - 1);
            }
        }
        if (txt_count > 0) {
            if (et && et[0]) strncpy(sp.et, et, sizeof(sp.et) - 1);
            if (cn && cn[0]) strncpy(sp.cn, cn, sizeof(sp.cn) - 1);
            if (md && md[0]) strncpy(sp.md, md, sizeof(sp.md) - 1);
            if (am && am[0]) {
                strncpy(sp.am, am, sizeof(sp.am) - 1);
                if (strcasestr(sp.am, "AudioAccessory") != NULL &&
                    strcasestr(sp.name, "HomePod") == NULL &&
                    strcasestr(sp.name, "Home Pod") == NULL) {
                    strncat(sp.name, " (HomePod)", sizeof(sp.name) - strlen(sp.name) - 1);
                }
            }
        }
        if (req_pin) sp.requires_pin = true;
        sp.is_ap2 = speaker_should_use_ap2(sp.am, sp.et);
        return;
    }

    if (count >= MAX_SPEAKERS - 1) return;

    AirPlaySpeaker &sp = found[count];
    memset(&sp, 0, sizeof(sp));
    sp.port = port ? port : 7000;
    sp.has_raop = is_raop;
    sp.last_seen_ms = now_ms;
    strncpy(sp.ip, ip_str, sizeof(sp.ip) - 1);
    if (clean_host && clean_host[0]) {
        snprintf(sp.host, sizeof(sp.host), "%s.local", clean_host);
    }
    strncpy(sp.name, sp_name, sizeof(sp.name) - 1);
    strncpy(sp.et, (et && et[0]) ? et : "0,1", sizeof(sp.et) - 1);
    strncpy(sp.cn, (cn && cn[0]) ? cn : "0,1", sizeof(sp.cn) - 1);
    if (md && md[0]) strncpy(sp.md, md, sizeof(sp.md) - 1);
    if (am && am[0]) strncpy(sp.am, am, sizeof(sp.am) - 1);
    sp.requires_pin = req_pin;
    sp.is_ap2 = speaker_should_use_ap2(sp.am, sp.et);
    *count_inout = count + 1;
}

static void collect_mdns_results(mdns_result_t *results, bool is_raop, const IPAddress &my_ip,
                                 MdnsScanScratch *scratch, int *count_inout, int *pending_inout,
                                 uint32_t now_ms) {
    const char *svc_name = is_raop ? "_raop" : "_airplay";
    for (mdns_result_t *r = results; r != NULL; r = r->next) {
        char clean_host[64] = {0};
        strip_trailing_local(r->hostname, clean_host, sizeof(clean_host));

        char ip_str[20] = {0};
        int ip_score = extract_best_ipv4(r->addr, my_ip, ip_str, sizeof(ip_str));

        char et[24] = {0}, cn[24] = {0}, md[24] = {0}, am[32] = {0};
        bool req_pin_pw = false;
        uint32_t sf_flags = 0;
        bool has_sf_flags = false;
        parse_airplay_txt_items(r->txt, r->txt_count, et, sizeof(et), cn, sizeof(cn),
                                md, sizeof(md), am, sizeof(am),
                                &req_pin_pw, &sf_flags, &has_sf_flags);

        const char *inst = r->instance_name ? r->instance_name : "";
        bool has_good_ip = (ip_score >= 20 && ip_str[0] != '\0');
        bool has_srv = (r->port > 0 || clean_host[0] != '\0');
        bool has_txt = (r->txt_count > 0);

        if (has_good_ip && has_srv && has_txt) {
            upsert_discovered_speaker(scratch->found, count_inout,
                                      ip_str, r->port ? r->port : 7000,
                                      clean_host, inst, is_raop, r->txt_count,
                                      et, cn, md, am, req_pin_pw, sf_flags, has_sf_flags, now_ms);
            continue;
        }

        // Queue incomplete PTR/SRV/TXT/A record for targeted unicast/multicast resolution
        if (!inst[0] && !clean_host[0]) continue;
        int p_idx = -1;
        for (int i = 0; i < *pending_inout; i++) {
            PendingMdnsEntry &pe = scratch->pending[i];
            if (pe.is_raop == is_raop &&
                ((inst[0] && strcasecmp(pe.instance, inst) == 0) ||
                 (!inst[0] && clean_host[0] && strcasecmp(pe.hostname, clean_host) == 0))) {
                p_idx = i;
                break;
            }
        }
        if (p_idx < 0) {
            if (*pending_inout >= MAX_PENDING_MDNS) {
                if (ip_score > 0 && ip_str[0] != '\0') {
                    upsert_discovered_speaker(scratch->found, count_inout,
                                              ip_str, r->port ? r->port : 7000,
                                              clean_host, inst, is_raop, r->txt_count,
                                              et, cn, md, am, req_pin_pw, sf_flags, has_sf_flags, now_ms);
                }
                continue;
            }
            p_idx = (*pending_inout)++;
            memset(&scratch->pending[p_idx], 0, sizeof(PendingMdnsEntry));
            scratch->pending[p_idx].is_raop = is_raop;
            strncpy(scratch->pending[p_idx].service_type, svc_name, sizeof(scratch->pending[p_idx].service_type) - 1);
            if (inst[0]) strncpy(scratch->pending[p_idx].instance, inst, sizeof(scratch->pending[p_idx].instance) - 1);
        }

        PendingMdnsEntry &pe = scratch->pending[p_idx];
        if (clean_host[0] && !pe.hostname[0]) {
            strncpy(pe.hostname, clean_host, sizeof(pe.hostname) - 1);
        }
        if (r->port > 0 && pe.port == 0) {
            pe.port = r->port;
        }
        if (r->txt_count > 0 && !pe.has_txt) {
            pe.has_txt = true;
            pe.txt_count = r->txt_count;
            strncpy(pe.et, et, sizeof(pe.et) - 1);
            strncpy(pe.cn, cn, sizeof(pe.cn) - 1);
            strncpy(pe.md, md, sizeof(pe.md) - 1);
            strncpy(pe.am, am, sizeof(pe.am) - 1);
            pe.req_pin_pw = req_pin_pw;
            pe.sf_flags = sf_flags;
            pe.has_sf_flags = has_sf_flags;
        }
        if (has_good_ip && pe.has_txt && (pe.port > 0 || pe.hostname[0])) {
            upsert_discovered_speaker(scratch->found, count_inout,
                                      ip_str, pe.port ? pe.port : 7000,
                                      pe.hostname, pe.instance, pe.is_raop, pe.txt_count,
                                      pe.et, pe.cn, pe.md, pe.am, pe.req_pin_pw, pe.sf_flags, pe.has_sf_flags, now_ms);
        }
    }
}

static int compare_speaker_names_alpha(const char *a, const char *b) {
    const unsigned char *pa = (const unsigned char *)(a ? a : "");
    const unsigned char *pb = (const unsigned char *)(b ? b : "");
    while (*pa && *pb) {
        // Normalize UTF-8 curly apostrophes (\xE2\x80\x98 / \xE2\x80\x99) to ASCII '\''
        unsigned char ca = *pa;
        if (ca == 0xE2 && pa[1] == 0x80 && (pa[2] == 0x98 || pa[2] == 0x99)) {
            ca = '\'';
            pa += 3;
        } else {
            pa++;
        }
        unsigned char cb = *pb;
        if (cb == 0xE2 && pb[1] == 0x80 && (pb[2] == 0x98 || pb[2] == 0x99)) {
            cb = '\'';
            pb += 3;
        } else {
            pb++;
        }
        int la = tolower((int)ca);
        int lb = tolower((int)cb);
        if (la != lb) return la - lb;
    }
    if (*pa != *pb) return (int)(*pa) - (int)(*pb);
    return strcmp(a ? a : "", b ? b : "");
}

static int find_matching_speaker_idx(const AirPlaySpeaker *list, int count,
                                     bool want_local_dac, const char *want_ip, const char *want_name) {
    if (count <= 0) return -1;
    if (want_local_dac) {
        for (int i = 0; i < count; i++) {
            if (list[i].is_local_dac) return i;
        }
        return count - 1;
    }
    if (want_ip && want_ip[0] != '\0') {
        for (int i = 0; i < count; i++) {
            if (!list[i].is_local_dac && strcmp(list[i].ip, want_ip) == 0) return i;
        }
    }
    if (want_name && want_name[0] != '\0') {
        for (int i = 0; i < count; i++) {
            if (!list[i].is_local_dac && strcasecmp(list[i].name, want_name) == 0) return i;
        }
    }
    return -1;
}

static void discover_airplay_speakers(bool manual_scan) {
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_app.speaker_scanning = true;
        g_app.speaker_scan_count++;
        g_app.speaker_scan_last_ms = millis();
        xSemaphoreGive(g_app.mutex);
    } else return;

    MdnsScanScratch *scratch = (MdnsScanScratch *)heap_caps_calloc(1, sizeof(MdnsScanScratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scratch) {
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_app.speaker_scanning = false;
            xSemaphoreGive(g_app.mutex);
        }
        return;
    }

    // Temporarily raise ESP-IDF's "mdns" task (default priority 1 on CPU 0) above radio_http (priority 4)
    // during active discovery so simultaneous responses from 15-30 LAN speakers never overflow mdns's 16-item queue!
    TaskHandle_t mdns_task = xTaskGetHandle("mdns");
    UBaseType_t orig_mdns_prio = 1;
    if (mdns_task) {
        orig_mdns_prio = uxTaskPriorityGet(mdns_task);
        if (orig_mdns_prio < 5) {
            vTaskPrioritySet(mdns_task, 5);
        }
    }

    uint32_t now_ms = millis();
    IPAddress my_ip = WiFi.localIP();
    int count = 0;
    int pending_count = 0;

    // Seed with existing speakers so previously discovered speakers and the active/saved speaker
    // remain available even if a battery-powered speaker briefly misses a multicast query.
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < g_app.speaker_count && count < MAX_SPEAKERS - 1; i++) {
            const AirPlaySpeaker &old_sp = g_app.speakers[i];
            if (old_sp.is_local_dac) continue;
            bool is_active_or_saved =
                (i == g_app.current_speaker) ||
                (s_saved_speaker_ip[0] != '\0' && strcmp(old_sp.ip, s_saved_speaker_ip) == 0) ||
                (s_saved_speaker_name[0] != '\0' && strcasecmp(old_sp.name, s_saved_speaker_name) == 0);
            if (is_active_or_saved || old_sp.ip[0] != '\0') {
                scratch->found[count++] = old_sp;
            }
        }
        xSemaphoreGive(g_app.mutex);
    }

    struct QueryPass {
        const char *service;
        mdns_query_transmission_type_t tx_type;
        uint32_t timeout_ms;
        bool is_raop;
    };
    const QueryPass passes[] = {
        { "_raop",    MDNS_QUERY_UNICAST,   manual_scan ? 700U : 550U, true  },
        { "_airplay", MDNS_QUERY_UNICAST,   manual_scan ? 550U : 450U, false },
        { "_raop",    MDNS_QUERY_MULTICAST, manual_scan ? 950U : 800U, true  },
        { "_airplay", MDNS_QUERY_MULTICAST, manual_scan ? 750U : 600U, false },
    };

    for (size_t p = 0; p < sizeof(passes) / sizeof(passes[0]); p++) {
        mdns_result_t *res = NULL;
        if (mdns_query_generic(NULL, passes[p].service, "_tcp", MDNS_TYPE_PTR,
                               passes[p].tx_type, passes[p].timeout_ms, 64, &res) == ESP_OK && res != NULL) {
            collect_mdns_results(res, passes[p].is_raop, my_ip, scratch, &count, &pending_count, now_ms);
            mdns_query_results_free(res);
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    // Pass 2: Resolve split-packet mDNS speakers where PTR arrived without inline SRV, A, or TXT records
    struct HostIpCache {
        char host[64];
        char ip[20];
        bool attempted;
    };
    HostIpCache host_cache[28] = {};
    int host_cache_cnt = 0;

    for (int i = 0; i < pending_count && count < MAX_SPEAKERS - 1; i++) {
        PendingMdnsEntry &pe = scratch->pending[i];

        // Skip redundant resolution if an earlier pass already resolved this speaker cleanly
        if (pe.hostname[0] != '\0') {
            char full_host[64];
            snprintf(full_host, sizeof(full_host), "%s.local", pe.hostname);
            bool already_complete = false;
            for (int k = 0; k < count; k++) {
                const AirPlaySpeaker &sp = scratch->found[k];
                if (!sp.is_local_dac && sp.ip[0] != '\0' && strcasecmp(sp.host, full_host) == 0 &&
                    (!pe.is_raop || sp.has_raop) && sp.am[0] != '\0') {
                    already_complete = true;
                    break;
                }
            }
            if (already_complete) continue;
        }

        if ((pe.hostname[0] == '\0' || pe.port == 0) && pe.instance[0] != '\0') {
            mdns_result_t *srv_res = NULL;
            if (mdns_query_srv(pe.instance, pe.service_type, "_tcp", 260, &srv_res) == ESP_OK && srv_res != NULL) {
                if (srv_res->hostname && srv_res->hostname[0]) {
                    strip_trailing_local(srv_res->hostname, pe.hostname, sizeof(pe.hostname));
                }
                if (srv_res->port > 0) {
                    pe.port = srv_res->port;
                }
                char inline_ip[20] = {0};
                if (extract_best_ipv4(srv_res->addr, my_ip, inline_ip, sizeof(inline_ip)) >= 20 && host_cache_cnt < 28) {
                    strncpy(host_cache[host_cache_cnt].host, pe.hostname, sizeof(host_cache[0].host) - 1);
                    strncpy(host_cache[host_cache_cnt].ip, inline_ip, sizeof(host_cache[0].ip) - 1);
                    host_cache[host_cache_cnt].attempted = true;
                    host_cache_cnt++;
                }
                mdns_query_results_free(srv_res);
            }
        }
        if (pe.hostname[0] == '\0') continue;

        // Look up or resolve IPv4 address for pe.hostname (cached per host so _raop + _airplay share 1 lookup)
        char resolved_ip[20] = {0};
        bool host_known = false;
        for (int k = 0; k < count; k++) {
            char clean_existing[64] = {0};
            strip_trailing_local(scratch->found[k].host, clean_existing, sizeof(clean_existing));
            if (!scratch->found[k].is_local_dac && scratch->found[k].ip[0] != '\0' &&
                clean_existing[0] != '\0' && strcasecmp(clean_existing, pe.hostname) == 0) {
                strncpy(resolved_ip, scratch->found[k].ip, sizeof(resolved_ip) - 1);
                host_known = true;
                break;
            }
        }
        if (!host_known) {
            for (int c = 0; c < host_cache_cnt; c++) {
                if (strcasecmp(host_cache[c].host, pe.hostname) == 0) {
                    strncpy(resolved_ip, host_cache[c].ip, sizeof(resolved_ip) - 1);
                    host_known = host_cache[c].attempted;
                    break;
                }
            }
        }
        if (!host_known) {
            esp_ip4_addr_t addr4 = {};
            int sc = -1;
            if (mdns_query_a(pe.hostname, 260, &addr4) == ESP_OK) {
                sc = score_lan_ipv4(addr4, my_ip);
                if (sc >= 20) {
                    snprintf(resolved_ip, sizeof(resolved_ip), IPSTR, IP2STR(&addr4));
                }
            }
            if (sc < 20) {
                mdns_result_t *a_res = NULL;
                if (mdns_query_generic(pe.hostname, NULL, NULL, MDNS_TYPE_A,
                                       MDNS_QUERY_MULTICAST, 280, 8, &a_res) == ESP_OK && a_res != NULL) {
                    for (mdns_result_t *ar = a_res; ar != NULL; ar = ar->next) {
                        char cand_ip[20] = {0};
                        int csc = extract_best_ipv4(ar->addr, my_ip, cand_ip, sizeof(cand_ip));
                        if (csc > sc) {
                            sc = csc;
                            strncpy(resolved_ip, cand_ip, sizeof(resolved_ip) - 1);
                        }
                    }
                    mdns_query_results_free(a_res);
                } else if (sc > 0 && resolved_ip[0] == '\0') {
                    snprintf(resolved_ip, sizeof(resolved_ip), IPSTR, IP2STR(&addr4));
                }
            }
            if (host_cache_cnt < 28) {
                strncpy(host_cache[host_cache_cnt].host, pe.hostname, sizeof(host_cache[0].host) - 1);
                strncpy(host_cache[host_cache_cnt].ip, resolved_ip, sizeof(host_cache[0].ip) - 1);
                host_cache[host_cache_cnt].attempted = true;
                host_cache_cnt++;
            }
        }
        if (resolved_ip[0] == '\0') continue;

        if (!pe.has_txt && pe.instance[0] != '\0') {
            mdns_result_t *txt_res = NULL;
            if (mdns_query_txt(pe.instance, pe.service_type, "_tcp", 240, &txt_res) == ESP_OK && txt_res != NULL) {
                parse_airplay_txt_items(txt_res->txt, txt_res->txt_count,
                                        pe.et, sizeof(pe.et), pe.cn, sizeof(pe.cn),
                                        pe.md, sizeof(pe.md), pe.am, sizeof(pe.am),
                                        &pe.req_pin_pw, &pe.sf_flags, &pe.has_sf_flags);
                pe.txt_count = txt_res->txt_count;
                pe.has_txt = (txt_res->txt_count > 0);
                mdns_query_results_free(txt_res);
            }
        }

        upsert_discovered_speaker(scratch->found, &count,
                                  resolved_ip, pe.port ? pe.port : 7000,
                                  pe.hostname, pe.instance, pe.is_raop, pe.txt_count,
                                  pe.et, pe.cn, pe.md, pe.am,
                                  pe.req_pin_pw, pe.sf_flags, pe.has_sf_flags, now_ms);
    }

    if (mdns_task && orig_mdns_prio < 5) {
        vTaskPrioritySet(mdns_task, orig_mdns_prio);
    }

    // Sort discovered AirPlay speakers alphabetically A -> Z (case-insensitive)
    for (int i = 1; i < count; i++) {
        AirPlaySpeaker key = scratch->found[i];
        int j = i - 1;
        while (j >= 0 && compare_speaker_names_alpha(scratch->found[j].name, key.name) > 0) {
            scratch->found[j + 1] = scratch->found[j];
            j--;
        }
        scratch->found[j + 1] = key;
    }

    bool need_save_resolved_spk = false;
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Always append Local 3.5mm Audio Jack as the final option
        AirPlaySpeaker &local_sp = scratch->found[count++];
        memset(&local_sp, 0, sizeof(local_sp));
        strncpy(local_sp.name, "Local 3.5mm Jack", sizeof(local_sp.name) - 1);
        local_sp.is_local_dac = true;
        local_sp.last_seen_ms = now_ms;

        char prev_selected_name[48] = {0};
        char prev_selected_ip[20] = {0};
        bool prev_was_local_dac = false;
        if (g_app.current_speaker >= 0 && g_app.current_speaker < g_app.speaker_count) {
            strncpy(prev_selected_name, g_app.speakers[g_app.current_speaker].name, sizeof(prev_selected_name) - 1);
            strncpy(prev_selected_ip, g_app.speakers[g_app.current_speaker].ip, sizeof(prev_selected_ip) - 1);
            prev_was_local_dac = g_app.speakers[g_app.current_speaker].is_local_dac;
        }

        char prev_preview_name[48] = {0};
        char prev_preview_ip[20] = {0};
        bool prev_preview_local = false;
        bool had_preview = (g_app.preview_speaker >= 0 && g_app.preview_speaker < g_app.speaker_count);
        if (had_preview) {
            strncpy(prev_preview_name, g_app.speakers[g_app.preview_speaker].name, sizeof(prev_preview_name) - 1);
            strncpy(prev_preview_ip, g_app.speakers[g_app.preview_speaker].ip, sizeof(prev_preview_ip) - 1);
            prev_preview_local = g_app.speakers[g_app.preview_speaker].is_local_dac;
        }

        memcpy(g_app.speakers, scratch->found, sizeof(AirPlaySpeaker) * count);
        g_app.speaker_count = count;
        g_app.speaker_list_seq++;

        int new_sel = -1;
        // 1. Preserve current active speaker across alphabetical re-sorts
        if (s_initial_speaker_autoselected && (!prev_was_local_dac || s_user_selected_local_dac)) {
            new_sel = find_matching_speaker_idx(g_app.speakers, count, prev_was_local_dac, prev_selected_ip, prev_selected_name);
        }
        // 2. Match saved speaker from NVS (exact IP -> exact name -> substring fallback)
        if (new_sel < 0 && !s_user_selected_local_dac) {
            new_sel = find_matching_speaker_idx(g_app.speakers, count, false, s_saved_speaker_ip, s_saved_speaker_name);
            if (new_sel >= 0) {
                s_initial_speaker_autoselected = true;
            } else if (s_saved_speaker_name[0] != '\0') {
                for (int i = 0; i < count; i++) {
                    if (!g_app.speakers[i].is_local_dac &&
                        strcasestr(g_app.speakers[i].name, s_saved_speaker_name) != NULL) {
                        new_sel = i;
                        s_initial_speaker_autoselected = true;
                        break;
                    }
                }
            }
        }
        // With no saved receiver, leave output local until the user selects one.
        if (new_sel < 0) {
            new_sel = count - 1;
        }

        g_app.current_speaker = new_sel;
        if (g_app.preview_timestamp_ms != 0 && had_preview) {
            int mapped_prev = find_matching_speaker_idx(g_app.speakers, count, prev_preview_local, prev_preview_ip, prev_preview_name);
            g_app.preview_speaker = (mapped_prev >= 0) ? mapped_prev : new_sel;
        } else {
            g_app.preview_speaker = new_sel;
        }

        if (new_sel >= 0 && new_sel < count && !g_app.speakers[new_sel].is_local_dac &&
            g_app.speakers[new_sel].ip[0] != '\0') {
            if (strcmp(s_saved_speaker_ip, g_app.speakers[new_sel].ip) != 0) {
                strncpy(s_saved_speaker_ip, g_app.speakers[new_sel].ip, sizeof(s_saved_speaker_ip) - 1);
                need_save_resolved_spk = true;
            }
        }
        g_app.speaker_scanning = false;
        xSemaphoreGive(g_app.mutex);
    }
    Serial.printf("[MDNS] Speaker discovery complete: %d AirPlay speakers (+ Local 3.5mm Jack)\n", count > 0 ? count - 1 : 0);
    if (need_save_resolved_spk) {
        app_state_save_playback();
    }
    heap_caps_free(scratch);
}

static void update_wifi_negotiated_link_info(void) {
    wifi_ap_record_t ap_info = {};
    wifi_phy_mode_t phymode = WIFI_PHY_MODE_HT20;
    bool got_ap = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
    bool got_phy = (esp_wifi_sta_get_negotiated_phymode(&phymode) == ESP_OK);

    int32_t rssi = got_ap ? ap_info.rssi : WiFi.RSSI();
    uint8_t chan = got_ap ? ap_info.primary : (uint8_t)WiFi.channel();
    int speed_mbps = 72;
    const char *phy_str = "802.11n HT20";

    if (got_phy) {
        switch (phymode) {
            case WIFI_PHY_MODE_11B:
                phy_str = "802.11b";
                speed_mbps = (rssi >= -72) ? 11 : 5;
                break;
            case WIFI_PHY_MODE_11G:
            case WIFI_PHY_MODE_11A:
                phy_str = "802.11g";
                speed_mbps = (rssi >= -68) ? 54 : ((rssi >= -75) ? 36 : 24);
                break;
            case WIFI_PHY_MODE_HT40:
                phy_str = "802.11n HT40";
                speed_mbps = (rssi >= -65) ? 150 : ((rssi >= -72) ? 135 : ((rssi >= -78) ? 108 : 81));
                break;
            case WIFI_PHY_MODE_HE20:
                phy_str = "802.11ax HE20";
                speed_mbps = (rssi >= -65) ? 114 : ((rssi >= -74) ? 86 : 65);
                break;
            case WIFI_PHY_MODE_VHT20:
                phy_str = "802.11ac VHT20";
                speed_mbps = (rssi >= -65) ? 86 : 65;
                break;
            case WIFI_PHY_MODE_LR:
                phy_str = "802.11 LR";
                speed_mbps = 1;
                break;
            case WIFI_PHY_MODE_HT20:
            default:
                phy_str = "802.11n HT20";
                speed_mbps = (rssi >= -65) ? 72 : ((rssi >= -72) ? 65 : ((rssi >= -78) ? 52 : 39));
                break;
        }
    } else if (got_ap) {
        if (ap_info.bandwidth == WIFI_BW_HT40) {
            phy_str = "802.11n HT40";
            speed_mbps = 150;
        } else if (ap_info.phy_11n) {
            phy_str = "802.11n HT20";
            speed_mbps = 72;
        } else if (ap_info.phy_11g) {
            phy_str = "802.11g";
            speed_mbps = 54;
        } else {
            phy_str = "802.11b";
            speed_mbps = 11;
        }
    }

    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        IPAddress ip = WiFi.localIP();
        snprintf(g_app.wifi_ip, sizeof(g_app.wifi_ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        g_app.wifi_rssi = rssi;
        g_app.wifi_channel = chan;
        g_app.wifi_speed_mbps = speed_mbps;
        strncpy(g_app.wifi_phy_mode, phy_str, sizeof(g_app.wifi_phy_mode) - 1);
        g_app.wifi_phy_mode[sizeof(g_app.wifi_phy_mode) - 1] = '\0';
        if (got_ap) {
            snprintf(g_app.wifi_bssid, sizeof(g_app.wifi_bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
                     ap_info.bssid[0], ap_info.bssid[1], ap_info.bssid[2],
                     ap_info.bssid[3], ap_info.bssid[4], ap_info.bssid[5]);
        }
        xSemaphoreGive(g_app.mutex);
    }
}

static bool connect_best_wifi_ap(const char *ssid, const char *pass,
                                 const uint8_t *avoid_bssid = NULL, bool require_alternate = false) {
    if (!ssid || ssid[0] == '\0') return false;
    Serial.printf("[WIFI] Scanning for best BSSID on SSID '%s'%s...\n",
                  ssid, require_alternate ? " (failover scan)" : "");

    WifiBssidPenalty penalties[8] = {};
    uint8_t pref_bssid[6] = {0};
    char pref_ssid[36] = {0};
    bool has_pref = false;
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        memcpy(penalties, s_bssid_penalties, sizeof(penalties));
        has_pref = s_has_preferred_bssid;
        memcpy(pref_bssid, s_preferred_bssid, 6);
        strncpy(pref_ssid, s_preferred_bssid_ssid, sizeof(pref_ssid) - 1);
        xSemaphoreGive(g_app.mutex);
    }

    WiFi.scanDelete();

    // Full all-channel scan (250ms/ch) so we see all candidate APs and avoid overlapping non-standard channels (like Ch 2 / Ch 10)
    int n = WiFi.scanNetworks(false, false, false, 250);
    uint32_t now_ms = millis();
    int best_idx = -1;
    int best_score = -1000;
    for (int i = 0; i < n; i++) {
        if (strcasecmp(WiFi.SSID(i).c_str(), ssid) != 0) continue;
        const uint8_t *cand_bssid = WiFi.BSSID(i);
        if (!cand_bssid) continue;
        if (require_alternate && avoid_bssid && memcmp(cand_bssid, avoid_bssid, 6) == 0) {
            continue;
        }
        int32_t rssi_i = WiFi.RSSI(i);
        int32_t ch_i = WiFi.channel(i);
        int score = (int)rssi_i;
        // Prefer standard non-overlapping 2.4 GHz channels (1, 6, 11) where CSMA/CA works cleanly without ACI
        if (ch_i == 1 || ch_i == 6 || ch_i == 11) {
            score += 15;
        } else {
            score -= 15; // Penalize non-standard channels (e.g., Ch 2, Ch 10) that suffer severe ACI
        }
        // Penalize APs suffering from 2.4 GHz Adjacent Channel Interference (1 <= |ch_i - ch_j| <= 4)
        for (int j = 0; j < n; j++) {
            if (i == j) continue;
            int32_t ch_j = WiFi.channel(j);
            int32_t rssi_j = WiFi.RSSI(j);
            int diff = abs((int)ch_i - (int)ch_j);
            if (diff >= 1 && diff <= 4 && rssi_j >= -80) {
                score -= 2;
            }
        }
        int pen = 0;
        for (int p = 0; p < 8; p++) {
            if (penalties[p].strikes > 0 &&
                (now_ms - penalties[p].last_stall_ms) < 1800000U &&
                memcmp(penalties[p].bssid, cand_bssid, 6) == 0) {
                pen = (int)penalties[p].strikes * 28;
                break;
            }
        }
        if (avoid_bssid && memcmp(cand_bssid, avoid_bssid, 6) == 0 && pen < 28) {
            pen = 28;
        }
        score -= pen;

        bool is_pref = has_pref && (pen == 0) && (rssi_i >= -76) &&
                       (strcasecmp(pref_ssid, ssid) == 0) &&
                       (memcmp(pref_bssid, cand_bssid, 6) == 0);
        if (is_pref) {
            score += 14;
        }

        Serial.printf("[WIFI]   Candidate AP [%d]: BSSID=%s Ch=%ld RSSI=%ld dBm Pen=%d Pref=%d Score=%d\n",
                      i, WiFi.BSSIDstr(i).c_str(), (long)ch_i, (long)rssi_i, pen, (int)is_pref, score);
        if (rssi_i >= -78 && score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }
    if (require_alternate && best_idx < 0) {
        WiFi.scanDelete();
        Serial.printf("[WIFI] No alternate BSSID >= -78 dBm found for '%s'; keeping existing link.\n", ssid);
        return false;
    }
    if (best_idx >= 0) {
        uint8_t target_bssid[6];
        memcpy(target_bssid, WiFi.BSSID(best_idx), 6);
        int32_t target_ch = WiFi.channel(best_idx);
        Serial.printf("[WIFI] Connecting to '%s' locked to BSSID=%s (Ch %ld, Score %d)\n",
                      ssid, WiFi.BSSIDstr(best_idx).c_str(), (long)target_ch, best_score);
        WiFi.scanDelete();
        WiFi.disconnect(false);
        vTaskDelay(pdMS_TO_TICKS(50));
        WiFi.begin(ssid, pass, target_ch, target_bssid, true);
    } else {
        WiFi.scanDelete();
        Serial.printf("[WIFI] Connecting to SSID: %s (auto BSSID)\n", ssid);
        WiFi.disconnect(false);
        vTaskDelay(pdMS_TO_TICKS(50));
        WiFi.begin(ssid, pass);
    }
    return true;
}

static void network_manager_task(void *arg) {
    (void)arg;
    bool mdns_started = false;

    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_STA);
    // Enable 802.11b/g/n (HT20 A-MPDU TX aggregation) so 125.3 pkt/s AirPlay 2 RTP streams
    // aggregate cleanly across Knob -> AP -> HomePod without CSMA/CA TX queue starvation.
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    WiFi.setSleep(false); // Disable Wi-Fi power save for low-jitter audio & AirPlay streaming
    esp_wifi_set_ps(WIFI_PS_NONE);

    char ssid[36] = {0}, pass[64] = {0};
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        strncpy(ssid, g_app.wifi_ssid, sizeof(ssid) - 1);
        strncpy(pass, g_app.wifi_pass, sizeof(pass) - 1);
        g_app.wifi_connect_start_ms = millis();
        xSemaphoreGive(g_app.mutex);
    }

    char saved_ssid[36] = {0}, saved_pass[64] = {0};
    strncpy(saved_ssid, ssid, sizeof(saved_ssid) - 1);
    strncpy(saved_pass, pass, sizeof(saved_pass) - 1);

    struct KnownWifi {
        const char *ssid;
        const char *pass;
    };
    KnownWifi fallbacks[4] = {};
    int num_fallbacks = 0;
    if (saved_ssid[0] != '\0') {
        fallbacks[num_fallbacks++] = { saved_ssid, saved_pass };
    }
    if (DEFAULT_WIFI_SSID[0] != '\0' && strcasecmp(saved_ssid, DEFAULT_WIFI_SSID) != 0) {
        fallbacks[num_fallbacks++] = { DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS };
    }
#ifdef FALLBACK_WIFI_SSID
    if (FALLBACK_WIFI_SSID[0] != '\0' &&
        strcasecmp(saved_ssid, FALLBACK_WIFI_SSID) != 0 &&
        strcasecmp(DEFAULT_WIFI_SSID, FALLBACK_WIFI_SSID) != 0) {
        fallbacks[num_fallbacks++] = { FALLBACK_WIFI_SSID, FALLBACK_WIFI_PASS };
    }
#endif
#ifdef FALLBACK2_WIFI_SSID
    if (FALLBACK2_WIFI_SSID[0] != '\0' &&
        strcasecmp(saved_ssid, FALLBACK2_WIFI_SSID) != 0 &&
        strcasecmp(DEFAULT_WIFI_SSID, FALLBACK2_WIFI_SSID) != 0
#ifdef FALLBACK_WIFI_SSID
        && strcasecmp(FALLBACK_WIFI_SSID, FALLBACK2_WIFI_SSID) != 0
#endif
    ) {
        fallbacks[num_fallbacks++] = { FALLBACK2_WIFI_SSID, FALLBACK2_WIFI_PASS };
    }
#endif
    int fb_idx = 0;

    if (ssid[0] != '\0') {
        connect_best_wifi_ap(ssid, pass);
    } else {
        Serial.println("[WIFI] No saved Wi-Fi SSID. Swipe Down on touchscreen -> Wi-Fi Scan to connect.");
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            strncpy(g_app.status_text, "Swipe Down -> Wi-Fi Scan", sizeof(g_app.status_text) - 1);
            xSemaphoreGive(g_app.mutex);
        }
    }
    uint32_t last_wifi_attempt_ms = millis();

    uint32_t last_mdns_scan_ms = 0;
    uint32_t last_link_info_ms = 0;
    bool post_connect_followup_done = false;

    for (;;) {
        wl_status_t st = WiFi.status();
        bool connected = (st == WL_CONNECTED);

        bool do_reconnect = false;
        bool do_bssid_failover = false;
        bool do_wifi_scan = false;
        bool do_spk_scan = false;
        bool just_connected = false;
        bool modal_open = false;
        bool defer_discovery = false;

        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            bool was_connected = g_app.wifi_connected;
            g_app.wifi_connected = connected;
            if (connected) {
                if (!was_connected) {
                    IPAddress ip = WiFi.localIP();
                    snprintf(g_app.wifi_ip, sizeof(g_app.wifi_ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
                    g_app.wifi_connected_since_ms = millis();
                    just_connected = true;
                }
            } else {
                g_app.wifi_connected_since_ms = 0;
                g_app.wifi_bssid[0] = '\0';
            }
            do_reconnect = g_app.wifi_reconnect_requested;
            g_app.wifi_reconnect_requested = false;
            if (s_wifi_bssid_failover_requested) {
                s_wifi_bssid_failover_requested = false;
                if (connected && !g_app.ota_in_progress && !g_app.wifi_modal_open && !g_app.pin_prompt_active) {
                    do_bssid_failover = true;
                }
            }

            do_wifi_scan = g_app.wifi_scan_requested;
            g_app.wifi_scan_requested = false;
            if (do_wifi_scan) {
                g_app.wifi_scanning = true;
            }

            do_spk_scan = g_app.speaker_scan_requested;
            g_app.speaker_scan_requested = false;

            bool needs_ip = g_app.current_speaker >= 0 && g_app.current_speaker < g_app.speaker_count &&
                            !g_app.speakers[g_app.current_speaker].is_local_dac &&
                            !g_app.speakers[g_app.current_speaker].ip[0];
            bool native_only = g_app.prefer_sonos_native && g_app.sonos_native &&
                               !g_app.also_local_dac && !g_app.whitecap_active;
            bool local_dac_cushioned = g_app.current_speaker >= 0 && g_app.current_speaker < g_app.speaker_count &&
                                       g_app.speakers[g_app.current_speaker].is_local_dac &&
                                       !g_app.raop_connected &&
                                       g_app.status != STREAM_CONNECTING_AIRPLAY &&
                                       g_app.pcm_buf_ms >= 1800;
            bool defer = connected && discovery_must_wait(g_app.playing, native_only || local_dac_cushioned, needs_ip,
                                             last_mdns_scan_ms == 0, g_app.raop_connected,
                                             g_app.status == STREAM_CONNECTING_AIRPLAY);
            defer_discovery = defer;
            if (defer && do_spk_scan) {
                g_app.speaker_scan_requested = true;
                do_spk_scan = false;
            }
            g_app.speaker_scan_deferred = defer && g_app.speaker_scan_requested;
            if (defer && do_wifi_scan) {
                g_app.wifi_scan_requested = true;
                g_app.wifi_scanning = false;
                do_wifi_scan = false;
            }
            g_app.wifi_scan_deferred = defer && g_app.wifi_scan_requested;

            modal_open = g_app.wifi_modal_open;

            if (do_reconnect) {
                strncpy(ssid, g_app.wifi_ssid, sizeof(ssid) - 1);
                strncpy(pass, g_app.wifi_pass, sizeof(pass) - 1);
                strncpy(saved_ssid, ssid, sizeof(saved_ssid) - 1);
                strncpy(saved_pass, pass, sizeof(saved_pass) - 1);
                fallbacks[0] = { saved_ssid, saved_pass };
                fb_idx = 0;
                g_app.wifi_connect_start_ms = millis();
                g_app.wifi_connected_since_ms = 0;
                last_mdns_scan_ms = 0;
            }
            xSemaphoreGive(g_app.mutex);
        } else {
            vTaskDelay(pdMS_TO_TICKS(150));
            continue; // No snapshot means no permission to start network work.
        }

        if (connected) {
            uint32_t now_link_ms = millis();
            last_wifi_attempt_ms = now_link_ms;
            if (just_connected || last_link_info_ms == 0 || (uint32_t)(now_link_ms - last_link_info_ms) >= 3000U) {
                update_wifi_negotiated_link_info();
                last_link_info_ms = now_link_ms;
            }
            if (just_connected) {
                esp_wifi_set_ps(WIFI_PS_NONE);
                esp_wifi_set_max_tx_power(72); // 18.0 dBm balances strong ACK SNR with clean EVM inside aluminum bezel
                if (fb_idx == 0) {
                    s_save_wifi_dirty = true;
                }
                // Only force an immediate multicast mDNS scan if we have not yet populated LAN speakers;
                // avoids blasting a 2.5s multicast storm during a brief mid-playback Wi-Fi reassociation.
                if (g_app.speaker_count <= 2) {
                    last_mdns_scan_ms = 0;
                    post_connect_followup_done = false;
                }
                Serial.printf("[WIFI] Link up: %s (%d Mbps, %s, Ch %u, BSSID %s, RSSI %ld dBm, IP %s)\n",
                              ssid, g_app.wifi_speed_mbps, g_app.wifi_phy_mode,
                              g_app.wifi_channel, g_app.wifi_bssid, (long)g_app.wifi_rssi, g_app.wifi_ip);
            }
        }

        if (do_wifi_scan) {
            Serial.println("[WIFI] Starting Wi-Fi network scan...");
            if (!connected) {
                // Stop active STA connection attempt & auto-reconnect first so esp_wifi_scan_start()
                // does not immediately return ESP_ERR_WIFI_STATE (WIFI_SCAN_FAILED = -2)
                WiFi.setAutoReconnect(false);
                WiFi.disconnect(false);
                esp_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(150));
            }
            WiFi.scanDelete();
            int n = WiFi.scanNetworks(false, false);
            if (n < 0) {
                Serial.printf("[WIFI] Scan attempt 1 returned %d; resetting STA state and retrying...\n", n);
                WiFi.setAutoReconnect(false);
                WiFi.disconnect(false);
                esp_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(250));
                n = WiFi.scanNetworks(false, false);
            }
            Serial.printf("[WIFI] Scan completed: %d APs found\n", n);
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                int cnt = 0;
                for (int i = 0; i < n && cnt < MAX_WIFI_SCAN; i++) {
                    String s = WiFi.SSID(i);
                    if (s.length() == 0) continue;
                    int32_t rssi = WiFi.RSSI(i);
                    bool sec = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
                    int existing = -1;
                    for (int j = 0; j < cnt; j++) {
                        if (strcmp(g_app.wifi_scan[j].ssid, s.c_str()) == 0) {
                            existing = j;
                            break;
                        }
                    }
                    if (existing >= 0) {
                        if (rssi > g_app.wifi_scan[existing].rssi) {
                            g_app.wifi_scan[existing].rssi = rssi;
                        }
                        continue;
                    }
                    strncpy(g_app.wifi_scan[cnt].ssid, s.c_str(), sizeof(g_app.wifi_scan[cnt].ssid) - 1);
                    g_app.wifi_scan[cnt].ssid[sizeof(g_app.wifi_scan[cnt].ssid) - 1] = '\0';
                    g_app.wifi_scan[cnt].rssi = rssi;
                    g_app.wifi_scan[cnt].secure = sec;
                    cnt++;
                }
                g_app.wifi_scan_count = cnt;
                g_app.wifi_scanning = false;
                g_app.wifi_scan_done_seq++;
                xSemaphoreGive(g_app.mutex);
            }
            WiFi.scanDelete();
            if (!connected) {
                WiFi.setAutoReconnect(true);
                last_wifi_attempt_ms = millis();
            }
        }

        uint32_t wait_elapsed = millis() - last_wifi_attempt_ms;
        if (do_bssid_failover && !do_reconnect && connected && ssid[0] != '\0' &&
            (s_last_bssid_failover_ms == 0 || (uint32_t)(millis() - s_last_bssid_failover_ms) >= 25000U)) {
            uint8_t cur_bssid[6] = {0};
            const uint8_t *bp = WiFi.BSSID();
            if (bp) memcpy(cur_bssid, bp, 6);
            s_last_bssid_failover_ms = millis();
            if (connect_best_wifi_ap(ssid, pass, bp ? cur_bssid : NULL, true)) {
                last_wifi_attempt_ms = millis();
                connected = false;
            }
        }
        if (do_reconnect) {
            Serial.printf("[WIFI] Reconnecting to %s...\n", ssid);
            WiFi.setAutoReconnect(true);
            WiFi.disconnect(false);
            vTaskDelay(pdMS_TO_TICKS(150));
            connect_best_wifi_ap(ssid, pass);
            last_wifi_attempt_ms = millis();
        } else if (!connected && !do_wifi_scan && !g_app.wifi_scanning && !modal_open && ssid[0] != '\0' &&
                   ((st == WL_NO_SSID_AVAIL && wait_elapsed >= 8000) || wait_elapsed >= 12000)) {
            if (num_fallbacks > 1) {
                fb_idx = (fb_idx + 1) % num_fallbacks;
                strncpy(ssid, fallbacks[fb_idx].ssid, sizeof(ssid) - 1);
                strncpy(pass, fallbacks[fb_idx].pass, sizeof(pass) - 1);
                if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    strncpy(g_app.wifi_ssid, ssid, sizeof(g_app.wifi_ssid) - 1);
                    strncpy(g_app.wifi_pass, pass, sizeof(g_app.wifi_pass) - 1);
                    xSemaphoreGive(g_app.mutex);
                }
            }
            Serial.printf("[WIFI] Trying Wi-Fi SSID '%s' (prev status=%d)...\n", ssid, (int)st);
            WiFi.disconnect(false);
            vTaskDelay(pdMS_TO_TICKS(100));
            connect_best_wifi_ap(ssid, pass);
            last_wifi_attempt_ms = millis();
        }

        if (connected && !mdns_started) {
            if (mdns_init() == ESP_OK) {
                mdns_hostname_set("airplayknob");
                mdns_instance_name_set("AirPlayKnob Radio Streamer");
                mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
                mdns_service_add(NULL, "_arduino", "_tcp", 3232, NULL, 0);
                vTaskDelay(pdMS_TO_TICKS(150)); // Let IGMP multicast membership settle before first PTR query
            }
            ArduinoOTA.setHostname("airplayknob");
            ArduinoOTA.setMdnsEnabled(false); // Prevent ArduinoOTA from re-initializing ESP-IDF mdns
            ArduinoOTA.onStart([]() {
                Serial.println("[OTA] ArduinoOTA Start");
                if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_app.ota_in_progress = true;
                    g_app.ota_progress_pct = 0;
                    xSemaphoreGive(g_app.mutex);
                }
            });
            ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
                int pct = total > 0 ? (int)((progress * 100U) / total) : 0;
                if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    g_app.ota_progress_pct = pct;
                    xSemaphoreGive(g_app.mutex);
                }
            });
            ArduinoOTA.onEnd([]() {
                Serial.println("\n[OTA] ArduinoOTA Complete");
            });
            ArduinoOTA.onError([](ota_error_t error) {
                Serial.printf("[OTA] Error[%u]\n", error);
                if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_app.ota_in_progress = false;
                    xSemaphoreGive(g_app.mutex);
                }
            });
            ArduinoOTA.begin();
            mdns_started = true;
            Serial.printf("[WIFI] Connected! IP=%s (http://airplayknob.local/ | OTA port 3232)\n", WiFi.localIP().toString().c_str());
        }

        if (connected && mdns_started) {
            ArduinoOTA.handle();
        }

        // Run initial mDNS speaker scan immediately upon Wi-Fi connect (before HTTP stream starts),
        // one quiet follow-up scan ~12s later (when safe / non-AirPlay) to catch sleepy battery speakers,
        // and thereafter ONLY scan when safe, paused, or if the selected speaker still needs its IP resolved.
        uint32_t scan_interval_ms = 30000;
        bool active_spk_needs_ip =
            (g_app.current_speaker >= 0 && g_app.current_speaker < g_app.speaker_count &&
             !g_app.speakers[g_app.current_speaker].is_local_dac &&
             g_app.speakers[g_app.current_speaker].ip[0] == '\0');
        bool discovery_needed = active_spk_needs_ip || !g_app.playing;
        bool initial_scan_due = (last_mdns_scan_ms == 0);
        bool followup_window_reached = !post_connect_followup_done && (last_mdns_scan_ms > 0) &&
                                       (g_app.wifi_connected_since_ms > 0) &&
                                       (millis() - g_app.wifi_connected_since_ms >= 12000U);
        if (followup_window_reached && g_app.speaker_count > 2) {
            post_connect_followup_done = true;
        }
        bool followup_scan_due = followup_window_reached && !post_connect_followup_done &&
                                 (g_app.speaker_count <= 2) &&
                                 (g_app.status != STREAM_CONNECTING_AIRPLAY);
        bool periodic_scan_due = (last_mdns_scan_ms > 0) &&
                                 (millis() - last_mdns_scan_ms >= scan_interval_ms) &&
                                 discovery_needed &&
                                 (g_app.status != STREAM_CONNECTING_AIRPLAY);
        if (connected && !defer_discovery && !g_app.ota_in_progress && !g_app.pin_prompt_active &&
            (do_spk_scan || initial_scan_due || followup_scan_due || periodic_scan_due)) {
            if (followup_scan_due || do_spk_scan) {
                post_connect_followup_done = true;
            }
            discover_airplay_speakers(do_spk_scan || followup_scan_due);
            last_mdns_scan_ms = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static int s_wifi_custom_init_err = -1;

extern "C" esp_err_t __real_esp_wifi_init(const wifi_init_config_t *config);
extern "C" esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t *config) {
    wifi_init_config_t cfg = *config;
    cfg.static_rx_buf_num = 16;
    cfg.dynamic_rx_buf_num = 64;
    cfg.rx_ba_win = 12;           // < static_rx_buf_num; 12-slot 802.11n AMPDU reorder window prevents BA drops
    cfg.tx_buf_type = 1;          // DYNAMIC_TX_BUFFER (required when CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=1)
    cfg.static_tx_buf_num = 0;
    cfg.dynamic_tx_buf_num = 48;  // On-demand Internal DMA SRAM TX buffers (~90KB free Internal SRAM headroom)
    cfg.cache_tx_buf_num = 64;    // 64 PSRAM->MAC staging slots (~500ms cushion for PSRAM lwIP pbufs, 0B Internal SRAM)
    cfg.feature_caps |= CONFIG_FEATURE_CACHE_TX_BUF_BIT;
    cfg.csi_enable = 0;
    esp_err_t err = __real_esp_wifi_init(&cfg);
    s_wifi_custom_init_err = (int)err;
    if (err != ESP_OK) {
        return __real_esp_wifi_init(config);
    }
    return err;
}

void setup() {
    // Hold backlight LOW immediately on power-up so the display never flashes white before LVGL renders
    pinMode(EXAMPLE_PIN_NUM_BK_LIGHT, OUTPUT);
    digitalWrite(EXAMPLE_PIN_NUM_BK_LIGHT, LOW);

    Serial.begin(115200);
    delay(200);
    Serial.println("\n==========================================");
    Serial.println("  AirPlayKnob - SomaFM to AirPlay Streamer");
    Serial.println("==========================================");
    Serial.printf("[BOOT] Build=%s reset_reason=%d\n", AIRPLAYKNOB_BUILD_ID, (int)esp_reset_reason());

    app_state_init();
    battery_monitor_init();

    Serial.printf("[BOOT] Free Internal Heap: %u bytes, Free PSRAM: %u bytes\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

    // Initialize Wi-Fi STA early so Wi-Fi MAC DMA buffers get contiguous internal SRAM
    WiFi.useStaticBuffers(false);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_STA);
    Serial.printf("[WIFI-INIT] custom_cfg_err=%d free_heap=%u free_psram=%u\n",
                  s_wifi_custom_init_err, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    // Enable 802.11b/g/n (HT20 A-MPDU TX aggregation) so 125.3 pkt/s AirPlay 2 RTP streams
    // aggregate cleanly across Knob -> AP -> HomePod without CSMA/CA TX queue starvation.
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power(72); // 18.0 dBm ceiling balances strong ACK SNR with clean EVM inside CNC aluminum ring

    // Initialize Touch + DRV2605 Haptics + 360x360 QSPI Display (with backlight still OFF)
    Touch_Init();
    lcd_lvgl_Init();

    // Initialize Bidirectional Rotary Encoder (GPIO 8 & 7)
    knob_config_t kcfg = {
        .gpio_encoder_a = EXAMPLE_ENCODER_ECA_PIN,
        .gpio_encoder_b = EXAMPLE_ENCODER_ECB_PIN,
    };
    knob_handle_t knob = iot_knob_create(&kcfg);
    if (knob) {
        iot_knob_register_cb(knob, KNOB_LEFT, knob_left_cb, NULL);
        iot_knob_register_cb(knob, KNOB_RIGHT, knob_right_cb, NULL);
    }

    // Start Album Art / Stream Art Fetcher before UI so image descriptor is ready
    artwork_fetcher_start();

    // Build LVGL Circular UI (starts on the black-background animated Wi-Fi connecting screen)
    knob_ui_init();
    if (g_app.whitecap_active) {
        knob_ui_set_whitecap(true);
    }

    // Let LVGL flush the first pure-black frame to SH8601 GRAM before turning on PWM backlight
    delay(40);
    lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);

    // Start Wi-Fi + mDNS discovery task (Internal SRAM stack so Wi-Fi/NVS/OTA never panic), Web Portal, and Streamer
    xTaskCreatePinnedToCore(network_manager_task, "net_mgr", 8192, NULL, 3, NULL, 0);
    web_portal_start();
    radio_streamer_start();
}

volatile bool g_screenshot_dump_active = false;

static void dump_serial_screenshot(void) {
    const uint16_t *fb = bsp_get_screen_fb();
    if (!fb) {
        Serial.println("[DBG] No screen framebuffer");
        return;
    }
    g_screenshot_dump_active = true;
    delay(5);
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    // Stream 180x180 downsampled RGB565 (big-endian) in base64 over USB-Serial/JTAG
    Serial.println("\n===SCREENSHOT_BEGIN 180 180===");
    uint8_t row_bytes[180 * 2];
    char b64_line[484];
    for (int y = 0; y < 180; y++) {
        const uint16_t *src = &fb[(y * 2) * EXAMPLE_LCD_H_RES];
        for (int x = 0; x < 180; x++) {
            uint16_t px = src[x * 2]; // Already byte-swapped (big-endian RGB565)
            row_bytes[x * 2]     = (uint8_t)(px & 0xFF);
            row_bytes[x * 2 + 1] = (uint8_t)(px >> 8);
        }
        int out_i = 0;
        for (int i = 0; i < 360; i += 3) {
            uint32_t v = ((uint32_t)row_bytes[i] << 16) | ((uint32_t)row_bytes[i + 1] << 8) | row_bytes[i + 2];
            b64_line[out_i++] = B64[(v >> 18) & 0x3F];
            b64_line[out_i++] = B64[(v >> 12) & 0x3F];
            b64_line[out_i++] = B64[(v >> 6)  & 0x3F];
            b64_line[out_i++] = B64[ v        & 0x3F];
        }
        b64_line[out_i] = '\0';
        Serial.println(b64_line);
    }
    Serial.println("===SCREENSHOT_END===");
    g_screenshot_dump_active = false;
}

static void poll_serial_debug(void) {
    static char s_cmd[64];
    static int s_len = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            if (s_len > 0) {
                s_cmd[s_len] = '\0';
                s_len = 0;
                if (strcasecmp(s_cmd, "SCREENSHOT") == 0) {
                    dump_serial_screenshot();
                } else if (strncasecmp(s_cmd, "TAB ", 4) == 0) {
                    int t = atoi(s_cmd + 4);
                    knob_ui_set_tab(t);
                    Serial.printf("[DBG] Switched to TAB %d\n", t);
                } else if (strncasecmp(s_cmd, "WHITECAP ", 9) == 0) {
                    int w = atoi(s_cmd + 9);
                    knob_ui_set_whitecap(w != 0);
                    Serial.printf("[DBG] WhiteCap=%d\n", w != 0);
                } else if (strncasecmp(s_cmd, "ROTATE ", 7) == 0) {
                    int d = atoi(s_cmd + 7);
                    knob_ui_on_rotate(d);
                    Serial.printf("[DBG] Rotate delta=%d\n", d);
                } else if (strncasecmp(s_cmd, "SPEAKER ", 8) == 0) {
                    int idx = atoi(s_cmd + 8);
                    app_select_speaker(idx);
                    Serial.printf("[DBG] Selected speaker %d\n", idx);
                } else if (strncasecmp(s_cmd, "PRESET ", 7) == 0) {
                    int idx = atoi(s_cmd + 7);
                    app_select_preset(idx, true);
                    Serial.printf("[DBG] Selected preset %d\n", idx);
                } else if (strncasecmp(s_cmd, "VOL ", 4) == 0) {
                    int v = atoi(s_cmd + 4);
                    app_set_volume(v);
                    Serial.printf("[DBG] Set volume=%d\n", v);
                } else if (strncasecmp(s_cmd, "SWIPE ", 6) == 0) {
                    char d = s_cmd[6];
                    int dir = 0;
                    if (d == 'L' || d == 'l') dir = BSP_SWIPE_LEFT;
                    else if (d == 'R' || d == 'r') dir = BSP_SWIPE_RIGHT;
                    else if (d == 'U' || d == 'u') dir = BSP_SWIPE_UP;
                    else if (d == 'D' || d == 'd') dir = BSP_SWIPE_DOWN;
                    knob_ui_on_swipe(dir);
                    Serial.printf("[DBG] Swipe dir=%d (%c)\n", dir, d);
                } else if (strcasecmp(s_cmd, "FPS") == 0) {
                    Serial.printf("[DBG] Display Flush FPS=%u (frame_us=%u)\n",
                                  (unsigned)bsp_get_fps(), (unsigned)bsp_get_frame_us());
                } else if (strcasecmp(s_cmd, "SCAN") == 0) {
                    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        g_app.speaker_scan_requested = true;
                        xSemaphoreGive(g_app.mutex);
                    }
                    Serial.println("[DBG] Requested speaker scan");
                } else if (strcasecmp(s_cmd, "BATTERY") == 0 || strcasecmp(s_cmd, "BATT") == 0) {
                    BatteryTelemetry bt = battery_monitor_snapshot();
                    Serial.printf("[BATT] state=%s (%s) pct=%d%% mv=%d raw_mv=%d raw_adc=%d slope=%d mV/min present=%d usb=%d usb_host=%d chg=%d full=%d low=%d crit=%d\n",
                                  battery_state_slug(bt.state), battery_state_label(bt.state),
                                  bt.percent, bt.voltage_mv, bt.raw_mv, bt.raw_adc, bt.slope_mv_per_min,
                                  (int)bt.battery_present, (int)bt.usb_present, (int)bt.usb_host,
                                  (int)bt.charging, (int)bt.charged, (int)bt.low_battery, (int)bt.critical_battery);
                } else if (strncasecmp(s_cmd, "TXPWR ", 6) == 0) {
                    int pwr = atoi(s_cmd + 6);
                    if (pwr < 8) pwr = 8;
                    if (pwr > 84) pwr = 84;
                    esp_err_t e = esp_wifi_set_max_tx_power((int8_t)pwr);
                    int8_t actual = 0;
                    esp_wifi_get_max_tx_power(&actual);
                    Serial.printf("[DBG] Set TX power=%d -> actual=%d (%.2f dBm) err=%d\n",
                                  pwr, (int)actual, (float)actual * 0.25f, (int)e);
                } else if (strncasecmp(s_cmd, "PROTO ", 6) == 0) {
                    const char *arg = s_cmd + 6;
                    uint8_t mask = (strcasecmp(arg, "bg") == 0)
                                       ? (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G)
                                       : (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
                    esp_err_t e = esp_wifi_set_protocol(WIFI_IF_STA, mask);
                    Serial.printf("[DBG] Set Wi-Fi protocol mask=0x%02x err=%d\n", (unsigned)mask, (int)e);
                } else if (strncasecmp(s_cmd, "BSSID ", 6) == 0) {
                    unsigned int b[6] = {0};
                    int ch = 0;
                    int parsed = sscanf(s_cmd + 6, "%x:%x:%x:%x:%x:%x %d",
                                        &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &ch);
                    if (parsed >= 6) {
                        uint8_t target_bssid[6] = {
                            (uint8_t)b[0], (uint8_t)b[1], (uint8_t)b[2],
                            (uint8_t)b[3], (uint8_t)b[4], (uint8_t)b[5]
                        };
                        char cur_ssid[36] = {0}, cur_pass[64] = {0};
                        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                            strncpy(cur_ssid, g_app.wifi_ssid, sizeof(cur_ssid) - 1);
                            strncpy(cur_pass, g_app.wifi_pass, sizeof(cur_pass) - 1);
                            xSemaphoreGive(g_app.mutex);
                        }
                        Serial.printf("[DBG] Reconnecting '%s' pinned to BSSID %02X:%02X:%02X:%02X:%02X:%02X ch=%d\n",
                                      cur_ssid, b[0], b[1], b[2], b[3], b[4], b[5], ch);
                        WiFi.disconnect(false);
                        delay(60);
                        WiFi.begin(cur_ssid, cur_pass, ch, target_bssid, true);
                    }
                } else if (strcasecmp(s_cmd, "REBOOT") == 0) {
                    app_state_flush_prefs();
                    Serial.println("[DBG] Rebooting...");
                    delay(100);
                    ESP.restart();
                } else if (strcasecmp(s_cmd, "STATUS") == 0) {
                    const char *spk_name = (g_app.current_speaker >= 0 && g_app.current_speaker < g_app.speaker_count)
                                           ? g_app.speakers[g_app.current_speaker].name : "?";
                    Serial.printf("[DBG] Tab=%d WhiteCap=%d FPS=%u BufMs=%u Mp3B=%u Underruns=%u RSSI=%d Ch=%u Speed=%uMbps BSSID=%s Preset=%d Speaker=%d ('%s') Vol=%d RAOP=%d AP2=%d Pkts=%u NTP=%u Retr=%u Status='%s' FreeHeap=%u FreePSRAM=%u\n",
                                  knob_ui_get_tab(), (int)g_app.whitecap_active, (unsigned)bsp_get_fps(),
                                  (unsigned)radio_get_buffered_ms(), (unsigned)g_app.mp3_buf_bytes, (unsigned)g_app.stream_underruns,
                                  (int)g_app.wifi_rssi, (unsigned)g_app.wifi_channel, (unsigned)g_app.wifi_speed_mbps,
                                  WiFi.BSSIDstr().c_str(),
                                  g_app.current_preset, g_app.current_speaker, spk_name,
                                  g_app.volume, (int)g_app.raop_connected, (int)g_app.raop_is_ap2,
                                  (unsigned)g_app.raop_pkts_sent, (unsigned)g_app.raop_ntp_replies,
                                  (unsigned)g_app.raop_retransmits,
                                  g_app.status_text,
                                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
                    BatteryTelemetry bt = battery_monitor_snapshot();
                    NetworkTxSnapshot tx = network_tx_snapshot();
                    int8_t tx_qdbm = 0;
                    esp_wifi_get_max_tx_power(&tx_qdbm);
                    Serial.printf("[DBG] Build=%s Uptime=%lu Reset=%d Sonos=%d Batt=%d%% (%dmV, %s, usb_host=%d) TxPwr=%.1fdBm TxSub=%lu TxAcc=%lu TxRej=%lu TxErr=%ld\n",
                                  AIRPLAYKNOB_BUILD_ID, (unsigned long)millis(), (int)esp_reset_reason(),
                                  (int)g_app.sonos_native, bt.percent, bt.voltage_mv,
                                  battery_state_slug(bt.state), (int)bt.usb_host,
                                  (float)tx_qdbm * 0.25f,
                                  (unsigned long)tx.submitted, (unsigned long)tx.accepted,
                                  (unsigned long)tx.rejected, (long)tx.last_error);
                    if (bsp_lvgl_lock(5)) {
                        lv_mem_monitor_t mem = {};
                        lv_mem_monitor(&mem);
                        bsp_lvgl_unlock();
                        Serial.printf("[DBG] UI heap free=%u largest=%u max_used=%u\n",
                                      (unsigned)mem.free_size, (unsigned)mem.free_biggest_size, (unsigned)mem.max_used);
                    }
                    for (const char *task_name : {"LVGL", "radio_http", "raop_tx", "art_fetch", "net_mgr"}) {
                        TaskHandle_t task = xTaskGetHandle(task_name);
                        if (task) Serial.printf("[DBG] Stack free %s=%u\n", task_name, (unsigned)uxTaskGetStackHighWaterMark(task));
                    }
                    for (int i = 0; i < g_app.speaker_count; i++) {
                        Serial.printf("  [SPK %d] '%s' %s:%u am=%s et=%s ap2=%d\n",
                                      i, g_app.speakers[i].name, g_app.speakers[i].ip,
                                      g_app.speakers[i].port, g_app.speakers[i].am, g_app.speakers[i].et,
                                      (int)g_app.speakers[i].is_ap2);
                    }
                }
            }
        } else if (s_len < (int)sizeof(s_cmd) - 1) {
            s_cmd[s_len++] = c;
        }
    }
}

void loop() {
    int32_t delta = 0;
    portENTER_CRITICAL(&s_knob_mux);
    delta = s_knob_delta;
    s_knob_delta = 0;
    portEXIT_CRITICAL(&s_knob_mux);

    if (delta != 0) {
        knob_ui_on_rotate(delta);
    }

    poll_serial_debug();
    battery_monitor_poll();
    app_state_flush_prefs();
    knob_ui_tick();
    web_portal_poll();
    delay(2);
}

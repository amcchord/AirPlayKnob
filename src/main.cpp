#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include <Preferences.h>
#include <mdns.h>
#include <ArduinoOTA.h>
#include <sodium.h>
#include <esp_system.h>
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

static void ingest_mdns_results(mdns_result_t *results, AirPlaySpeaker *found, int *count_inout, uint32_t now_ms) {
    int count = *count_inout;
    for (mdns_result_t *r = results; r != NULL && count < MAX_SPEAKERS - 1; r = r->next) {
        char ip_str[20] = {0};
        for (mdns_ip_addr_t *a = r->addr; a != NULL; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&(a->addr.u_addr.ip4)));
                break;
            }
        }
        if (ip_str[0] == '\0' || strcmp(ip_str, "0.0.0.0") == 0) continue;

        uint16_t port = r->port ? r->port : 7000;
        const char *inst = r->instance_name ? r->instance_name : "AirPlay Speaker";
        const char *at = strchr(inst, '@');
        char sp_name[48] = {0};
        strncpy(sp_name, (at && *(at + 1)) ? (at + 1) : inst, sizeof(sp_name) - 1);

        char et[24] = {0};
        char cn[24] = {0};
        char md[24] = {0};
        char am[32] = {0};
        bool req_pin = false;

        for (size_t t = 0; t < r->txt_count; t++) {
            const char *k = r->txt[t].key;
            const char *v = r->txt[t].value ? r->txt[t].value : "";
            if (!k) continue;
            if (strcasecmp(k, "et") == 0 && v[0]) strncpy(et, v, sizeof(et) - 1);
            else if (strcasecmp(k, "cn") == 0 && v[0]) strncpy(cn, v, sizeof(cn) - 1);
            else if (strcasecmp(k, "md") == 0 && v[0]) strncpy(md, v, sizeof(md) - 1);
            else if ((strcasecmp(k, "am") == 0 || strcasecmp(k, "model") == 0) && v[0]) strncpy(am, v, sizeof(am) - 1);
            else if (strcasecmp(k, "pw") == 0 && (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0)) {
                req_pin = true;
            } else if (strcasecmp(k, "sf") == 0 || strcasecmp(k, "flags") == 0) {
                uint32_t sf = (uint32_t)strtoul(v, NULL, 0);
                if ((sf & 0x08U) != 0) {
                    req_pin = true;
                } else if ((sf & 0x200U) != 0 && !app_is_ap2_speaker_paired(ip_str)) {
                    req_pin = true;
                }
            } else if (strcasecmp(k, "ft") == 0 || strcasecmp(k, "features") == 0) {
                unsigned long long low = 0, high = 0;
                if (sscanf(v, "%llx,%llx", &low, &high) == 2 && (high & 0x11C040ULL) != 0) {
                    if (et[0] == '\0') {
                        strncpy(et, "0,4", sizeof(et) - 1);
                    } else if (strchr(et, '4') == NULL && strchr(et, '5') == NULL) {
                        strncat(et, ",4", sizeof(et) - strlen(et) - 1);
                    }
                }
            }
        }

        if (strcasestr(am, "AudioAccessory") != NULL &&
            strcasestr(sp_name, "HomePod") == NULL &&
            strcasestr(sp_name, "Home Pod") == NULL) {
            strncat(sp_name, " (HomePod)", sizeof(sp_name) - strlen(sp_name) - 1);
        }

        int existing_idx = -1;
        for (int i = 0; i < count; i++) {
            if (found[i].ip[0] != '\0' && strcmp(found[i].ip, ip_str) == 0) {
                existing_idx = i;
                break;
            }
        }
        if (existing_idx < 0) {
            // Also match pre-populated saved speaker whose IP was empty or updated via DHCP
            for (int i = 0; i < count; i++) {
                if (!found[i].is_local_dac && strcasecmp(found[i].name, sp_name) == 0 &&
                    (found[i].ip[0] == '\0' || (am[0] && strcasecmp(found[i].am, am) == 0))) {
                    existing_idx = i;
                    break;
                }
            }
        }

        if (existing_idx >= 0) {
            AirPlaySpeaker &sp = found[existing_idx];
            sp.last_seen_ms = now_ms;
            sp.port = port;
            strncpy(sp.ip, ip_str, sizeof(sp.ip) - 1);
            if (r->hostname && r->hostname[0]) {
                snprintf(sp.host, sizeof(sp.host), "%s.local", r->hostname);
            }
            if (sp_name[0] && strcmp(sp_name, "AirPlay Speaker") != 0) {
                strncpy(sp.name, sp_name, sizeof(sp.name) - 1);
            }
            if (r->txt_count > 0) {
                if (et[0]) strncpy(sp.et, et, sizeof(sp.et) - 1);
                if (cn[0]) strncpy(sp.cn, cn, sizeof(sp.cn) - 1);
                if (md[0]) strncpy(sp.md, md, sizeof(sp.md) - 1);
                if (am[0]) strncpy(sp.am, am, sizeof(sp.am) - 1);
            }
            if (req_pin) sp.requires_pin = true;
            sp.is_ap2 = speaker_should_use_ap2(sp.am, sp.et);
            continue;
        }

        AirPlaySpeaker &sp = found[count];
        memset(&sp, 0, sizeof(sp));
        sp.port = port;
        sp.last_seen_ms = now_ms;
        strncpy(sp.ip, ip_str, sizeof(sp.ip) - 1);
        if (r->hostname) {
            snprintf(sp.host, sizeof(sp.host), "%s.local", r->hostname);
        }
        strncpy(sp.name, sp_name, sizeof(sp.name) - 1);
        strncpy(sp.et, et[0] ? et : "0,1", sizeof(sp.et) - 1);
        strncpy(sp.cn, cn[0] ? cn : "0,1", sizeof(sp.cn) - 1);
        strncpy(sp.md, md, sizeof(sp.md) - 1);
        strncpy(sp.am, am, sizeof(sp.am) - 1);
        sp.requires_pin = req_pin;
        sp.is_ap2 = speaker_should_use_ap2(sp.am, sp.et);

        count++;
    }
    *count_inout = count;
}

static void discover_airplay_speakers(bool manual_scan) {
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_app.speaker_scanning = true;
        g_app.speaker_scan_count++;
        g_app.speaker_scan_last_ms = millis();
        xSemaphoreGive(g_app.mutex);
    } else return;

    uint32_t now_ms = millis();
    mdns_result_t *raop_results = NULL;
    mdns_result_t *ap_results = NULL;
    mdns_query_ptr("_raop", "_tcp", manual_scan ? 1400 : 1000, 48, &raop_results);
    if (manual_scan || !raop_results) {
        mdns_query_ptr("_airplay", "_tcp", 600, 48, &ap_results);
    }

    AirPlaySpeaker *found = (AirPlaySpeaker *)heap_caps_calloc(MAX_SPEAKERS, sizeof(AirPlaySpeaker), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!found) {
        if (raop_results) mdns_query_results_free(raop_results);
        if (ap_results) mdns_query_results_free(ap_results);
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_app.speaker_scanning = false;
            xSemaphoreGive(g_app.mutex);
        }
        return;
    }
    int count = 0;

    // Seed with existing speakers so speaker list order & indices stay stable, and NEVER drop
    // previously discovered speakers or the active/saved speaker if they miss a multicast mDNS query!
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < g_app.speaker_count && count < MAX_SPEAKERS - 1; i++) {
            const AirPlaySpeaker &old_sp = g_app.speakers[i];
            if (old_sp.is_local_dac) continue;
            bool is_active_or_saved =
                (i == g_app.current_speaker) ||
                (s_saved_speaker_ip[0] != '\0' && strcmp(old_sp.ip, s_saved_speaker_ip) == 0) ||
                (s_saved_speaker_name[0] != '\0' && strcasecmp(old_sp.name, s_saved_speaker_name) == 0);
            if (is_active_or_saved || old_sp.ip[0] != '\0') {
                found[count++] = old_sp;
            }
        }
        xSemaphoreGive(g_app.mutex);
    }

    ingest_mdns_results(raop_results, found, &count, now_ms);
    if (ap_results) {
        ingest_mdns_results(ap_results, found, &count, now_ms);
    }

    if (raop_results) mdns_query_results_free(raop_results);
    if (ap_results) mdns_query_results_free(ap_results);

    bool need_save_resolved_spk = false;
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Always append Local 3.5mm Audio Jack as the final option
        AirPlaySpeaker &local_sp = found[count++];
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

        memcpy(g_app.speakers, found, sizeof(AirPlaySpeaker) * count);
        g_app.speaker_count = count;

        int new_sel = -1;
        // 1. Preserve current active AirPlay speaker by exact IP first, then exact name
        if (s_initial_speaker_autoselected && (!prev_was_local_dac || s_user_selected_local_dac)) {
            if (!prev_was_local_dac && prev_selected_ip[0] != '\0') {
                for (int i = 0; i < count; i++) {
                    if (!g_app.speakers[i].is_local_dac && strcmp(g_app.speakers[i].ip, prev_selected_ip) == 0) {
                        new_sel = i;
                        break;
                    }
                }
            }
            if (new_sel < 0 && prev_selected_name[0] != '\0') {
                for (int i = 0; i < count; i++) {
                    if (strcasecmp(g_app.speakers[i].name, prev_selected_name) == 0) {
                        new_sel = i;
                        break;
                    }
                }
            }
        }
        // 2. Match saved speaker from NVS (exact IP -> exact name -> substring fallback)
        if (new_sel < 0 && !s_user_selected_local_dac) {
            if (s_saved_speaker_ip[0] != '\0') {
                for (int i = 0; i < count; i++) {
                    if (!g_app.speakers[i].is_local_dac && strcmp(g_app.speakers[i].ip, s_saved_speaker_ip) == 0) {
                        new_sel = i;
                        s_initial_speaker_autoselected = true;
                        break;
                    }
                }
            }
            if (new_sel < 0 && s_saved_speaker_name[0] != '\0') {
                for (int i = 0; i < count; i++) {
                    if (strcasecmp(g_app.speakers[i].name, s_saved_speaker_name) == 0) {
                        new_sel = i;
                        s_initial_speaker_autoselected = true;
                        break;
                    }
                }
            }
            if (new_sel < 0 && s_saved_speaker_name[0] != '\0') {
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
        if (g_app.preview_timestamp_ms == 0) {
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
    if (need_save_resolved_spk) {
        app_state_save_playback();
    }
    heap_caps_free(found);
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
        g_app.wifi_rssi = rssi;
        g_app.wifi_channel = chan;
        g_app.wifi_speed_mbps = speed_mbps;
        strncpy(g_app.wifi_phy_mode, phy_str, sizeof(g_app.wifi_phy_mode) - 1);
        xSemaphoreGive(g_app.mutex);
    }
}

static void connect_best_wifi_ap(const char *ssid, const char *pass) {
    if (!ssid || ssid[0] == '\0') return;
    Serial.printf("[WIFI] Scanning for best BSSID on SSID '%s'...\n", ssid);
    WiFi.scanDelete();

    // Full all-channel scan (250ms/ch) so we see all candidate APs and avoid overlapping non-standard channels (like Ch 2 / Ch 10)
    int n = WiFi.scanNetworks(false, false, false, 250);
    int best_idx = -1;
    int best_score = -1000;
    for (int i = 0; i < n; i++) {
        if (strcasecmp(WiFi.SSID(i).c_str(), ssid) != 0) continue;
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
        Serial.printf("[WIFI]   Candidate AP [%d]: BSSID=%s Ch=%ld RSSI=%ld dBm Score=%d\n",
                      i, WiFi.BSSIDstr(i).c_str(), (long)ch_i, (long)rssi_i, score);
        if (rssi_i >= -75 && score > best_score) {
            best_score = score;
            best_idx = i;
        }
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
}

static void network_manager_task(void *arg) {
    (void)arg;
    bool mdns_started = false;

    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_STA);
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

    for (;;) {
        wl_status_t st = WiFi.status();
        bool connected = (st == WL_CONNECTED);

        bool do_reconnect = false;
        bool do_wifi_scan = false;
        bool do_spk_scan = false;
        bool just_connected = false;
        bool modal_open = false;
        bool defer_discovery = false;

        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            bool was_connected = g_app.wifi_connected;
            g_app.wifi_connected = connected;
            if (connected) {
                IPAddress ip = WiFi.localIP();
                snprintf(g_app.wifi_ip, sizeof(g_app.wifi_ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
                g_app.wifi_rssi = WiFi.RSSI();
                if (!was_connected) {
                    g_app.wifi_connected_since_ms = millis();
                    just_connected = true;
                }
            } else {
                g_app.wifi_connected_since_ms = 0;
            }
            do_reconnect = g_app.wifi_reconnect_requested;
            g_app.wifi_reconnect_requested = false;

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
            bool native_only = g_app.sonos_native && !g_app.also_local_dac && !g_app.whitecap_active;
            bool defer = connected && discovery_must_wait(g_app.playing, native_only, needs_ip,
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
            }
            xSemaphoreGive(g_app.mutex);
        } else {
            vTaskDelay(pdMS_TO_TICKS(150));
            continue; // No snapshot means no permission to start network work.
        }

        if (connected) {
            last_wifi_attempt_ms = millis();
            update_wifi_negotiated_link_info();
            if (just_connected) {
                esp_wifi_set_ps(WIFI_PS_NONE);
                if (fb_idx == 0) {
                    s_save_wifi_dirty = true;
                }
                last_mdns_scan_ms = 0;
                Serial.printf("[WIFI] Link up: %s (%d Mbps, %s, Ch %u, RSSI %ld dBm, IP %s)\n",
                              ssid, g_app.wifi_speed_mbps, g_app.wifi_phy_mode,
                              g_app.wifi_channel, (long)g_app.wifi_rssi, g_app.wifi_ip);
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
        // and thereafter ONLY scan when safe, paused, or if
        // the selected speaker still needs its IP resolved. Never blast 48-device multicast mDNS
        // queries while streaming audio on the Now Playing screen!
        uint32_t scan_interval_ms = 30000;
        bool active_spk_needs_ip =
            (g_app.current_speaker >= 0 && g_app.current_speaker < g_app.speaker_count &&
             !g_app.speakers[g_app.current_speaker].is_local_dac &&
             g_app.speakers[g_app.current_speaker].ip[0] == '\0');
        bool discovery_needed = active_spk_needs_ip || !g_app.playing;
        bool initial_scan_due = (last_mdns_scan_ms == 0);
        bool periodic_scan_due = (last_mdns_scan_ms > 0) &&
                                 (millis() - last_mdns_scan_ms >= scan_interval_ms) &&
                                 discovery_needed &&
                                 (g_app.status != STREAM_CONNECTING_AIRPLAY);
        if (connected && !defer_discovery && !g_app.ota_in_progress && !g_app.pin_prompt_active &&
            (do_spk_scan || initial_scan_due || periodic_scan_due)) {
            discover_airplay_speakers(do_spk_scan);
            last_mdns_scan_ms = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

extern "C" esp_err_t __real_esp_wifi_init(const wifi_init_config_t *config);
extern "C" esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t *config) {
    wifi_init_config_t cfg = *config;
    cfg.static_rx_buf_num = 10;
    cfg.dynamic_rx_buf_num = 32;
    cfg.rx_ba_win = 4;            // Must be < static_rx_buf_num so 802.11n AMPDU RX BA reorder never wedges RX DMA
    cfg.tx_buf_type = 1;          // DYNAMIC_TX_BUFFER (required when CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=1)
    cfg.static_tx_buf_num = 0;
    // Preserve working memory for the UI and connection recovery under load.
    cfg.dynamic_tx_buf_num = 16;
    cfg.cache_tx_buf_num = 8;
    return __real_esp_wifi_init(&cfg);
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

    Serial.printf("[BOOT] Free Internal Heap: %u bytes, Free PSRAM: %u bytes\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

    // Initialize Wi-Fi STA early so Wi-Fi MAC DMA buffers get contiguous internal SRAM
    WiFi.useStaticBuffers(false);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);

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

static void dump_serial_screenshot(void) {
    const uint16_t *fb = bsp_get_screen_fb();
    if (!fb) {
        Serial.println("[DBG] No screen framebuffer");
        return;
    }
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
                    Serial.printf("[DBG] Build=%s Uptime=%lu Reset=%d Sonos=%d\n", AIRPLAYKNOB_BUILD_ID,
                                  (unsigned long)millis(), (int)esp_reset_reason(), (int)g_app.sonos_native);
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
    app_state_flush_prefs();
    knob_ui_tick();
    web_portal_poll();
    delay(2);
}

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <mdns.h>
#include <ArduinoOTA.h>

#include "app_state.h"
#include "bsp/lcd_bsp.h"
#include "bsp/cst816.h"
#include "bsp/lcd_bl_pwm_bsp.h"
#include "bsp/lcd_config.h"
#include "bsp/bidi_switch_knob.h"
#include "radio_streamer.h"
#include "artwork_fetcher.h"
#include "knob_ui.h"
#include "web_portal.h"

AppState g_app = {};
static Preferences s_prefs;
static volatile int32_t s_knob_delta = 0;
static portMUX_TYPE s_knob_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_saved_speaker_name[48] = {0};

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
    s_prefs.begin("airplayknob", false);
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_prefs.putInt("preset_cnt", g_app.preset_count);
        s_prefs.putBytes("presets", g_app.presets, sizeof(StationPreset) * g_app.preset_count);
        xSemaphoreGive(g_app.mutex);
    }
    s_prefs.end();
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
    app_state_save_presets();
}

void app_state_save_playback(void) {
    s_prefs.begin("airplayknob", false);
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_prefs.putInt("cur_preset", g_app.current_preset);
        s_prefs.putInt("volume", g_app.volume);
        if (g_app.current_speaker >= 0 && g_app.current_speaker < g_app.speaker_count) {
            s_prefs.putString("spk_name", g_app.speakers[g_app.current_speaker].name);
        }
        xSemaphoreGive(g_app.mutex);
    }
    s_prefs.end();
}

void app_state_save_wifi(const char *ssid, const char *pass) {
    s_prefs.begin("airplayknob", false);
    s_prefs.putString("wifi_ssid", ssid);
    s_prefs.putString("wifi_pass", pass);
    s_prefs.end();

    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        strncpy(g_app.wifi_ssid, ssid, sizeof(g_app.wifi_ssid) - 1);
        strncpy(g_app.wifi_pass, pass, sizeof(g_app.wifi_pass) - 1);
        g_app.wifi_reconnect_requested = true;
        xSemaphoreGive(g_app.mutex);
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
}

void app_toggle_play(void) {
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_app.playing = !g_app.playing;
        g_app.stream_reconfig_needed = true;
        xSemaphoreGive(g_app.mutex);
    }
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
    if (g_app.volume > 20) {
        g_app.volume = 18; // Always start at a safe low volume on boot
    }
    g_app.playing = true;
    g_app.knob_mode = KNOB_MODE_VOLUME;
    g_app.also_local_dac = true;

    String w_ssid = s_prefs.getString("wifi_ssid", DEFAULT_WIFI_SSID);
    String w_pass = s_prefs.getString("wifi_pass", DEFAULT_WIFI_PASS);
    String spk_n  = s_prefs.getString("spk_name", "");
    s_prefs.end();

    strncpy(g_app.wifi_ssid, w_ssid.c_str(), sizeof(g_app.wifi_ssid) - 1);
    strncpy(g_app.wifi_pass, w_pass.c_str(), sizeof(g_app.wifi_pass) - 1);
    strncpy(s_saved_speaker_name, spk_n.c_str(), sizeof(s_saved_speaker_name) - 1);

    // Default speaker slot 0: Local 3.5mm Audio Jack (until AirPlay speakers are discovered)
    g_app.speaker_count = 1;
    strncpy(g_app.speakers[0].name, "Local 3.5mm Jack", sizeof(g_app.speakers[0].name) - 1);
    g_app.speakers[0].is_local_dac = true;
    g_app.current_speaker = 0;

    strncpy(g_app.status_text, "Joining Wi-Fi...", sizeof(g_app.status_text));
    strncpy(g_app.track_title, "SomaFM Internet Radio", sizeof(g_app.track_title));
}

static void discover_airplay_speakers(void) {
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_app.speaker_scanning = true;
        xSemaphoreGive(g_app.mutex);
    }

    Serial.println("[MDNS] Querying _raop._tcp.local for AirPlay speakers...");
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr("_raop", "_tcp", 3000, 32, &results);
    if (err == ESP_OK && results == NULL) {
        // Retry once if multicast response arrived late right after Wi-Fi association
        vTaskDelay(pdMS_TO_TICKS(300));
        err = mdns_query_ptr("_raop", "_tcp", 3000, 32, &results);
    }
    if (err != ESP_OK) {
        Serial.printf("[MDNS] Query failed: %d\n", err);
    }

    AirPlaySpeaker *found = (AirPlaySpeaker *)heap_caps_calloc(MAX_SPEAKERS, sizeof(AirPlaySpeaker), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!found) {
        if (results) mdns_query_results_free(results);
        return;
    }
    int count = 0;

    for (mdns_result_t *r = results; r != NULL && count < MAX_SPEAKERS - 1; r = r->next) {
        // Find IPv4 address
        char ip_str[20] = {0};
        for (mdns_ip_addr_t *a = r->addr; a != NULL; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&(a->addr.u_addr.ip4)));
                break;
            }
        }
        if (ip_str[0] == '\0' || strcmp(ip_str, "0.0.0.0") == 0) continue;

        // Deduplicate by IP + port
        bool dup = false;
        for (int i = 0; i < count; i++) {
            if (strcmp(found[i].ip, ip_str) == 0 && found[i].port == r->port) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        AirPlaySpeaker &sp = found[count];
        sp.port = r->port ? r->port : 7000;
        strncpy(sp.ip, ip_str, sizeof(sp.ip) - 1);
        if (r->hostname) {
            snprintf(sp.host, sizeof(sp.host), "%s.local", r->hostname);
        }

        // Instance name is usually "MAC@Friendly Name"
        const char *inst = r->instance_name ? r->instance_name : "AirPlay Speaker";
        const char *at = strchr(inst, '@');
        strncpy(sp.name, (at && *(at + 1)) ? (at + 1) : inst, sizeof(sp.name) - 1);

        // Parse TXT records (et, cn, md, am)
        strncpy(sp.et, "0,1", sizeof(sp.et) - 1);
        strncpy(sp.cn, "0,1", sizeof(sp.cn) - 1);
        for (size_t t = 0; t < r->txt_count; t++) {
            const char *k = r->txt[t].key;
            const char *v = r->txt[t].value ? r->txt[t].value : "";
            if (!k) continue;
            if (strcasecmp(k, "et") == 0) strncpy(sp.et, v, sizeof(sp.et) - 1);
            else if (strcasecmp(k, "cn") == 0) strncpy(sp.cn, v, sizeof(sp.cn) - 1);
            else if (strcasecmp(k, "md") == 0) strncpy(sp.md, v, sizeof(sp.md) - 1);
            else if (strcasecmp(k, "am") == 0) strncpy(sp.am, v, sizeof(sp.am) - 1);
        }

        // Disambiguate HomePod vs Sonos when rooms have both
        if (strcasestr(sp.am, "AudioAccessory") != NULL && strcasestr(sp.name, "HomePod") == NULL && strcasestr(sp.name, "Home Pod") == NULL) {
            strncat(sp.name, " (HomePod)", sizeof(sp.name) - strlen(sp.name) - 1);
        }

        Serial.printf("[MDNS] Found AirPlay speaker: '%s' at %s:%u (am=%s et=%s)\n",
                      sp.name, sp.ip, sp.port, sp.am, sp.et);
        count++;
    }

    if (results) {
        mdns_query_results_free(results);
    }

    // Always append Local 3.5mm Audio Jack as the final option
    AirPlaySpeaker &local_sp = found[count++];
    strncpy(local_sp.name, "Local 3.5mm Jack", sizeof(local_sp.name) - 1);
    local_sp.is_local_dac = true;

    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        char prev_selected_name[48] = {0};
        if (g_app.current_speaker >= 0 && g_app.current_speaker < g_app.speaker_count) {
            strncpy(prev_selected_name, g_app.speakers[g_app.current_speaker].name, sizeof(prev_selected_name) - 1);
        }

        memcpy(g_app.speakers, found, sizeof(AirPlaySpeaker) * count);
        g_app.speaker_count = count;

        int new_sel = -1;
        // Try matching current active speaker name first
        if (prev_selected_name[0] && strcmp(prev_selected_name, "Local 3.5mm Jack") != 0) {
            for (int i = 0; i < count; i++) {
                if (strcasecmp(g_app.speakers[i].name, prev_selected_name) == 0) {
                    new_sel = i;
                    break;
                }
            }
        }
        // Next try saved speaker name from NVS (only if it is Tree House or Local Jack)
        if (new_sel < 0 && s_saved_speaker_name[0] &&
            (strcasestr(s_saved_speaker_name, "Tree House") != NULL || strcasestr(s_saved_speaker_name, "Local") != NULL)) {
            if (strcasecmp(s_saved_speaker_name, "Tree House") == 0) {
                strncpy(s_saved_speaker_name, "Tree House (HomePod)", sizeof(s_saved_speaker_name) - 1);
            }
            for (int i = 0; i < count; i++) {
                if (strcasecmp(g_app.speakers[i].name, s_saved_speaker_name) == 0) {
                    new_sel = i;
                    break;
                }
            }
        }
        // Default exclusively to Tree House HomePod (or Local 3.5mm Jack if not found)
        if (new_sel < 0) {
            for (int i = 0; i < count; i++) {
                if (strcasestr(g_app.speakers[i].name, "Tree House") != NULL &&
                    strcasestr(g_app.speakers[i].am, "AudioAccessory") != NULL) {
                    new_sel = i;
                    break;
                }
            }
        }
        if (new_sel < 0) {
            new_sel = count - 1; // Local 3.5mm Jack
        }
        g_app.current_speaker = new_sel;
        g_app.preview_speaker = new_sel;
        g_app.speaker_scanning = false;
        xSemaphoreGive(g_app.mutex);
    }
    heap_caps_free(found);
}

static void network_manager_task(void *arg) {
    (void)arg;
    bool mdns_started = false;

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false); // Disable Wi-Fi power save for low-jitter audio & AirPlay streaming

    char ssid[36] = {0}, pass[64] = {0};
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        strncpy(ssid, g_app.wifi_ssid, sizeof(ssid) - 1);
        strncpy(pass, g_app.wifi_pass, sizeof(pass) - 1);
        xSemaphoreGive(g_app.mutex);
    }

    if (ssid[0] != '\0') {
        Serial.printf("[WIFI] Connecting to SSID: %s\n", ssid);
        WiFi.begin(ssid, pass);
    } else {
        Serial.println("[WIFI] No saved Wi-Fi SSID. Swipe Down on touchscreen -> Wi-Fi Scan to connect.");
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            strncpy(g_app.status_text, "Swipe Down -> Wi-Fi Scan", sizeof(g_app.status_text) - 1);
            xSemaphoreGive(g_app.mutex);
        }
    }

    uint32_t last_mdns_scan_ms = 0;

    for (;;) {
        wl_status_t st = WiFi.status();
        bool connected = (st == WL_CONNECTED);

        bool do_reconnect = false;
        bool do_wifi_scan = false;
        bool do_spk_scan = false;

        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            bool was_connected = g_app.wifi_connected;
            g_app.wifi_connected = connected;
            if (connected) {
                IPAddress ip = WiFi.localIP();
                snprintf(g_app.wifi_ip, sizeof(g_app.wifi_ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
                g_app.wifi_rssi = WiFi.RSSI();
                if (!was_connected) {
                    g_app.speaker_scan_requested = true;
                }
            }
            do_reconnect = g_app.wifi_reconnect_requested;
            g_app.wifi_reconnect_requested = false;

            do_wifi_scan = g_app.wifi_scan_requested;
            g_app.wifi_scan_requested = false;

            do_spk_scan = g_app.speaker_scan_requested;
            g_app.speaker_scan_requested = false;

            if (do_reconnect) {
                strncpy(ssid, g_app.wifi_ssid, sizeof(ssid) - 1);
                strncpy(pass, g_app.wifi_pass, sizeof(pass) - 1);
            }
            xSemaphoreGive(g_app.mutex);
        }

        if (do_reconnect) {
            Serial.printf("[WIFI] Reconnecting to %s...\n", ssid);
            WiFi.disconnect();
            vTaskDelay(pdMS_TO_TICKS(200));
            WiFi.begin(ssid, pass);
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

        if (connected && (do_spk_scan || (last_mdns_scan_ms == 0) ||
                          (g_app.speaker_count <= 1 && (millis() - last_mdns_scan_ms >= 8000)))) {
            last_mdns_scan_ms = millis();
            discover_airplay_speakers();
        }

        if (do_wifi_scan) {
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                g_app.wifi_scanning = true;
                xSemaphoreGive(g_app.mutex);
            }
            int n = WiFi.scanNetworks(false, false);
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                int cnt = 0;
                for (int i = 0; i < n && cnt < MAX_WIFI_SCAN; i++) {
                    String s = WiFi.SSID(i);
                    if (s.length() == 0) continue;
                    strncpy(g_app.wifi_scan[cnt].ssid, s.c_str(), sizeof(g_app.wifi_scan[cnt].ssid) - 1);
                    g_app.wifi_scan[cnt].rssi = WiFi.RSSI(i);
                    g_app.wifi_scan[cnt].secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
                    cnt++;
                }
                g_app.wifi_scan_count = cnt;
                g_app.wifi_scanning = false;
                xSemaphoreGive(g_app.mutex);
            }
            WiFi.scanDelete();
        }

        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n==========================================");
    Serial.println("  AirPlayKnob - SomaFM to AirPlay Streamer");
    Serial.println("==========================================");

    app_state_init();

    Serial.printf("[BOOT] Free Internal Heap: %u bytes, Free PSRAM: %u bytes\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

    // Initialize Wi-Fi STA early so Wi-Fi MAC DMA buffers get contiguous internal SRAM
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);

    // Initialize Touch + DRV2605 Haptics + 360x360 QSPI Display + Backlight
    Touch_Init();
    lcd_lvgl_Init();
    lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);

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

    // Build LVGL Circular UI
    knob_ui_init();

    // Start Wi-Fi + mDNS discovery task, Web Portal, and Radio/AirPlay Streamer tasks
    xTaskCreatePinnedToCore(network_manager_task, "net_mgr", 12288, NULL, 3, NULL, 0);
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
                } else if (strcasecmp(s_cmd, "STATUS") == 0) {
                    const char *spk_name = (g_app.current_speaker >= 0 && g_app.current_speaker < g_app.speaker_count)
                                           ? g_app.speakers[g_app.current_speaker].name : "?";
                    Serial.printf("[DBG] Tab=%d WhiteCap=%d FPS=%u BufMs=%u Preset=%d Speaker=%d ('%s') Vol=%d Status='%s' FreeHeap=%u FreePSRAM=%u\n",
                                  knob_ui_get_tab(), (int)g_app.whitecap_active, (unsigned)bsp_get_fps(),
                                  (unsigned)radio_get_buffered_ms(),
                                  g_app.current_preset, g_app.current_speaker, spk_name,
                                  g_app.volume, g_app.status_text,
                                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
                    for (int i = 0; i < g_app.speaker_count; i++) {
                        Serial.printf("  [SPK %d] '%s' %s:%u am=%s et=%s\n",
                                      i, g_app.speakers[i].name, g_app.speakers[i].ip,
                                      g_app.speakers[i].port, g_app.speakers[i].am, g_app.speakers[i].et);
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
    knob_ui_tick();
    web_portal_poll();
    delay(2);
}

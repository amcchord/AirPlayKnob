#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

#include "radio_streamer.h"
#include "app_state.h"
#include "raop_AnySpeaker.h"
#include "bsp/local_dac.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

// 256KB stereo PCM ringbuffer in PSRAM (65536 stereo frames = ~1.48 seconds at 44.1kHz)
#define PCM_RING_FRAMES 65536
static int16_t *s_pcm_ring = NULL;
static volatile uint32_t s_ring_write = 0;
static volatile uint32_t s_ring_read = 0;
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t ring_available_frames() {
    portENTER_CRITICAL(&s_ring_mux);
    uint32_t avail = (s_ring_write - s_ring_read) & (PCM_RING_FRAMES - 1);
    portEXIT_CRITICAL(&s_ring_mux);
    return avail;
}

static uint32_t ring_free_frames() {
    return (PCM_RING_FRAMES - 1) - ring_available_frames();
}

static void ring_clear() {
    portENTER_CRITICAL(&s_ring_mux);
    s_ring_write = 0;
    s_ring_read = 0;
    portEXIT_CRITICAL(&s_ring_mux);
}

static void ring_push_frames(const int16_t *stereo_frames, uint32_t count) {
    if (!s_pcm_ring) return;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t w = s_ring_write;
        uint32_t next_w = (w + 1) & (PCM_RING_FRAMES - 1);
        if (next_w == s_ring_read) {
            break; // full
        }
        s_pcm_ring[w * 2]     = stereo_frames[i * 2];
        s_pcm_ring[w * 2 + 1] = stereo_frames[i * 2 + 1];
        s_ring_write = next_w;
    }
}

static bool ring_pop_352(int16_t *out_stereo_352) {
    if (!s_pcm_ring || ring_available_frames() < RAOP_CHUNK_FRAMES) {
        return false;
    }
    uint32_t r = s_ring_read;
    for (int i = 0; i < RAOP_CHUNK_FRAMES; i++) {
        out_stereo_352[i * 2]     = s_pcm_ring[r * 2];
        out_stereo_352[i * 2 + 1] = s_pcm_ring[r * 2 + 1];
        r = (r + 1) & (PCM_RING_FRAMES - 1);
    }
    s_ring_read = r;
    return true;
}

static void update_vu_meters(const int16_t *stereo_352) {
    uint8_t bars[12] = {0};
    int32_t max_abs = 0;
    int samples_per_bar = RAOP_CHUNK_FRAMES / 12;

    for (int b = 0; b < 12; b++) {
        int32_t sum = 0;
        int start = b * samples_per_bar;
        for (int i = 0; i < samples_per_bar; i++) {
            int32_t l = abs((int32_t)stereo_352[(start + i) * 2]);
            int32_t r = abs((int32_t)stereo_352[(start + i) * 2 + 1]);
            int32_t m = (l + r) >> 1;
            sum += m;
            if (m > max_abs) max_abs = m;
        }
        int32_t avg = sum / samples_per_bar;
        int pct = (avg * 100) / 14000;
        if (pct > 100) pct = 100;
        bars[b] = (uint8_t)pct;
    }
    int peak_pct = (max_abs * 100) / 24000;
    if (peak_pct > 100) peak_pct = 100;

    int8_t wl[128], wr[128];
    for (int i = 0; i < 128; i++) {
        int idx = (i * RAOP_CHUNK_FRAMES) / 128;
        int32_t sl = stereo_352[idx * 2] / 220;
        int32_t sr = stereo_352[idx * 2 + 1] / 220;
        if (sl < -127) sl = -127;
        if (sl > 127)  sl = 127;
        if (sr < -127) sr = -127;
        if (sr > 127)  sr = 127;
        wl[i] = (int8_t)sl;
        wr[i] = (int8_t)sr;
    }

    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        for (int b = 0; b < 12; b++) {
            // Smooth decay
            if (bars[b] >= g_app.vu_bars[b]) {
                g_app.vu_bars[b] = bars[b];
            } else {
                g_app.vu_bars[b] = (uint8_t)((g_app.vu_bars[b] * 3 + bars[b]) / 4);
            }
        }
        g_app.vu_peak = (uint8_t)peak_pct;
        memcpy(g_app.wave_l, wl, 128);
        memcpy(g_app.wave_r, wr, 128);
        xSemaphoreGive(g_app.mutex);
    }
}

static void parse_icy_metadata(const char *meta_buf, int meta_len) {
    const char *p = strstr(meta_buf, "StreamTitle='");
    if (!p) return;
    p += 13;
    const char *end = strstr(p, "';");
    if (!end) end = strchr(p, '\'');
    if (!end) return;

    int len = (int)(end - p);
    if (len <= 0) return;
    if (len > 120) len = 120;

    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        memcpy(g_app.track_title, p, len);
        g_app.track_title[len] = '\0';
        xSemaphoreGive(g_app.mutex);
        Serial.printf("[ICY] Now Playing: %s\n", g_app.track_title);
    }
}

// Task 1: HTTP Icecast reader + MP3 decoder -> PSRAM ringbuffer
static void http_decoder_task(void *arg) {
    mp3dec_t *mp3d = (mp3dec_t *)heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *pcm_out = (int16_t *)heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *stereo_tmp = (int16_t *)heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *mp3_buf = (uint8_t *)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *meta_buf = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    int mirror_idx = 0;
    const char *somafm_mirrors[] = {"ice6.somafm.com", "ice2.somafm.com", "ice4.somafm.com"};

    for (;;) {
        bool want_play = false;
        bool wifi_ok = false;
        char target_url[128] = {0};
        char station_name[40] = {0};

        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            want_play = g_app.playing && !g_app.ota_in_progress;
            wifi_ok = g_app.wifi_connected;
            if (g_app.current_preset >= 0 && g_app.current_preset < g_app.preset_count) {
                strncpy(target_url, g_app.presets[g_app.current_preset].url, sizeof(target_url) - 1);
                strncpy(station_name, g_app.presets[g_app.current_preset].name, sizeof(station_name) - 1);
            }
            g_app.stream_reconfig_needed = false;
            xSemaphoreGive(g_app.mutex);
        }

        if (!want_play || !wifi_ok || target_url[0] == '\0') {
            ring_clear();
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                memset(g_app.vu_bars, 0, sizeof(g_app.vu_bars));
                g_app.vu_peak = 0;
                if (!want_play) {
                    g_app.status = STREAM_STOPPED;
                    strncpy(g_app.status_text, "Paused", sizeof(g_app.status_text));
                } else if (!wifi_ok) {
                    g_app.status = STREAM_CONNECTING_RADIO;
                    strncpy(g_app.status_text, "Waiting for Wi-Fi...", sizeof(g_app.status_text));
                }
                xSemaphoreGive(g_app.mutex);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Apply mirror rotation if URL is a somafm ice server
        char effective_url[140];
        strncpy(effective_url, target_url, sizeof(effective_url) - 1);
        char *ice_pos = strstr(effective_url, "ice6.somafm.com");
        if (!ice_pos) ice_pos = strstr(effective_url, "ice1.somafm.com");
        if (ice_pos && mirror_idx > 0) {
            memcpy(ice_pos, somafm_mirrors[mirror_idx % 3], 15);
        }

        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            g_app.status = STREAM_CONNECTING_RADIO;
            snprintf(g_app.status_text, sizeof(g_app.status_text), "Tuning %s...", station_name);
            snprintf(g_app.track_title, sizeof(g_app.track_title), "Connecting to %s...", station_name);
            xSemaphoreGive(g_app.mutex);
        }

        Serial.printf("[RADIO] Opening stream: %s\n", effective_url);

        WiFiClient plain_client;
        WiFiClientSecure *secure_client = NULL;
        HTTPClient http;
        http.setUserAgent("VLC/3.0.20");
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        http.setConnectTimeout(6000);
        http.setTimeout(6000);

        const char *header_keys[] = {"icy-metaint", "icy-name", "icy-br"};
        http.collectHeaders(header_keys, 3);

        bool begun = false;
        if (strncmp(effective_url, "https://", 8) == 0) {
            secure_client = new WiFiClientSecure();
            secure_client->setInsecure();
            begun = http.begin(*secure_client, effective_url);
        } else {
            begun = http.begin(plain_client, effective_url);
        }

        if (!begun) {
            if (secure_client) delete secure_client;
            mirror_idx++;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        http.addHeader("Icy-MetaData", "1");
        int code = http.GET();
        if (code != HTTP_CODE_OK) {
            Serial.printf("[RADIO] HTTP GET failed (%d) on %s\n", code, effective_url);
            http.end();
            if (secure_client) delete secure_client;
            mirror_idx++;
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                g_app.status = STREAM_ERROR;
                snprintf(g_app.status_text, sizeof(g_app.status_text), "Stream error (%d)", code);
                xSemaphoreGive(g_app.mutex);
            }
            vTaskDelay(pdMS_TO_TICKS(800));
            continue;
        }

        int metaint = 0;
        if (http.hasHeader("icy-metaint")) {
            metaint = http.header("icy-metaint").toInt();
        }
        int br = 128;
        if (http.hasHeader("icy-br")) {
            br = http.header("icy-br").toInt();
        }
        String icy_name = http.header("icy-name");
        Serial.printf("[RADIO] Stream connected! metaint=%d br=%d name=%s\n", metaint, br, icy_name.c_str());

        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            g_app.bitrate_kbps = (br > 0) ? br : 128;
            if (icy_name.length() > 0) {
                strncpy(g_app.icy_station, icy_name.c_str(), sizeof(g_app.icy_station) - 1);
            } else {
                strncpy(g_app.icy_station, station_name, sizeof(g_app.icy_station) - 1);
            }
            snprintf(g_app.track_title, sizeof(g_app.track_title), "%s - Live Stream", station_name);
            xSemaphoreGive(g_app.mutex);
        }

        mp3dec_init(mp3d);
        ring_clear();

        WiFiClient *stream = http.getStreamPtr();
        int bytes_until_meta = metaint;
        int mp3_fill = 0;
        uint32_t last_data_ms = millis();

        while (http.connected()) {
            bool keep_going = true;
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                if (!g_app.playing || !g_app.wifi_connected || g_app.stream_reconfig_needed || g_app.ota_in_progress) {
                    keep_going = false;
                }
                xSemaphoreGive(g_app.mutex);
            }
            if (!keep_going) break;

            // Throttle reading if ringbuffer is nearly full
            if (ring_free_frames() < 2304) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }

            int avail = stream->available();
            if (avail <= 0) {
                if (millis() - last_data_ms > 8000) {
                    Serial.println("[RADIO] Stream read timeout, reconnecting...");
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(3));
                continue;
            }
            last_data_ms = millis();

            int space = 16384 - mp3_fill;
            if (space <= 0) {
                // Drop half if buffer couldn't sync
                memmove(mp3_buf, mp3_buf + 8192, mp3_fill - 8192);
                mp3_fill -= 8192;
                space = 8192;
            }

            int to_read = (avail < space) ? avail : space;
            if (metaint > 0 && to_read > bytes_until_meta) {
                to_read = bytes_until_meta;
            }

            int n = stream->read(mp3_buf + mp3_fill, to_read);
            if (n > 0) {
                mp3_fill += n;
                if (metaint > 0) {
                    bytes_until_meta -= n;
                    if (bytes_until_meta == 0) {
                        // Read 1-byte ICY metadata length
                        uint32_t t0 = millis();
                        while (stream->available() < 1 && millis() - t0 < 2000) {
                            vTaskDelay(pdMS_TO_TICKS(2));
                        }
                        int len_byte = stream->read();
                        if (len_byte > 0) {
                            int mlen = len_byte * 16;
                            int mread = 0;
                            t0 = millis();
                            while (mread < mlen && millis() - t0 < 2500) {
                                int c = stream->read((uint8_t *)meta_buf + mread, mlen - mread);
                                if (c > 0) mread += c;
                                else vTaskDelay(pdMS_TO_TICKS(2));
                            }
                            meta_buf[mread] = '\0';
                            parse_icy_metadata(meta_buf, mread);
                        }
                        bytes_until_meta = metaint;
                    }
                }
            }

            // Decode as many MP3 frames as available
            while (mp3_fill >= 1024 && ring_free_frames() >= 1152) {
                mp3dec_frame_info_t info = {};
                int samples = mp3dec_decode_frame(mp3d, mp3_buf, mp3_fill, pcm_out, &info);
                if (info.frame_bytes == 0) {
                    break; // need more data
                }
                if (samples > 0) {
                    if (info.channels == 2) {
                        ring_push_frames(pcm_out, (uint32_t)samples);
                    } else if (info.channels == 1) {
                        for (int i = 0; i < samples; i++) {
                            stereo_tmp[i * 2]     = pcm_out[i];
                            stereo_tmp[i * 2 + 1] = pcm_out[i];
                        }
                        ring_push_frames(stereo_tmp, (uint32_t)samples);
                    }
                }
                mp3_fill -= info.frame_bytes;
                if (mp3_fill > 0) {
                    memmove(mp3_buf, mp3_buf + info.frame_bytes, mp3_fill);
                }
            }
        }

        http.end();
        if (secure_client) delete secure_client;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Task 2: Real-time 44.1kHz RAOP AirPlay + Local DAC sender
static void raop_sender_task(void *arg) {
    RaopClient *raop = new RaopClient();
    int active_speaker_idx = -1;
    char last_sent_title[128] = {0};
    int16_t chunk_pcm[RAOP_CHUNK_FRAMES * 2];

    uint64_t next_send_us = esp_timer_get_time();

    for (;;) {
        bool want_play = false;
        bool wifi_ok = false;
        int spk_idx = 0;
        int vol = 65;
        bool vol_dirty = false;
        bool also_local = false;
        AirPlaySpeaker spk = {};
        char cur_title[128] = {0};
        char cur_station[40] = {0};

        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            want_play = g_app.playing && !g_app.ota_in_progress;
            wifi_ok = g_app.wifi_connected;
            spk_idx = g_app.current_speaker;
            vol = g_app.volume;
            vol_dirty = g_app.volume_dirty;
            g_app.volume_dirty = false;
            also_local = g_app.also_local_dac;
            if (spk_idx >= 0 && spk_idx < g_app.speaker_count) {
                spk = g_app.speakers[spk_idx];
            }
            strncpy(cur_title, g_app.track_title, sizeof(cur_title) - 1);
            if (g_app.current_preset >= 0 && g_app.current_preset < g_app.preset_count) {
                strncpy(cur_station, g_app.presets[g_app.current_preset].name, sizeof(cur_station) - 1);
            }
            xSemaphoreGive(g_app.mutex);
        }

        if (!want_play || !wifi_ok) {
            if (raop->isConnected()) {
                raop->disconnect();
            }
            active_speaker_idx = -1;
            vTaskDelay(pdMS_TO_TICKS(40));
            next_send_us = esp_timer_get_time();
            continue;
        }

        // Check if speaker changed
        if (spk_idx != active_speaker_idx || (!spk.is_local_dac && !raop->isConnected())) {
            if (raop->isConnected()) {
                raop->disconnect();
            }
            active_speaker_idx = spk_idx;
            last_sent_title[0] = '\0';

            if (!spk.is_local_dac && spk.ip[0] != '\0') {
                if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    g_app.status = STREAM_CONNECTING_AIRPLAY;
                    snprintf(g_app.status_text, sizeof(g_app.status_text), "AirPlay -> %s...", spk.name);
                    xSemaphoreGive(g_app.mutex);
                }
                if (!raop->connect(spk, vol)) {
                    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                        g_app.status = STREAM_ERROR;
                        snprintf(g_app.status_text, sizeof(g_app.status_text), "AirPlay retry: %s", spk.name);
                        xSemaphoreGive(g_app.mutex);
                    }
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    next_send_us = esp_timer_get_time();
                    continue;
                }
            }
            next_send_us = esp_timer_get_time();
        }

        if (vol_dirty && raop->isConnected()) {
            raop->setVolume(vol);
        }

        if (raop->isConnected() && cur_title[0] != '\0' && strcmp(cur_title, last_sent_title) != 0) {
            strncpy(last_sent_title, cur_title, sizeof(last_sent_title) - 1);
            raop->setMetadata(cur_title, cur_station, "SomaFM Radio");
        }

        raop->pollControlAndKeepalive();

        // Wait until at least ~0.25s of audio is buffered before starting playback burst
        uint32_t avail = ring_available_frames();
        if (avail < RAOP_CHUNK_FRAMES) {
            vTaskDelay(pdMS_TO_TICKS(4));
            next_send_us = esp_timer_get_time();
            continue;
        }

        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            if (g_app.status != STREAM_PLAYING) {
                g_app.status = STREAM_PLAYING;
                snprintf(g_app.status_text, sizeof(g_app.status_text), "Streaming -> %s", spk.name);
            }
            xSemaphoreGive(g_app.mutex);
        }

        uint64_t now_us = esp_timer_get_time();
        if (now_us < next_send_us) {
            uint64_t wait_us = next_send_us - now_us;
            if (wait_us > 2000) {
                vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));
            }
            continue;
        }

        if (ring_pop_352(chunk_pcm)) {
            update_vu_meters(chunk_pcm);
            if (!spk.is_local_dac && raop->isConnected()) {
                raop->sendChunk(chunk_pcm);
            }
            if (spk.is_local_dac || also_local) {
                local_dac_write_frames(chunk_pcm, RAOP_CHUNK_FRAMES, vol);
            }
            // 352 / 44100 = 7981.859 microseconds per chunk
            next_send_us += 7982ULL;
            if (now_us > next_send_us + 50000ULL) {
                next_send_us = now_us;
            }
        }
    }
}

void radio_streamer_start(void) {
    if (!s_pcm_ring) {
        s_pcm_ring = (int16_t *)heap_caps_calloc(PCM_RING_FRAMES * 2, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    local_dac_init();

    // Allocate large audio decoder & RAOP task stacks in PSRAM to keep internal SRAM free for TLS/Wi-Fi
    StackType_t *http_stack = (StackType_t *)heap_caps_malloc(28672, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticTask_t *http_tcb  = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    xTaskCreateStaticPinnedToCore(http_decoder_task, "radio_http", 28672 / sizeof(StackType_t), NULL, 3, http_stack, http_tcb, 0);

    StackType_t *raop_stack = (StackType_t *)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticTask_t *raop_tcb  = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    xTaskCreateStaticPinnedToCore(raop_sender_task, "raop_tx", 16384 / sizeof(StackType_t), NULL, 6, raop_stack, raop_tcb, 1);
}

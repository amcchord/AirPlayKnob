#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

#include "radio_streamer.h"
#include "artwork_fetcher.h"
#include "app_state.h"
#include "raop_AnySpeaker.h"
#include "sonos_radio.h"
#include "bsp/local_dac.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <lwip/sockets.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

// 2MB stereo PCM ringbuffer in PSRAM (524288 stereo frames = ~11.89 seconds at 44.1kHz)
#define PCM_RING_FRAMES 524288
// 256KB MP3 buffer in PSRAM (~16.4 seconds at 128kbps) absorbs the entire 260KB Icecast initial burst
// so lwIP's TCP receive window never advertises ZeroWindow!
#define MP3_BUF_SIZE    262144
static int16_t *s_pcm_ring = NULL;
static volatile uint32_t s_ring_write = 0;
static volatile uint32_t s_ring_read = 0;
static volatile uint32_t s_ring_seq = 0;
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t ring_available_frames() {
    portENTER_CRITICAL(&s_ring_mux);
    uint32_t avail = (s_ring_write - s_ring_read) & (PCM_RING_FRAMES - 1);
    portEXIT_CRITICAL(&s_ring_mux);
    return avail;
}

uint32_t radio_get_buffered_ms(void) {
    return (uint32_t)(((uint64_t)ring_available_frames() * 1000ULL) / 44100ULL);
}

static uint32_t ring_free_frames() {
    return (PCM_RING_FRAMES - 1) - ring_available_frames();
}

static void ring_clear() {
    portENTER_CRITICAL(&s_ring_mux);
    s_ring_write = 0;
    s_ring_read = 0;
    s_ring_seq = s_ring_seq + 1;
    portEXIT_CRITICAL(&s_ring_mux);
}

static void ring_push_frames(const int16_t *stereo_frames, uint32_t count) {
    if (!s_pcm_ring || count == 0) return;
    portENTER_CRITICAL(&s_ring_mux);
    uint32_t avail = (s_ring_write - s_ring_read) & (PCM_RING_FRAMES - 1);
    uint32_t free_f = (PCM_RING_FRAMES - 1) - avail;
    uint32_t w = s_ring_write;
    uint32_t seq = s_ring_seq;
    portEXIT_CRITICAL(&s_ring_mux);

    if (count > free_f) count = free_f;
    if (count == 0) return;

    uint32_t first = PCM_RING_FRAMES - w;
    if (first >= count) {
        memcpy(&s_pcm_ring[w * 2], stereo_frames, count * 2 * sizeof(int16_t));
    } else {
        memcpy(&s_pcm_ring[w * 2], stereo_frames, first * 2 * sizeof(int16_t));
        memcpy(&s_pcm_ring[0], &stereo_frames[first * 2], (count - first) * 2 * sizeof(int16_t));
    }

    portENTER_CRITICAL(&s_ring_mux);
    if (s_ring_seq == seq) {
        s_ring_write = (w + count) & (PCM_RING_FRAMES - 1);
    }
    portEXIT_CRITICAL(&s_ring_mux);
}

static bool ring_pop_352(int16_t *out_stereo_352) {
    if (!s_pcm_ring) return false;
    portENTER_CRITICAL(&s_ring_mux);
    uint32_t avail = (s_ring_write - s_ring_read) & (PCM_RING_FRAMES - 1);
    uint32_t r = s_ring_read;
    uint32_t seq = s_ring_seq;
    portEXIT_CRITICAL(&s_ring_mux);

    if (avail < RAOP_CHUNK_FRAMES) {
        return false;
    }

    uint32_t first = PCM_RING_FRAMES - r;
    if (first >= RAOP_CHUNK_FRAMES) {
        memcpy(out_stereo_352, &s_pcm_ring[r * 2], RAOP_CHUNK_FRAMES * 2 * sizeof(int16_t));
    } else {
        memcpy(out_stereo_352, &s_pcm_ring[r * 2], first * 2 * sizeof(int16_t));
        memcpy(&out_stereo_352[first * 2], &s_pcm_ring[0], (RAOP_CHUNK_FRAMES - first) * 2 * sizeof(int16_t));
    }

    portENTER_CRITICAL(&s_ring_mux);
    if (s_ring_seq == seq) {
        s_ring_read = (r + RAOP_CHUNK_FRAMES) & (PCM_RING_FRAMES - 1);
    }
    portEXIT_CRITICAL(&s_ring_mux);
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

// Task 1: HTTP Icecast reader + MP3 decoder -> 2MB PSRAM ringbuffer (~12 seconds cushion)
static void http_decoder_task(void *arg) {
    // Allocate hot MP3 decoder state & frame buffer in fast 240MHz internal SRAM for 3x faster decoding
    mp3dec_t *mp3d = (mp3dec_t *)heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!mp3d) mp3d = (mp3dec_t *)heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *pcm_out = (int16_t *)heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!pcm_out) pcm_out = (int16_t *)heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *stereo_tmp = (int16_t *)heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *mp3_buf = (uint8_t *)heap_caps_malloc(MP3_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *meta_buf = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    int mirror_idx = 0;
    int active_preset = -1;
    // Ordered by fastest measured TTFB (ice4: 212ms, ice2: 222ms, ice6: 230ms, ice1: 257ms)
    const char *somafm_mirrors[] = {"ice4.somafm.com", "ice2.somafm.com", "ice6.somafm.com", "ice1.somafm.com"};

    for (;;) {
        bool want_play = false;
        bool wifi_ok = false;
        bool reconfig = false;
        bool native_without_monitor = false;
        int cur_preset = -1;
        char target_url[128] = {0};
        char station_name[40] = {0};

        bool boot_mdns_busy = false;
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            want_play = g_app.playing && !g_app.ota_in_progress;
            native_without_monitor = (g_app.sonos_native || g_app.sonos_radio_selected) &&
                                     !g_app.also_local_dac && !g_app.whitecap_active;
            wifi_ok = g_app.wifi_connected;
            uint32_t since_wifi = (g_app.wifi_connected_since_ms > 0) ? (millis() - g_app.wifi_connected_since_ms) : 0;
            boot_mdns_busy = wifi_ok && (g_app.speaker_scanning || since_wifi < 1400) && (ring_available_frames() == 0);
            reconfig = g_app.stream_reconfig_needed;
            cur_preset = g_app.current_preset;
            if (g_app.current_preset >= 0 && g_app.current_preset < g_app.preset_count) {
                strncpy(target_url, g_app.presets[g_app.current_preset].url, sizeof(target_url) - 1);
                strncpy(station_name, g_app.presets[g_app.current_preset].name, sizeof(station_name) - 1);
            }
            if (!boot_mdns_busy) {
                g_app.stream_reconfig_needed = false;
            }
            xSemaphoreGive(g_app.mutex);
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        if (boot_mdns_busy) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (native_without_monitor) {
            ring_clear();
            active_preset = -1;
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                g_app.mp3_buf_bytes = 0;
                g_app.pcm_buf_ms = 0;
                g_app.pcm_buf_kb = 0;
                memset(g_app.vu_bars, 0, sizeof(g_app.vu_bars));
                g_app.vu_peak = 0;
                xSemaphoreGive(g_app.mutex);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!want_play || !wifi_ok || target_url[0] == '\0') {
            ring_clear();
            active_preset = -1;
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                memset(g_app.vu_bars, 0, sizeof(g_app.vu_bars));
                g_app.vu_peak = 0;
                g_app.mp3_buf_bytes = 0;
                g_app.pcm_buf_ms = 0;
                g_app.pcm_buf_kb = 0;
                if (!want_play) {
                    g_app.status = STREAM_STOPPED;
                    strncpy(g_app.status_text, "Paused", sizeof(g_app.status_text));
                } else if (!wifi_ok) {
                    g_app.status = STREAM_CONNECTING_RADIO;
                    strncpy(g_app.status_text, "Waiting for Wi-Fi...", sizeof(g_app.status_text));
                }
                xSemaphoreGive(g_app.mutex);
            }
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        bool station_changed = reconfig || (cur_preset != active_preset);
        if (station_changed) {
            ring_clear();
            active_preset = cur_preset;
        }

        // Apply mirror selection if URL is a somafm ice server
        char effective_url[140];
        strncpy(effective_url, target_url, sizeof(effective_url) - 1);
        effective_url[sizeof(effective_url) - 1] = '\0';
        char *ice_pos = strstr(effective_url, "ice6.somafm.com");
        if (!ice_pos) ice_pos = strstr(effective_url, "ice1.somafm.com");
        if (!ice_pos) ice_pos = strstr(effective_url, "ice2.somafm.com");
        if (!ice_pos) ice_pos = strstr(effective_url, "ice4.somafm.com");
        if (ice_pos) {
            memcpy(ice_pos, somafm_mirrors[mirror_idx % 4], 15);
        }

        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            g_app.mp3_buf_bytes = 0;
            uint32_t af = ring_available_frames();
            g_app.pcm_buf_ms = (uint32_t)(((uint64_t)af * 1000ULL) / 44100ULL);
            g_app.pcm_buf_kb = (af * 4U) / 1024U;
            if (af < RAOP_CHUNK_FRAMES) {
                g_app.status = STREAM_CONNECTING_RADIO;
                snprintf(g_app.status_text, sizeof(g_app.status_text), "Tuning %s...", station_name);
                snprintf(g_app.track_title, sizeof(g_app.track_title), "Connecting to %s...", station_name);
            }
            xSemaphoreGive(g_app.mutex);
        }

        uint32_t t_open_ms = millis();
        Serial.printf("[RADIO] Opening stream: %s (keep_ring=%d buf_ms=%lu)\n",
                      effective_url, (int)!station_changed, (unsigned long)radio_get_buffered_ms());

        WiFiClient plain_client;
        WiFiClientSecure *secure_client = NULL;
        HTTPClient http;
        http.setUserAgent("VLC/3.0.20");
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        http.setConnectTimeout(2500);
        http.setTimeout(3500);

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
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }

        http.addHeader("Icy-MetaData", "1");
        int code = http.GET();
        if (code != HTTP_CODE_OK) {
            Serial.printf("[RADIO] HTTP GET failed (%d) on %s after %lums\n",
                          code, effective_url, (unsigned long)(millis() - t_open_ms));
            http.end();
            if (secure_client) delete secure_client;
            mirror_idx++;
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                if (ring_available_frames() < RAOP_CHUNK_FRAMES) {
                    g_app.status = STREAM_ERROR;
                    snprintf(g_app.status_text, sizeof(g_app.status_text), "Retrying %s...", station_name);
                }
                xSemaphoreGive(g_app.mutex);
            }
            vTaskDelay(pdMS_TO_TICKS(150));
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
        Serial.printf("[RADIO] Stream connected in %lums! metaint=%d br=%d name=%s\n",
                      (unsigned long)(millis() - t_open_ms), metaint, br, icy_name.c_str());

        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            g_app.bitrate_kbps = (br > 0) ? br : 128;
            if (icy_name.length() > 0) {
                strncpy(g_app.icy_station, icy_name.c_str(), sizeof(g_app.icy_station) - 1);
            } else {
                strncpy(g_app.icy_station, station_name, sizeof(g_app.icy_station) - 1);
            }
            if (g_app.track_title[0] == '\0' || strncmp(g_app.track_title, "Connecting", 10) == 0) {
                snprintf(g_app.track_title, sizeof(g_app.track_title), "%s - Live Stream", station_name);
            }
            xSemaphoreGive(g_app.mutex);
        }

        mp3dec_init(mp3d);

        WiFiClient *stream = http.getStreamPtr();
        if (stream) {
            stream->setNoDelay(true);
            int sfd = stream->fd();
            if (sfd >= 0) {
                int keepalive = 1;
                int keepidle = 5;
                int keepintvl = 2;
                int keepcnt = 4;
                setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
                setsockopt(sfd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
                setsockopt(sfd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
                setsockopt(sfd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
            }
        }
        int bytes_until_meta = metaint;
        int mp3_fill = 0;
        int mp3_off = 0;
        uint32_t last_data_ms = millis();
        uint32_t last_state_check_ms = 0;
        uint32_t last_diag_ms = millis();
        uint32_t diag_loops = 0;
        uint32_t diag_rx_bytes = 0;
        uint32_t diag_dec_frames = 0;

        while (http.connected()) {
            diag_loops++;
            uint32_t now_ms = millis();
            if (now_ms - last_diag_ms >= 1000) {
                int fion = -1;
                if (stream && stream->fd() >= 0) {
                    lwip_ioctl(stream->fd(), FIONREAD, &fion);
                }
                Serial.printf("[HTTP-1S] loops=%lu rxB=%lu decF=%lu fion=%d mp3Rem=%d bufMs=%lu raop=%d\n",
                              (unsigned long)diag_loops,
                              (unsigned long)diag_rx_bytes,
                              (unsigned long)diag_dec_frames,
                              fion,
                              (mp3_fill - mp3_off),
                              (unsigned long)radio_get_buffered_ms(),
                              (int)g_app.raop_connected);
                diag_loops = 0;
                diag_rx_bytes = 0;
                diag_dec_frames = 0;
                last_diag_ms = now_ms;
            }
            if (now_ms - last_state_check_ms >= 20) {
                last_state_check_ms = now_ms;
                bool keep_going = true;
                if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                    if (!g_app.playing || !g_app.wifi_connected || g_app.stream_reconfig_needed || g_app.ota_in_progress ||
                        ((g_app.sonos_native || g_app.sonos_radio_selected) &&
                         !g_app.also_local_dac && !g_app.whitecap_active)) {
                        keep_going = false;
                    }
                    g_app.mp3_buf_bytes = (uint32_t)(mp3_fill - mp3_off);
                    uint32_t af = ring_available_frames();
                    g_app.pcm_buf_ms = (uint32_t)(((uint64_t)af * 1000ULL) / 44100ULL);
                    g_app.pcm_buf_kb = (af * 4U) / 1024U;
                    xSemaphoreGive(g_app.mutex);
                }
                if (!keep_going) break;
            }

            // Throttle reading only when the 12-second PCM ringbuffer is nearly full
            if (ring_free_frames() < 4608 && (mp3_fill - mp3_off) > (MP3_BUF_SIZE - 8192)) {
                if (!g_app.raop_connected && stream && stream->available() >= 1400) {
                    // Slide live window forward while waiting for AirPlay speaker connection
                    // so Icecast never drops our TCP connection due to TCP ZeroWindow!
                    int16_t discard_pcm[RAOP_CHUNK_FRAMES * 2];
                    for (int d = 0; d < 4; d++) {
                        ring_pop_352(discard_pcm);
                    }
                } else {
                    vTaskDelay(pdMS_TO_TICKS(4));
                    continue;
                }
            }

            // Compact sliding MP3 buffer when needed
            if (mp3_off > 0 && (mp3_off >= 16384 || (MP3_BUF_SIZE - mp3_fill) < 8192)) {
                int rem = mp3_fill - mp3_off;
                if (rem > 0) {
                    memmove(mp3_buf, mp3_buf + mp3_off, rem);
                }
                mp3_fill = rem;
                mp3_off = 0;
            }

            // Drain all available TCP bytes into the 64KB PSRAM mp3_buf first so lwIP's 5744B TCP window
            // stays wide open and absorbs the 260KB Icecast initial burst without ZeroWindow stalls!
            int total_read_this_turn = 0;
            int avail = 0;
            while (total_read_this_turn < 16384 && (avail = stream->available()) > 0) {
                last_data_ms = millis();
                int space = MP3_BUF_SIZE - mp3_fill;
                if (space < 4096 && mp3_off > 0) {
                    int rem = mp3_fill - mp3_off;
                    if (rem > 0) memmove(mp3_buf, mp3_buf + mp3_off, rem);
                    mp3_fill = rem;
                    mp3_off = 0;
                    space = MP3_BUF_SIZE - mp3_fill;
                }
                if (space <= 512) {
                    break; // MP3 buffer is full; decode frames below first
                }

                int to_read = (avail < space) ? avail : space;
                if (to_read > 8192) to_read = 8192;
                if (metaint > 0 && to_read > bytes_until_meta) {
                    to_read = bytes_until_meta;
                }

                int n = stream->read(mp3_buf + mp3_fill, to_read);
                if (n <= 0) break;

                mp3_fill += n;
                total_read_this_turn += n;
                diag_rx_bytes += (uint32_t)n;
                if (metaint > 0) {
                    bytes_until_meta -= n;
                    if (bytes_until_meta == 0) {
                        uint32_t t0 = millis();
                        while (stream->available() < 1 && millis() - t0 < 2000) {
                            vTaskDelay(pdMS_TO_TICKS(1));
                        }
                        int len_byte = stream->read();
                        if (len_byte > 0) {
                            int mlen = len_byte * 16;
                            int mread = 0;
                            t0 = millis();
                            while (mread < mlen && millis() - t0 < 2500) {
                                int c = stream->read((uint8_t *)meta_buf + mread, mlen - mread);
                                if (c > 0) mread += c;
                                else vTaskDelay(pdMS_TO_TICKS(1));
                            }
                            meta_buf[mread] = '\0';
                            parse_icy_metadata(meta_buf, mread);
                        }
                        bytes_until_meta = metaint;
                    }
                }
            }

            if (total_read_this_turn == 0 && (mp3_fill - mp3_off) < 65536 && (millis() - last_data_ms > 5500)) {
                Serial.printf("[RADIO] Stream read timeout (5.5s, mp3_rem=%d, buf_ms=%lu), rotating mirror...\n",
                              (mp3_fill - mp3_off), (unsigned long)radio_get_buffered_ms());
                // Decode any remaining MP3 frames that fit into the PCM ringbuffer before reconnecting
                while ((mp3_fill - mp3_off) >= 640 && ring_free_frames() >= 1152) {
                    mp3dec_frame_info_t info = {};
                    int samples = mp3dec_decode_frame(mp3d, mp3_buf + mp3_off, mp3_fill - mp3_off, pcm_out, &info);
                    if (info.frame_bytes == 0) break;
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
                    mp3_off += info.frame_bytes;
                }
                mirror_idx++;
                break;
            }

            if (total_read_this_turn == 0 && (mp3_fill - mp3_off) < 640) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            if (ring_free_frames() < 1152) {
                if (total_read_this_turn == 0) {
                    vTaskDelay(pdMS_TO_TICKS(2));
                }
                continue;
            }

            // Decode MP3 frames into PCM ringbuffer, yielding immediately if new TCP segments arrive
            // while mp3_buf still has plenty of room so lwIP never hits TCP ZeroWindow.
            int decoded_batches = 0;
            while ((mp3_fill - mp3_off) >= 640 && ring_free_frames() >= 1152 && decoded_batches < 8) {
                mp3dec_frame_info_t info = {};
                int samples = mp3dec_decode_frame(mp3d, mp3_buf + mp3_off, mp3_fill - mp3_off, pcm_out, &info);
                if (info.frame_bytes == 0) {
                    break; // need more MP3 bytes
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
                mp3_off += info.frame_bytes;
                decoded_batches++;
                diag_dec_frames++;
                if ((MP3_BUF_SIZE - (mp3_fill - mp3_off)) >= 8192 && stream->available() >= 1400) {
                    break; // Immediately drain incoming TCP segments from lwIP!
                }
            }

            if (decoded_batches == 0 && total_read_this_turn == 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }

        http.end();
        if (secure_client) delete secure_client;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// Task 2: Real-time 44.1kHz RAOP AirPlay + Local DAC sender
static void raop_sender_task(void *arg) {
    RaopClient *raop = new RaopClient();
    SonosRadioClient sonos;
    char native_url[128] = {0};
    char native_rejected_ip[20] = {0};
    char active_speaker_ip[20] = {0};
    bool active_is_local_dac = true;
    bool speaker_initialized = false;
    int connect_failures = 0;
    char last_sent_title[128] = {0};
    uint32_t last_sent_art_seq = 0;
    uint8_t *art_jpg_buf = (uint8_t *)heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *chunk_pcm = (int16_t *)heap_caps_calloc(RAOP_CHUNK_FRAMES * 2, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint64_t next_send_us = esp_timer_get_time();
    bool prebuffering = true;

    for (;;) {
        bool want_play = false;
        bool wifi_ok = false;
        int spk_idx = 0;
        int vol = 65;
        bool vol_dirty = false;
        bool also_local = false;
        bool want_visualizer = false;
        bool prefer_sonos_native = false;
        bool force_pin_reconnect = false;
        bool pin_halted = false;
        bool radio_connecting = false;
        uint32_t mp3_bytes_buffered = 0;
        AirPlaySpeaker spk = {};
        char cur_title[128] = {0};
        char cur_station[40] = {0};
        char cur_url[128] = {0};

        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            want_play = g_app.playing && !g_app.ota_in_progress;
            wifi_ok = g_app.wifi_connected;
            spk_idx = g_app.current_speaker;
            vol = g_app.volume;
            vol_dirty = g_app.volume_dirty;
            g_app.volume_dirty = false;
            also_local = g_app.also_local_dac;
            want_visualizer = g_app.whitecap_active;
            prefer_sonos_native = g_app.prefer_sonos_native;
            force_pin_reconnect = g_app.pin_manual_request;
            radio_connecting = (g_app.status == STREAM_CONNECTING_RADIO || g_app.stream_reconfig_needed);
            mp3_bytes_buffered = g_app.mp3_buf_bytes;
            if (spk_idx >= 0 && spk_idx < g_app.speaker_count) {
                spk = g_app.speakers[spk_idx];
            } else {
                spk.is_local_dac = true;
                strncpy(spk.name, "Local 3.5mm Jack", sizeof(spk.name) - 1);
            }
            if (spk.is_local_dac && force_pin_reconnect) {
                g_app.pin_manual_request = false;
                force_pin_reconnect = false;
            }
            if (!spk.is_local_dac && spk.ip[0] != '\0' && g_app.pin_halted_ip[0] != '\0' &&
                strcmp(spk.ip, g_app.pin_halted_ip) == 0 && !force_pin_reconnect) {
                pin_halted = true;
            }
            strncpy(cur_title, g_app.track_title, sizeof(cur_title) - 1);
            if (g_app.current_preset >= 0 && g_app.current_preset < g_app.preset_count) {
                strncpy(cur_station, g_app.presets[g_app.current_preset].name, sizeof(cur_station) - 1);
                strncpy(cur_url, g_app.presets[g_app.current_preset].url, sizeof(cur_url) - 1);
            }
            uint32_t af = ring_available_frames();
            g_app.pcm_buf_ms = (uint32_t)(((uint64_t)af * 1000ULL) / 44100ULL);
            g_app.pcm_buf_kb = (af * 4U) / 1024U;
            g_app.raop_connected = raop->isConnected();
            g_app.raop_is_ap2 = raop->isAirPlay2();
            g_app.sonos_native = sonos.isConnected();
            // Stop the duplicate source download before the native handshake,
            // rather than waiting until it succeeds. Otherwise recovery from
            // a congested AirPlay stream keeps competing with local decoding.
            g_app.sonos_radio_selected = prefer_sonos_native && !force_pin_reconnect &&
                                        SonosRadioClient::isCandidate(spk) &&
                                        strcmp(native_rejected_ip, spk.ip) != 0;
            g_app.raop_pkts_sent = raop->getPktsSent();
            g_app.raop_ntp_replies = raop->getNtpReplies();
            g_app.raop_retransmits = raop->getRetransmits();
            g_app.raop_tx_accepted = raop->txStats().accepted;
            g_app.raop_tx_rejected = raop->txStats().rejected;
            g_app.raop_tx_retries = raop->txStats().retries;
            g_app.raop_tx_last_error = raop->txStats().last_error;
            g_app.raop_repair_rejected = raop->repairStats().rejected;
            g_app.raop_repair_pending = raop->pendingRepairs();
            xSemaphoreGive(g_app.mutex);
        } else {
            // A busy UI is not a playback/power-state change. Keep the current
            // session and buffered audio intact until a snapshot is available.
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (!want_play || !wifi_ok) {
            if (!want_play) sonos.stop();
            if (raop->isConnected()) {
                raop->disconnect();
            }
            speaker_initialized = false;
            prebuffering = true;
            vTaskDelay(pdMS_TO_TICKS(40));
            next_send_us = esp_timer_get_time();
            continue;
        }

        // Compare speaker by IP / is_local_dac so background mDNS scans never trigger false reconnects!
        bool speaker_changed = !speaker_initialized ||
                               (spk.is_local_dac != active_is_local_dac) ||
                               (!spk.is_local_dac && strcmp(spk.ip, active_speaker_ip) != 0);

        if (force_pin_reconnect && SonosRadioClient::isCandidate(spk)) {
            strncpy(native_rejected_ip, spk.ip, sizeof(native_rejected_ip) - 1);
        }
        bool use_sonos = prefer_sonos_native && SonosRadioClient::isCandidate(spk) &&
                         strcmp(native_rejected_ip, spk.ip) != 0;
        if (!use_sonos && sonos.isConnected()) sonos.stop();

        if (use_sonos) {
            if (raop->isConnected()) raop->disconnect();
            bool same_stream = sonos.isConnected() &&
                               strcmp(active_speaker_ip, spk.ip) == 0 && strcmp(native_url, cur_url) == 0;
            if (!same_stream) {
                if (sonos.isConnected() && strcmp(active_speaker_ip, spk.ip) != 0) sonos.stop();
                if (!sonos.start(spk, cur_url, cur_station, vol)) {
                    if (sonos.rejected()) strncpy(native_rejected_ip, spk.ip, sizeof(native_rejected_ip) - 1);
                    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                        g_app.status = STREAM_ERROR;
                        snprintf(g_app.status_text, sizeof(g_app.status_text), "Sonos retry: %s", spk.name);
                        xSemaphoreGive(g_app.mutex);
                    }
                    vTaskDelay(pdMS_TO_TICKS(1200));
                    continue;
                }
                strncpy(native_url, cur_url, sizeof(native_url) - 1);
                prebuffering = true;
                next_send_us = esp_timer_get_time();
            }
            speaker_initialized = true;
            active_is_local_dac = false;
            strncpy(active_speaker_ip, spk.ip, sizeof(active_speaker_ip) - 1);
            if (vol_dirty) sonos.setVolume(vol);
            sonos.poll();
            if (!also_local && !want_visualizer) {
                if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    g_app.status = STREAM_PLAYING;
                    snprintf(g_app.status_text, sizeof(g_app.status_text), "Sonos radio -> %s", spk.name);
                    xSemaphoreGive(g_app.mutex);
                }
                prebuffering = true;
                next_send_us = esp_timer_get_time();
                vTaskDelay(pdMS_TO_TICKS(40));
                continue;
            }
        } else if (pin_halted && !force_pin_reconnect) {
            if (raop->isConnected()) {
                raop->disconnect();
            }
            speaker_initialized = true;
            active_is_local_dac = spk.is_local_dac;
            strncpy(active_speaker_ip, spk.ip, sizeof(active_speaker_ip) - 1);
            prebuffering = true;
            vTaskDelay(pdMS_TO_TICKS(80));
            next_send_us = esp_timer_get_time();
            continue;
        } else if (force_pin_reconnect || speaker_changed || (!spk.is_local_dac && !raop->isConnected())) {
            if (raop->isConnected()) {
                raop->disconnect();
            }
            speaker_initialized = true;
            active_is_local_dac = spk.is_local_dac;
            strncpy(active_speaker_ip, spk.ip, sizeof(active_speaker_ip) - 1);
            last_sent_title[0] = '\0';
            last_sent_art_seq = 0;
            prebuffering = true;

            if (!spk.is_local_dac) {
                if (spk.ip[0] == '\0') {
                    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                        g_app.status = STREAM_CONNECTING_AIRPLAY;
                        snprintf(g_app.status_text, sizeof(g_app.status_text), "Finding %s...", spk.name);
                        xSemaphoreGive(g_app.mutex);
                    }
                    speaker_initialized = false;
                    vTaskDelay(pdMS_TO_TICKS(150));
                    next_send_us = esp_timer_get_time();
                    continue;
                }
                // Wait for initial mDNS discovery (~2.5s after Wi-Fi link up) and initial Icecast prebuffer
                // (~3s PCM or >=48KB MP3) before starting the blocking AirPlay RTSP/HomeKit handshake,
                // so mDNS discovers all speakers and SomaFM fills the PSRAM ringbuffer without contention.
                uint32_t since_wifi_ms = (g_app.wifi_connected_since_ms > 0)
                                             ? (millis() - g_app.wifi_connected_since_ms)
                                             : 0;
                bool mdns_ready = (g_app.speaker_count >= 10) || (since_wifi_ms >= 3500);
                bool audio_prebuffered = (ring_available_frames() >= 44100 * 3) ||
                                         (mp3_bytes_buffered >= 49152) ||
                                         (ring_available_frames() >= 44100 && since_wifi_ms >= 7500);
                if (!force_pin_reconnect && (!mdns_ready || !audio_prebuffered)) {
                    speaker_initialized = false;
                    vTaskDelay(pdMS_TO_TICKS(100));
                    next_send_us = esp_timer_get_time();
                    continue;
                }
                if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    g_app.status = STREAM_CONNECTING_AIRPLAY;
                    snprintf(g_app.status_text, sizeof(g_app.status_text), "AirPlay -> %s...", spk.name);
                    xSemaphoreGive(g_app.mutex);
                }
                if (!raop->connect(spk, vol)) {
                    connect_failures++;
                    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                        g_app.status = STREAM_ERROR;
                        bool now_halted = (g_app.pin_halted_ip[0] != '\0' && strcmp(spk.ip, g_app.pin_halted_ip) == 0);
                        if (!now_halted) {
                            snprintf(g_app.status_text, sizeof(g_app.status_text), "AirPlay retry: %s", spk.name);
                            if (connect_failures >= 2) {
                                g_app.speaker_scan_requested = true;
                            }
                        }
                        xSemaphoreGive(g_app.mutex);
                    }
                    vTaskDelay(pdMS_TO_TICKS(1200));
                    next_send_us = esp_timer_get_time();
                    continue;
                }
                connect_failures = 0;
            }
            next_send_us = esp_timer_get_time();
        }

        if (vol_dirty && raop->isConnected()) {
            raop->setVolume(vol);
        }

        if (raop->isConnected() && !prebuffering &&
            cur_title[0] != '\0' && strncmp(cur_title, "Connecting", 10) != 0 &&
            strcmp(cur_title, last_sent_title) != 0) {
            strncpy(last_sent_title, cur_title, sizeof(last_sent_title) - 1);
            raop->setMetadata(cur_title, cur_station, "SomaFM Radio");
        }

        uint32_t cur_art_seq = artwork_get_jpeg_seq();
        if (raop->isConnected() && !prebuffering && art_jpg_buf &&
            cur_art_seq != 0 && cur_art_seq != last_sent_art_seq) {
            uint32_t copied_seq = 0;
            size_t jpg_len = artwork_copy_jpeg(art_jpg_buf, 64 * 1024, &copied_seq);
            if (jpg_len > 0) {
                last_sent_art_seq = copied_seq;
                raop->setArtwork(art_jpg_buf, jpg_len);
            }
        }

        raop->pollControlAndKeepalive(prebuffering || !raop->acceptFrames());

        // Start playback once >= 1.5s of PCM + >= 16KB MP3 is buffered (or >= 2.0s PCM),
        // while the remaining Icecast burst continues decoding into the 2MB PSRAM ringbuffer!
        uint32_t avail = ring_available_frames();
        if (prebuffering) {
            uint32_t min_start_frames = (mp3_bytes_buffered >= 16384) ? 66150 : 88200;
            if (avail < min_start_frames) {
                vTaskDelay(pdMS_TO_TICKS(4));
                next_send_us = esp_timer_get_time();
                continue;
            }
            prebuffering = false;
            if (!spk.is_local_dac && raop->isConnected()) {
                raop->resyncTimestamp();
            }
            next_send_us = esp_timer_get_time();
        } else if (avail < RAOP_CHUNK_FRAMES) {
            if (!radio_connecting) {
                Serial.println("[RADIO] PCM ringbuffer underrun; re-buffering...");
                if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                    g_app.stream_underruns++;
                    xSemaphoreGive(g_app.mutex);
                }
            }
            prebuffering = true;
            vTaskDelay(pdMS_TO_TICKS(5));
            next_send_us = esp_timer_get_time();
            continue;
        }

        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            if (g_app.status != STREAM_PLAYING) {
                g_app.status = STREAM_PLAYING;
                snprintf(g_app.status_text, sizeof(g_app.status_text), "%s -> %s",
                         use_sonos ? "Sonos radio" : "Streaming", spk.name);
            }
            xSemaphoreGive(g_app.mutex);
        }

        if (!spk.is_local_dac && raop->isConnected()) {
            if (!raop->acceptFrames()) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            if (ring_pop_352(chunk_pcm)) {
                update_vu_meters(chunk_pcm);
                raop->sendChunk(chunk_pcm);
                (void)also_local;
            }
        } else if (spk.is_local_dac || (use_sonos && sonos.isConnected())) {
            uint64_t now_us = esp_timer_get_time();
            if (now_us < next_send_us) {
                uint64_t wait_us = next_send_us - now_us;
                uint32_t delay_ms = (uint32_t)(wait_us / 1000ULL);
                vTaskDelay(pdMS_TO_TICKS(delay_ms > 0 ? delay_ms : 1));
                continue;
            }
            if (ring_pop_352(chunk_pcm)) {
                update_vu_meters(chunk_pcm);
                if (spk.is_local_dac || also_local) local_dac_write_frames(chunk_pcm, RAOP_CHUNK_FRAMES, vol);
                next_send_us += 7982ULL;
                uint64_t after_us = esp_timer_get_time();
                if (after_us > next_send_us + 50000ULL) {
                    next_send_us = after_us;
                } else if (after_us < next_send_us) {
                    uint64_t wait_us = next_send_us - after_us;
                    uint32_t delay_ms = (uint32_t)(wait_us / 1000ULL);
                    vTaskDelay(pdMS_TO_TICKS(delay_ms > 0 ? delay_ms : 1));
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
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
    xTaskCreateStaticPinnedToCore(http_decoder_task, "radio_http", 28672 / sizeof(StackType_t), NULL, 4, http_stack, http_tcb, 0);

    // Run RAOP sender + I2S DAC task on a genuine Internal SRAM stack so it never panics during flash/driver operations
    xTaskCreatePinnedToCore(raop_sender_task, "raop_tx", 14336, NULL, 6, NULL, 1);
}

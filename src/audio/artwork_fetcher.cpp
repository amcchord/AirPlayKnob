#include "artwork_fetcher.h"
#include "radio_streamer.h"
#include "app_state.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <extra/libs/sjpg/tjpgd.h>
#include <math.h>

#define MAX_JPG_DIM 300

static lv_color_t *s_art_pixels = NULL;
static lv_color_t *s_station_cache_pixels = NULL;
static lv_img_dsc_t s_art_dsc = {};
static volatile uint32_t s_art_seq = 0;

struct JpgMemSession {
    const uint8_t *data;
    size_t len;
    size_t pos;
    uint8_t *rgb_raw; // w * h * 3
    int w;
    int h;
};

static size_t tjpg_input_cb(JDEC *jd, uint8_t *buff, size_t ndata) {
    JpgMemSession *sess = (JpgMemSession *)jd->device;
    if (sess->pos >= sess->len) return 0;
    size_t rem = sess->len - sess->pos;
    if (ndata > rem) ndata = rem;
    if (buff) {
        memcpy(buff, sess->data + sess->pos, ndata);
    }
    sess->pos += ndata;
    return ndata;
}

static int tjpg_output_cb(JDEC *jd, void *bitmap, JRECT *rect) {
    JpgMemSession *sess = (JpgMemSession *)jd->device;
    if (!sess->rgb_raw) return 0;
    const uint8_t *src = (const uint8_t *)bitmap;
    int bw = rect->right - rect->left + 1;
    for (int y = rect->top; y <= rect->bottom; y++) {
        if (y >= sess->h) break;
        int row_off = (y - rect->top) * bw * 3;
        for (int x = rect->left; x <= rect->right; x++) {
            if (x >= sess->w) break;
            int so = row_off + (x - rect->left) * 3;
            int dst_off = (y * sess->w + x) * 3;
            sess->rgb_raw[dst_off + 0] = src[so + 0];
            sess->rgb_raw[dst_off + 1] = src[so + 1];
            sess->rgb_raw[dst_off + 2] = src[so + 2];
        }
    }
    return 1;
}

// Renders full-screen 360x360 bespoke geometric SomaFM stream art into `dst`
static void render_procedural_stream_art(lv_color_t *dst, const StationPreset &st) {
    uint32_t col = st.color ? st.color : 0x10B981;
    uint8_t cr = (col >> 16) & 0xFF;
    uint8_t cg = (col >> 8) & 0xFF;
    uint8_t cb = col & 0xFF;

    uint32_t seed = 0;
    for (const char *p = st.id; *p; p++) seed = seed * 31 + (uint8_t)(*p);
    int rings = 4 + (seed % 4);
    float freq = 3.0f + (seed % 5);
    float phase = (seed % 360) * 0.017453f;

    const float cx = (ARTWORK_SIZE - 1) * 0.5f;
    const float cy = 148.0f; // Slightly above center so bottom overlay text frames the emblem

    for (int y = 0; y < ARTWORK_SIZE; y++) {
        // Bottom scrim factor for y = 185..359 so bottom track/station text is ultra-legible
        float scrim = 1.0f;
        if (y > 180) {
            float t = (float)(y - 180) / 115.0f;
            if (t > 1.0f) t = 1.0f;
            scrim = 1.0f - 0.86f * (t * t);
        }

        for (int x = 0; x < ARTWORK_SIZE; x++) {
            float dx = (float)x - cx;
            float dy = (float)y - cy;
            float dist = sqrtf(dx * dx + dy * dy);
            float ang = atan2f(dy, dx);

            float norm_d = dist / 210.0f;
            if (norm_d > 1.0f) norm_d = 1.0f;
            float glow = (1.0f - norm_d) * (1.0f - norm_d);

            float r = 8.0f + cr * 0.42f * glow;
            float g = 10.0f + cg * 0.42f * glow;
            float b = 16.0f + cb * 0.42f * glow;

            float wave_r = 72.0f + 16.0f * sinf(ang * freq + phase) + 8.0f * cosf(ang * (freq - 1.0f));
            float ring_d = fabsf(dist - wave_r);
            if (ring_d < 3.2f) {
                float k = (3.2f - ring_d) / 3.2f;
                r += (cr - r + 65.0f) * k;
                g += (cg - g + 65.0f) * k;
                b += (cb - b + 65.0f) * k;
            }

            for (int i = 1; i <= rings; i++) {
                float rad = 28.0f * i;
                float od = fabsf(dist - rad);
                if (od < 1.5f) {
                    float k = (1.5f - od) * 0.55f;
                    r += cr * k;
                    g += cg * k;
                    b += cb * k;
                }
            }

            r = r * scrim + 4.0f * (1.0f - scrim);
            g = g * scrim + 5.0f * (1.0f - scrim);
            b = b * scrim + 8.0f * (1.0f - scrim);

            if (r > 255.0f) r = 255.0f;
            if (g > 255.0f) g = 255.0f;
            if (b > 255.0f) b = 255.0f;
            dst[y * ARTWORK_SIZE + x] = lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
    }
}

static bool decode_jpeg_to_artwork(const uint8_t *jpg_data, size_t jpg_len, lv_color_t *dst, uint32_t accent_col) {
    (void)accent_col;
    void *work = heap_caps_malloc(3800, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!work) return false;

    JDEC jdec = {};
    JpgMemSession sess = {};
    sess.data = jpg_data;
    sess.len = jpg_len;
    sess.pos = 0;

    JRESULT res = jd_prepare(&jdec, tjpg_input_cb, work, 3800, &sess);
    if (res != JDR_OK) {
        heap_caps_free(work);
        return false;
    }

    uint8_t scale = 0;
    uint16_t sw = jdec.width;
    uint16_t sh = jdec.height;
    while ((sw > MAX_JPG_DIM || sh > MAX_JPG_DIM) && scale < 3) {
        scale++;
        sw >>= 1;
        sh >>= 1;
    }
    sess.w = (int)sw;
    sess.h = (int)sh;
    sess.rgb_raw = (uint8_t *)heap_caps_calloc(sw * sh * 3, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sess.rgb_raw) {
        heap_caps_free(work);
        return false;
    }

    res = jd_decomp(&jdec, tjpg_output_cb, scale);
    heap_caps_free(work);

    if (res != JDR_OK) {
        heap_caps_free(sess.rgb_raw);
        return false;
    }

    // Bilinear resample from (sw x sh) to full-screen 360x360 with bottom vignette scrim
    for (int y = 0; y < ARTWORK_SIZE; y++) {
        float fy = ((float)y * (sw > 1 ? (sh - 1) : 0)) / (float)(ARTWORK_SIZE - 1);
        int y0 = (int)fy;
        int y1 = (y0 + 1 < sess.h) ? (y0 + 1) : y0;
        float wy = fy - y0;

        // Smooth bottom dark gradient scrim for y = 180..359 so overlaid track/station info pops
        float scrim = 1.0f;
        if (y > 180) {
            float t = (float)(y - 180) / 112.0f;
            if (t > 1.0f) t = 1.0f;
            scrim = 1.0f - 0.86f * (t * t);
        }

        for (int x = 0; x < ARTWORK_SIZE; x++) {
            float fx = ((float)x * (sw > 1 ? (sw - 1) : 0)) / (float)(ARTWORK_SIZE - 1);
            int x0 = (int)fx;
            int x1 = (x0 + 1 < sess.w) ? (x0 + 1) : x0;
            float wx = fx - x0;

            const uint8_t *p00 = &sess.rgb_raw[(y0 * sess.w + x0) * 3];
            const uint8_t *p10 = &sess.rgb_raw[(y0 * sess.w + x1) * 3];
            const uint8_t *p01 = &sess.rgb_raw[(y1 * sess.w + x0) * 3];
            const uint8_t *p11 = &sess.rgb_raw[(y1 * sess.w + x1) * 3];

            float rf = ((p00[0] * (1 - wx) + p10[0] * wx) * (1 - wy) +
                        (p01[0] * (1 - wx) + p11[0] * wx) * wy);
            float gf = ((p00[1] * (1 - wx) + p10[1] * wx) * (1 - wy) +
                        (p01[1] * (1 - wx) + p11[1] * wx) * wy);
            float bf = ((p00[2] * (1 - wx) + p10[2] * wx) * (1 - wy) +
                        (p01[2] * (1 - wx) + p11[2] * wx) * wy);

            uint8_t r = (uint8_t)(rf * scrim + 4.0f * (1.0f - scrim));
            uint8_t g = (uint8_t)(gf * scrim + 5.0f * (1.0f - scrim));
            uint8_t b = (uint8_t)(bf * scrim + 8.0f * (1.0f - scrim));

            dst[y * ARTWORK_SIZE + x] = lv_color_make(r, g, b);
        }
    }

    heap_caps_free(sess.rgb_raw);
    return true;
}

static bool download_url_to_buf(const char *url, uint8_t *buf, size_t max_len, size_t *out_len) {
    *out_len = 0;
    WiFiClientSecure sc;
    sc.setInsecure();
    WiFiClient pc;
    HTTPClient http;
    http.setUserAgent("Mozilla/5.0 (AirPlayKnob/1.0)");
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setConnectTimeout(4500);
    http.setTimeout(4500);

    bool ok = false;
    if (strncmp(url, "https://", 8) == 0) {
        ok = http.begin(sc, url);
    } else {
        ok = http.begin(pc, url);
    }
    if (!ok) return false;

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        return false;
    }

    int total = http.getSize();
    if (total > (int)max_len) {
        http.end();
        return false;
    }

    WiFiClient *s = http.getStreamPtr();
    size_t got = 0;
    uint32_t t0 = millis();
    while (http.connected() && (total < 0 || got < (size_t)total) && got < max_len) {
        int avail = s->available();
        if (avail > 0) {
            int to_read = (avail < (int)(max_len - got)) ? avail : (int)(max_len - got);
            int n = s->read(buf + got, to_read);
            if (n > 0) got += n;
            t0 = millis();
        } else {
            if (millis() - t0 > 3500) break;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    http.end();
    *out_len = got;
    return got > 256;
}

static void url_encode_query(const char *src, char *dst, size_t dst_max) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 4 < dst_max; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            dst[j++] = (char)c;
        } else if (c == ' ') {
            dst[j++] = '+';
        } else {
            snprintf(dst + j, 4, "%%%02X", c);
            j += 3;
        }
    }
    dst[j] = '\0';
}

static void artwork_worker_task(void *arg) {
    (void)arg;
    const size_t MAX_DL = 64 * 1024;
    uint8_t *dl_buf = (uint8_t *)heap_caps_malloc(MAX_DL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_color_t *work_pixels = (lv_color_t *)heap_caps_malloc(ARTWORK_SIZE * ARTWORK_SIZE * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    int last_preset = -1;
    char last_track[128] = {0};
    bool station_jpg_loaded = false;

    for (;;) {
        bool wifi_ok = false;
        bool ota = false;
        bool spk_scan = false;
        int cur_preset = 0;
        StationPreset st = {};
        char cur_track[128] = {0};

        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            wifi_ok = g_app.wifi_connected;
            ota = g_app.ota_in_progress;
            spk_scan = g_app.speaker_scanning;
            cur_preset = g_app.current_preset;
            if (cur_preset >= 0 && cur_preset < g_app.preset_count) {
                st = g_app.presets[cur_preset];
            }
            strncpy(cur_track, g_app.track_title, sizeof(cur_track) - 1);
            xSemaphoreGive(g_app.mutex);
        }

        if (ota || spk_scan) {
            vTaskDelay(pdMS_TO_TICKS(400));
            continue;
        }

        // 1. Did station preset change?
        if (cur_preset != last_preset) {
            last_preset = cur_preset;
            last_track[0] = '\0';
            station_jpg_loaded = false;

            // Immediately display procedural SomaFM cover so UI responds in <5ms
            render_procedural_stream_art(s_station_cache_pixels, st);
            memcpy(s_art_pixels, s_station_cache_pixels, ARTWORK_SIZE * ARTWORK_SIZE * sizeof(lv_color_t));
            s_art_seq = s_art_seq + 1;

            char somafm_art_url[128];
            snprintf(somafm_art_url, sizeof(somafm_art_url), "https://somafm.com/img/%s120.jpg", st.id);
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                strncpy(g_app.artwork_url, somafm_art_url, sizeof(g_app.artwork_url) - 1);
                xSemaphoreGive(g_app.mutex);
            }
        }

        // 2. If Wi-Fi is up and we haven't downloaded the SomaFM station JPEG yet, fetch it
        //    Wait until at least 2.0s of audio is buffered so TLS handshake never starves initial playback!
        if (wifi_ok && !station_jpg_loaded && st.id[0] != '\0' &&
            (!g_app.playing || radio_get_buffered_ms() >= 2000)) {
            char somafm_art_url[128];
            snprintf(somafm_art_url, sizeof(somafm_art_url), "https://somafm.com/img/%s120.jpg", st.id);
            size_t jpg_len = 0;
            if (download_url_to_buf(somafm_art_url, dl_buf, MAX_DL, &jpg_len)) {
                if (decode_jpeg_to_artwork(dl_buf, jpg_len, s_station_cache_pixels, st.color)) {
                    memcpy(s_art_pixels, s_station_cache_pixels, ARTWORK_SIZE * ARTWORK_SIZE * sizeof(lv_color_t));
                    s_art_seq = s_art_seq + 1;
                    Serial.printf("[ART] Loaded SomaFM stream art for '%s' (%u bytes)\n", st.name, (unsigned)jpg_len);
                }
            }
            station_jpg_loaded = true;
        }

        // 3. Did the live ICY track change? Query iTunes for Album Art once audio buffer has >= 2.2s cushion!
        if (wifi_ok && cur_track[0] != '\0' && strcmp(cur_track, last_track) != 0 &&
            (!g_app.playing || radio_get_buffered_ms() >= 2200)) {
            strncpy(last_track, cur_track, sizeof(last_track) - 1);

            bool is_real_song = (strstr(cur_track, " - ") != NULL) &&
                                (strstr(cur_track, "Live Stream") == NULL) &&
                                (strstr(cur_track, "Connecting") == NULL) &&
                                (strstr(cur_track, "SomaFM") == NULL);

            bool got_track_art = false;
            if (is_real_song) {
                char encoded[256];
                url_encode_query(cur_track, encoded, sizeof(encoded));
                char search_url[360];
                snprintf(search_url, sizeof(search_url),
                         "https://itunes.apple.com/search?term=%s&media=music&entity=song&limit=1", encoded);

                size_t json_len = 0;
                if (download_url_to_buf(search_url, dl_buf, 16384, &json_len)) {
                    dl_buf[json_len] = '\0';
                    char *art_key = strstr((char *)dl_buf, "\"artworkUrl100\":\"");
                    if (art_key) {
                        art_key += 17;
                        char *art_end = strchr(art_key, '"');
                        if (art_end && (art_end - art_key) < 230) {
                            char art_url[256] = {0};
                            memcpy(art_url, art_key, art_end - art_key);
                            // Upgrade 100x100bb.jpg to 300x300bb.jpg for sharp full-screen 360x360 display
                            char *sz_pos = strstr(art_url, "100x100bb.jpg");
                            if (sz_pos) {
                                memcpy(sz_pos, "300x300bb.jpg", 13);
                            }
                            size_t jpg_len = 0;
                            if (download_url_to_buf(art_url, dl_buf, MAX_DL, &jpg_len)) {
                                if (decode_jpeg_to_artwork(dl_buf, jpg_len, work_pixels, st.color)) {
                                    memcpy(s_art_pixels, work_pixels, ARTWORK_SIZE * ARTWORK_SIZE * sizeof(lv_color_t));
                                    s_art_seq = s_art_seq + 1;
                                    got_track_art = true;
                                    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                                        strncpy(g_app.artwork_url, art_url, sizeof(g_app.artwork_url) - 1);
                                        xSemaphoreGive(g_app.mutex);
                                    }
                                    Serial.printf("[ART] Loaded live Album Art for '%s' (%u bytes)\n", cur_track, (unsigned)jpg_len);
                                }
                            }
                        }
                    }
                }
            }

            if (!got_track_art) {
                memcpy(s_art_pixels, s_station_cache_pixels, ARTWORK_SIZE * ARTWORK_SIZE * sizeof(lv_color_t));
                s_art_seq = s_art_seq + 1;
                char somafm_art_url[128];
                snprintf(somafm_art_url, sizeof(somafm_art_url), "https://somafm.com/img/%s120.jpg", st.id);
                if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    strncpy(g_app.artwork_url, somafm_art_url, sizeof(g_app.artwork_url) - 1);
                    xSemaphoreGive(g_app.mutex);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void artwork_fetcher_start(void) {
    if (!s_art_pixels) {
        s_art_pixels = (lv_color_t *)heap_caps_calloc(ARTWORK_SIZE * ARTWORK_SIZE, sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_station_cache_pixels) {
        s_station_cache_pixels = (lv_color_t *)heap_caps_calloc(ARTWORK_SIZE * ARTWORK_SIZE, sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    s_art_dsc.header.always_zero = 0;
    s_art_dsc.header.w = ARTWORK_SIZE;
    s_art_dsc.header.h = ARTWORK_SIZE;
    s_art_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_art_dsc.data_size = ARTWORK_SIZE * ARTWORK_SIZE * sizeof(lv_color_t);
    s_art_dsc.data = (const uint8_t *)s_art_pixels;

    StationPreset st = g_app.presets[g_app.current_preset];
    render_procedural_stream_art(s_station_cache_pixels, st);
    memcpy(s_art_pixels, s_station_cache_pixels, ARTWORK_SIZE * ARTWORK_SIZE * sizeof(lv_color_t));
    s_art_seq = 1;

    StackType_t *art_stack = (StackType_t *)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticTask_t *art_tcb  = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    xTaskCreateStaticPinnedToCore(artwork_worker_task, "art_fetch", 16384 / sizeof(StackType_t), NULL, 1, art_stack, art_tcb, 0);
}

const lv_img_dsc_t *artwork_get_dsc(void) {
    return &s_art_dsc;
}

uint32_t artwork_get_seq(void) {
    return s_art_seq;
}

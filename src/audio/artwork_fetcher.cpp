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
#include <atomic>
#include "network_diagnostics.h"

#define MAX_JPG_DIM 300
#define MAX_JPG_BYTES (64 * 1024)

static std::atomic<TaskHandle_t> s_art_task{nullptr};
static std::atomic<uint32_t> s_attempts{0}, s_failures{0}, s_tls_psram{0};
static std::atomic<int> s_http_status{0}, s_decode_error{0};
static std::atomic<uint8_t> s_art_kind{0};

// IDF's pinned build allocates mbedTLS buffers only in internal RAM. Restrict
// this override to the artwork task; other TLS/pairing allocations are unchanged.
// Espressif's normal free hook is heap_caps_free, valid for either memory kind.
extern "C" void *__real_esp_mbedtls_mem_calloc(size_t, size_t);
extern "C" void *__wrap_esp_mbedtls_mem_calloc(size_t n, size_t size) {
    if (n && size > SIZE_MAX / n) return nullptr;
    if (xTaskGetCurrentTaskHandle() == s_art_task.load() && n * size >= 1024) {
        void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) s_tls_psram.fetch_add(1);
        return p; // Optional artwork must not fall back into scarce driver RAM.
    }
    return __real_esp_mbedtls_mem_calloc(n, size);
}

ArtworkDiagnostics artwork_diagnostics() {
    return {s_attempts.load(), s_failures.load(), s_tls_psram.load(),
            s_http_status.load(), s_decode_error.load(), s_art_kind.load()};
}

static lv_color_t *s_art_pixels = NULL;
static lv_color_t *s_station_cache_pixels = NULL;
static lv_img_dsc_t s_art_dsc = {};
static volatile uint32_t s_art_seq = 0;

static uint8_t *s_station_jpg_buf = NULL;
static size_t s_station_jpg_len = 0;
static uint8_t *s_current_jpg_buf = NULL;
static size_t s_current_jpg_len = 0;
static volatile uint32_t s_current_jpg_seq = 0;
static SemaphoreHandle_t s_jpg_mutex = NULL;

static void store_current_jpeg(const uint8_t *data, size_t len, bool is_station_cover, bool publish = true) {
    if (!data || len == 0 || len > MAX_JPG_BYTES || !s_jpg_mutex) return;
    if (xSemaphoreTake(s_jpg_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (is_station_cover && s_station_jpg_buf) {
            memcpy(s_station_jpg_buf, data, len);
            s_station_jpg_len = len;
        }
        if (publish && s_current_jpg_buf) {
            memcpy(s_current_jpg_buf, data, len);
            s_current_jpg_len = len;
            s_current_jpg_seq = s_current_jpg_seq + 1;
        }
        xSemaphoreGive(s_jpg_mutex);
    }
}

static void restore_station_jpeg_as_current(void) {
    if (!s_jpg_mutex) return;
    if (xSemaphoreTake(s_jpg_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_station_jpg_buf && s_station_jpg_len > 0 && s_current_jpg_buf) {
            memcpy(s_current_jpg_buf, s_station_jpg_buf, s_station_jpg_len);
            s_current_jpg_len = s_station_jpg_len;
            s_current_jpg_seq = s_current_jpg_seq + 1;
        } else {
            s_current_jpg_len = 0;
            ++s_current_jpg_seq;
        }
        xSemaphoreGive(s_jpg_mutex);
    }
}

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
    s_decode_error.store((int)res);
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

    s_decode_error.store((int)res);
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

static bool artwork_network_ready(uint32_t cushion_ms) {
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;
    bool native_only = g_app.prefer_sonos_native && g_app.sonos_native &&
                       !g_app.also_local_dac && !g_app.whitecap_active;
    bool busy = !g_app.wifi_connected || g_app.ota_in_progress || g_app.speaker_scanning ||
                g_app.wifi_scanning || g_app.stream_reconfig_needed ||
                g_app.status == STREAM_CONNECTING_AIRPLAY;
    bool ok = artwork_network_allowed(g_app.playing, native_only, busy,
                                       radio_get_buffered_ms(), cushion_ms);
    xSemaphoreGive(g_app.mutex);
    return ok && ESP.getFreeHeap() >= 32000;
}

class ArtworkBufferStream : public Stream {
public:
    ArtworkBufferStream(uint8_t *data, size_t cap) : data(data), cap(cap) {}
    size_t write(const uint8_t *p, size_t n) override {
        if (n > cap - used || !artwork_network_ready(3000)) return 0;
        memcpy(data + used, p, n); used += n; return n;
    }
    size_t write(uint8_t c) override { return write(&c, 1); }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
    uint8_t *data; size_t cap, used = 0;
};

static bool download_url_to_buf(const char *url, uint8_t *buf, size_t max_len, size_t *out_len) {
    *out_len = 0;
    if (!artwork_network_ready(6000)) return false;
    ++s_attempts;
    bool secure = strncmp(url, "https://", 8) == 0;
    WiFiClient pc;
    WiFiClientSecure sc;
    sc.setInsecure();
    sc.setHandshakeTimeout(5);
    HTTPClient http;
    http.setUserAgent("AirPlayKnob/1.0");
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setConnectTimeout(2500); http.setTimeout(2500);
    bool begun = secure ? http.begin(sc, url) : http.begin(pc, url);
    bool ok = false;
    if (begun) {
        int code = http.GET(); s_http_status.store(code);
        if (code == HTTP_CODE_OK && http.getSize() <= (int)max_len) {
            // HTTPClient removes transfer-encoding framing; the raw stream
            // previously treated chunk headers as image/JSON bytes.
            ArtworkBufferStream sink(buf, max_len);
            int n = http.writeToStream(&sink);
            ok = n > 120 && sink.used == (size_t)n &&
                 (http.getSize() < 0 || sink.used == (size_t)http.getSize());
            if (ok) *out_len = sink.used;
        }
        http.end();
    }
    if (!ok) ++s_failures;
    return ok;
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
    s_art_task.store(xTaskGetCurrentTaskHandle());
    const size_t MAX_DL = MAX_JPG_BYTES;
    uint8_t *dl_buf = (uint8_t *)heap_caps_malloc(MAX_DL + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_color_t *work_pixels = (lv_color_t *)heap_caps_malloc(ARTWORK_SIZE * ARTWORK_SIZE * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dl_buf || !work_pixels) { heap_caps_free(dl_buf); heap_caps_free(work_pixels); vTaskDelete(nullptr); return; }
    int last_preset = -1, station_tries = 0, track_tries = 0;
    char last_track[128] = {0};
    bool station_loaded = false, track_done = false;
    uint32_t next_station_ms = 0, next_track_ms = 0;
    for (;;) {
        int preset = -1;
        StationPreset st = {};
        char track[128] = {0};
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(250)); continue;
        }
        preset = g_app.current_preset;
        if (preset >= 0 && preset < g_app.preset_count) st = g_app.presets[preset];
        strncpy(track, g_app.track_title, sizeof(track) - 1);
        xSemaphoreGive(g_app.mutex);
        if (preset != last_preset) {
            last_preset = preset; last_track[0] = 0;
            station_loaded = false; station_tries = 0; next_station_ms = 0;
            s_art_kind.store(0);
            render_procedural_stream_art(s_station_cache_pixels, st);
            memcpy(s_art_pixels, s_station_cache_pixels, ARTWORK_SIZE * ARTWORK_SIZE * sizeof(lv_color_t));
            ++s_art_seq;
            if (xSemaphoreTake(s_jpg_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                s_station_jpg_len = 0; s_current_jpg_len = 0;
                xSemaphoreGive(s_jpg_mutex);
            }
        }
        if (strcmp(track, last_track) != 0) {
            strncpy(last_track, track, sizeof(last_track) - 1);
            track_tries = 0; track_done = false; next_track_ms = 0;
            memcpy(s_art_pixels, s_station_cache_pixels, ARTWORK_SIZE * ARTWORK_SIZE * sizeof(lv_color_t));
            ++s_art_seq; restore_station_jpeg_as_current();
            s_art_kind.store(station_loaded ? 1 : 0);
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                snprintf(g_app.artwork_url, sizeof(g_app.artwork_url), "http://somafm.com/img/%s120.jpg", st.id);
                xSemaphoreGive(g_app.mutex);
            }
        }
        auto still_current = [&]() {
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
            bool same = g_app.current_preset == preset && strcmp(g_app.track_title, track) == 0;
            xSemaphoreGive(g_app.mutex); return same;
        };
        char station_url[128];
        snprintf(station_url, sizeof(station_url), "http://somafm.com/img/%s120.jpg", st.id);
        if (!station_loaded && station_tries < 3 && st.id[0] &&
            (int32_t)(millis() - next_station_ms) >= 0 && artwork_network_ready(6000)) {
            ++station_tries; next_station_ms = millis() + 15000;
            size_t n = 0;
            if (download_url_to_buf(station_url, dl_buf, MAX_DL, &n) &&
                decode_jpeg_to_artwork(dl_buf, n, work_pixels, st.color) && still_current()) {
                memcpy(s_station_cache_pixels, work_pixels, ARTWORK_SIZE * ARTWORK_SIZE * sizeof(lv_color_t));
                const bool album_active = s_art_kind.load() == 2;
                store_current_jpeg(dl_buf, n, true, !album_active);
                station_loaded = true;
                if (!album_active) {
                    memcpy(s_art_pixels, work_pixels, ARTWORK_SIZE * ARTWORK_SIZE * sizeof(lv_color_t));
                    ++s_art_seq; s_art_kind.store(1);
                }
                if (!album_active && xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    strncpy(g_app.artwork_url, station_url, sizeof(g_app.artwork_url) - 1);
                    xSemaphoreGive(g_app.mutex);
                }
            }
        }
        bool song = strstr(track, " - ") && !strstr(track, "Live Stream") &&
                    !strstr(track, "Connecting") && !strstr(track, "SomaFM");
        if (song && !track_done && track_tries < 3 &&
            (int32_t)(millis() - next_track_ms) >= 0 && artwork_network_ready(6000)) {
            ++track_tries; next_track_ms = millis() + 15000;
            char query[384], url[512];
            url_encode_query(track, query, sizeof(query));
            snprintf(url, sizeof(url), "https://itunes.apple.com/search?term=%s&media=music&entity=song&limit=1", query);
            size_t n = 0;
            if (download_url_to_buf(url, dl_buf, 16384, &n)) {
                dl_buf[n] = 0;
                char *key = strstr((char *)dl_buf, "\"artworkUrl100\":\"");
                if (!key) track_done = true; // Successful search, no matching album.
                else {
                    key += strlen("\"artworkUrl100\":\"");
                    char *end = strchr(key, '"');
                    if (end && end - key < 240) {
                        char art_url[256] = {0}; memcpy(art_url, key, end - key);
                        char *size = strstr(art_url, "100x100bb.jpg");
                        if (size) memcpy(size, "160x160bb.jpg", 13); // Baseline JPEG, supported by ROM decoder.
                        char download[256];
                        snprintf(download, sizeof(download), "%s", art_url);
                        if (strncmp(download, "https://", 8) == 0) memmove(download + 4, download + 5, strlen(download + 5) + 1);
                        if (download_url_to_buf(download, dl_buf, MAX_DL, &n) &&
                            decode_jpeg_to_artwork(dl_buf, n, work_pixels, st.color) && still_current()) {
                            memcpy(s_art_pixels, work_pixels, ARTWORK_SIZE * ARTWORK_SIZE * sizeof(lv_color_t));
                            ++s_art_seq; store_current_jpeg(dl_buf, n, false); s_art_kind.store(2);
                            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                                strncpy(g_app.artwork_url, art_url, sizeof(g_app.artwork_url) - 1);
                                xSemaphoreGive(g_app.mutex);
                            }
                            track_done = true;
                            Serial.printf("[ART] Album cover published (%u bytes)\n", (unsigned)n);
                        }
                    } else track_done = true;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void artwork_fetcher_start(void) {
    if (!s_jpg_mutex) {
        s_jpg_mutex = xSemaphoreCreateMutex();
    }
    if (!s_station_jpg_buf) {
        s_station_jpg_buf = (uint8_t *)heap_caps_malloc(MAX_JPG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_current_jpg_buf) {
        s_current_jpg_buf = (uint8_t *)heap_caps_malloc(MAX_JPG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
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

uint32_t artwork_get_jpeg_seq(void) {
    return s_current_jpg_seq;
}

size_t artwork_copy_jpeg(uint8_t *dst, size_t max_len, uint32_t *seq_out) {
    if (!dst || max_len == 0 || !s_jpg_mutex) return 0;
    size_t copied = 0;
    if (xSemaphoreTake(s_jpg_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_current_jpg_buf && s_current_jpg_len > 0 && s_current_jpg_len <= max_len) {
            memcpy(dst, s_current_jpg_buf, s_current_jpg_len);
            copied = s_current_jpg_len;
            if (seq_out) *seq_out = s_current_jpg_seq;
        }
        xSemaphoreGive(s_jpg_mutex);
    }
    return copied;
}

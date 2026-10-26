#include "knob_ui.h"
#include "app_state.h"
#include "artwork_fetcher.h"
#include "bsp/lcd_bsp.h"
#include "bsp/lcd_config.h"
#include "bsp/cst816.h"
#include <Arduino.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <math.h>

enum ActiveTab {
    TAB_NOW_PLAYING = 0,
    TAB_STATIONS    = 1,
    TAB_SPEAKERS    = 2,
    TAB_WIFI        = 3,
};

static ActiveTab s_active_tab = TAB_NOW_PLAYING;

// Root & Perimeter Ring
static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_outer_arc = NULL;
static lv_obj_t *s_pages[4] = {NULL, NULL, NULL, NULL};

// Tab 0: Minimalist Now Playing (Artwork + Track Name Focus)
static lv_obj_t *s_art_cont = NULL;
static lv_obj_t *s_art_img = NULL;
static lv_obj_t *s_pause_badge = NULL;
static lv_obj_t *s_lbl_track = NULL;
static lv_obj_t *s_lbl_artist = NULL;
static lv_obj_t *s_lbl_sub_meta = NULL;
static lv_obj_t *s_btn_dock_stations = NULL;
static lv_obj_t *s_btn_dock_whitecap = NULL;
static lv_obj_t *s_btn_dock_speakers = NULL;

// Tab 1: Minimalist Stations Sheet
static lv_obj_t *s_station_list = NULL;
static lv_obj_t *s_station_items[MAX_PRESETS] = {NULL};
static int s_station_cursor = 0;
static int s_station_rendered_count = 0;

// Tab 2: Minimalist AirPlay Speakers Sheet
static lv_obj_t *s_speaker_list = NULL;
static lv_obj_t *s_speaker_items[MAX_SPEAKERS] = {NULL};
static lv_obj_t *s_lbl_spkr_hdr = NULL;
static int s_speaker_cursor = 0;
static int s_speaker_rendered_count = 0;

// Tab 3: Wi-Fi, Web Portal QR & OTA Info Sheet
static lv_obj_t *s_lbl_wifi_ssid = NULL;
static lv_obj_t *s_lbl_wifi_ip = NULL;
static lv_obj_t *s_qr_code = NULL;
static lv_obj_t *s_wifi_modal = NULL;
static lv_obj_t *s_wifi_list = NULL;
static lv_obj_t *s_wifi_pass_ta = NULL;
static lv_obj_t *s_wifi_kb = NULL;
static char s_pending_ssid[36] = {0};

// Minimalist Floating HUD Pill (Volume / Tuner / OTA)
static lv_obj_t *s_hud_pill = NULL;
static lv_obj_t *s_lbl_hud_title = NULL;
static lv_obj_t *s_lbl_hud_val = NULL;
static uint32_t s_hud_hide_ms = 0;

// =============================================================================
// Full-Screen Old-School "WhiteCap" Visualizer (360x360 Framebuffer in PSRAM)
// =============================================================================
#define WC_W 360
#define WC_H 360

static lv_color_t *s_wc_fb = NULL;
static lv_img_dsc_t s_wc_dsc = {};
static lv_obj_t *s_wc_layer = NULL;
static lv_obj_t *s_wc_img = NULL;
static lv_obj_t *s_wc_caption_box = NULL;
static lv_obj_t *s_wc_lbl_track = NULL;
static lv_obj_t *s_wc_lbl_sub = NULL;
static uint32_t s_wc_caption_hide_ms = 0;
static uint32_t s_wc_last_frame_ms = 0;
static uint32_t s_wc_last_morph_ms = 0;
static float s_wc_phase = 0.0f;

static const char *WHITECAP_PRESET_NAMES[4] = {
    "WhiteCap • 3D Ribbon Tunnel",
    "WhiteCap • Harmonic Coriolis",
    "WhiteCap • Aether Horizon",
    "WhiteCap • Supernova Halo"
};

static lv_color_t hex_to_lv(uint32_t hex) {
    return lv_color_make((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF);
}

static void show_hud(const char *label, const char *value, uint32_t color_hex, uint32_t duration_ms) {
    if (!s_hud_pill) return;
    lv_label_set_text(s_lbl_hud_title, label);
    lv_label_set_text(s_lbl_hud_val, value);
    lv_obj_set_style_border_color(s_hud_pill, hex_to_lv(color_hex), 0);
    lv_obj_set_style_text_color(s_lbl_hud_title, hex_to_lv(color_hex), 0);
    lv_obj_clear_flag(s_hud_pill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_hud_pill);
    s_hud_hide_ms = millis() + duration_ms;
}

static void show_whitecap_caption(const char *track, const char *sub, uint32_t duration_ms) {
    if (!s_wc_caption_box) return;
    lv_label_set_text(s_wc_lbl_track, track);
    lv_label_set_text(s_wc_lbl_sub, sub);
    lv_obj_clear_flag(s_wc_caption_box, LV_OBJ_FLAG_HIDDEN);
    s_wc_caption_hide_ms = millis() + duration_ms;
}

// Fast phosphor trail decay on 360x360 RGB565 (byte-swapped LV_COLOR_16_SWAP=1) framebuffer
static void wc_fade_frame(void) {
    if (!s_wc_fb) return;
    uint16_t *p = (uint16_t *)s_wc_fb;
    const int count = WC_W * WC_H;
    for (int i = 0; i < count; i++) {
        uint16_t sw = p[i];
        if (sw == 0) continue;
        // Unswap to standard RGB565, scale by 3/4 (subtract 1/4), reswap
        uint16_t c = (sw >> 8) | (sw << 8);
        uint16_t q = (c & 0xE79C) >> 2;
        c -= q;
        if ((c & 0xF800) <= 0x0800) c &= ~0xF800;
        if ((c & 0x07E0) <= 0x0040) c &= ~0x07E0;
        if ((c & 0x001F) <= 0x0001) c &= ~0x001F;
        p[i] = (c >> 8) | (c << 8);
    }
}

static inline void wc_put_px(int x, int y, lv_color_t col) {
    if ((unsigned)x < WC_W && (unsigned)y < WC_H) {
        s_wc_fb[y * WC_W + x] = col;
    }
}

// Draws a glowing wireframe line (core + soft 1px halo) for authentic WhiteCap look
static void wc_draw_line(int x0, int y0, int x1, int y1, lv_color_t core_col, lv_color_t glow_col) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int max_steps = 512;

    while (max_steps-- > 0) {
        if ((unsigned)x0 < WC_W && (unsigned)y0 < WC_H) {
            s_wc_fb[y0 * WC_W + x0] = core_col;
            if (x0 + 1 < WC_W) s_wc_fb[y0 * WC_W + x0 + 1] = glow_col;
            if (y0 + 1 < WC_H) s_wc_fb[(y0 + 1) * WC_W + x0] = glow_col;
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// HSV (h: 0..360, s: 0..1, v: 0..1) -> lv_color_t
static lv_color_t wc_hsv(float h, float s, float v) {
    h = fmodf(h, 360.0f);
    if (h < 0) h += 360.0f;
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r = 0, g = 0, b = 0;
    if (h < 60)       { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else              { r = c; g = 0; b = x; }
    return lv_color_make((uint8_t)((r + m) * 255.0f),
                         (uint8_t)((g + m) * 255.0f),
                         (uint8_t)((b + m) * 255.0f));
}

static void render_whitecap_frame(const int8_t *wl, const int8_t *wr, const uint8_t *vu, uint8_t peak, int preset) {
    if (!s_wc_fb) return;

    wc_fade_frame();

    float energy = (float)peak / 100.0f;
    if (energy < 0.12f) energy = 0.12f;
    s_wc_phase += 0.045f + energy * 0.055f;

    float base_hue = fmodf(s_wc_phase * 18.0f + preset * 75.0f, 360.0f);
    const float cx = 180.0f;
    const float cy = 180.0f;

    if (preset == 0) {
        // =====================================================================
        // PRESET 0: WhiteCap Classic 3D Ribbon Tunnel
        // Receding perspective wave-ribbons with cross-ribs & roll rotation
        // =====================================================================
        const int NUM_RIBBONS = 6;
        const int PTS = 36;
        int prev_rx[PTS], prev_ry[PTS];
        bool has_prev = false;

        float roll = sinf(s_wc_phase * 0.35f) * 0.42f;
        float c_roll = cosf(roll);
        float s_roll = sinf(roll);

        for (int r = 0; r < NUM_RIBBONS; r++) {
            float z = 1.0f + (float)r * 0.38f - fmodf(s_wc_phase * 0.3f, 0.38f);
            if (z < 0.75f) z = 0.75f;
            float persp = 1.35f / z;
            float y_plane = ((r % 2 == 0) ? 1.0f : -1.0f) * (34.0f + energy * 18.0f);

            lv_color_t col_core = wc_hsv(base_hue + r * 28.0f, 0.75f, fminf(1.0f, 1.15f / z));
            lv_color_t col_glow = wc_hsv(base_hue + r * 28.0f, 0.85f, fminf(0.55f, 0.65f / z));

            int cur_rx[PTS], cur_ry[PTS];
            for (int i = 0; i < PTS; i++) {
                float u = ((float)i / (float)(PTS - 1)) * 2.0f - 1.0f; // -1..1
                int w_idx = (i * 127) / (PTS - 1);
                float sample = (r & 1) ? (wr[w_idx] / 127.0f) : (wl[w_idx] / 127.0f);
                float wave = sample * (48.0f + energy * 42.0f) +
                             14.0f * sinf(u * 4.5f + s_wc_phase * 1.4f + r * 0.7f);

                float x3 = u * 165.0f;
                float y3 = y_plane + wave;

                float rx = (x3 * c_roll - y3 * s_roll) * persp + cx;
                float ry = (x3 * s_roll + y3 * c_roll) * persp + cy;
                cur_rx[i] = (int)rx;
                cur_ry[i] = (int)ry;

                if (i > 0) {
                    wc_draw_line(cur_rx[i - 1], cur_ry[i - 1], cur_rx[i], cur_ry[i], col_core, col_glow);
                }
                if (has_prev && (i % 4 == 0)) {
                    wc_draw_line(prev_rx[i], prev_ry[i], cur_rx[i], cur_ry[i], col_glow, col_glow);
                }
            }
            memcpy(prev_rx, cur_rx, sizeof(cur_rx));
            memcpy(prev_ry, cur_ry, sizeof(cur_ry));
            has_prev = true;
        }
    } else if (preset == 1) {
        // =====================================================================
        // PRESET 1: Harmonic Coriolis (Stereo Lissajous Spirograph Web)
        // =====================================================================
        const int RINGS = 3;
        const int N = 72;
        for (int ring = 0; ring < RINGS; ring++) {
            float rad_base = 58.0f + ring * 38.0f + (vu[ring * 3] * 0.25f);
            float f1 = 2.0f + ring;
            float f2 = 3.0f + ring * 2.0f;
            float rot = s_wc_phase * (0.4f + ring * 0.2f) * ((ring & 1) ? -1.0f : 1.0f);

            lv_color_t col_core = wc_hsv(base_hue + ring * 65.0f, 0.7f, 0.95f);
            lv_color_t col_glow = wc_hsv(base_hue + ring * 65.0f + 20.0f, 0.85f, 0.45f);

            int first_x = 0, first_y = 0, px = 0, py = 0;
            for (int i = 0; i <= N; i++) {
                int idx = (i * 127) / N;
                float ang = ((float)i / (float)N) * 6.2831853f;
                float audio_mod = (wl[idx] * 0.35f + wr[idx] * 0.35f);
                float r = rad_base + audio_mod +
                          22.0f * sinf(ang * f1 + s_wc_phase) * cosf(ang * f2 - rot);
                int x = (int)(cx + r * cosf(ang + rot));
                int y = (int)(cy + r * sinf(ang + rot));
                if (i == 0) {
                    first_x = x; first_y = y;
                } else {
                    wc_draw_line(px, py, x, y, col_core, col_glow);
                }
                px = x; py = y;
            }
            wc_draw_line(px, py, first_x, first_y, col_core, col_glow);
        }
    } else if (preset == 2) {
        // =====================================================================
        // PRESET 2: Aether Horizon (Dual 3D Stereo Wave Terrain + Core Sun)
        // =====================================================================
        const int ROWS = 5;
        const int COLS = 32;
        for (int side = -1; side <= 1; side += 2) {
            int prev_x[COLS], prev_y[COLS];
            for (int r = 0; r < ROWS; r++) {
                float depth = 1.0f + r * 0.42f;
                float y_base = cy + side * (28.0f + r * 24.0f);
                float x_span = 165.0f / (0.65f + r * 0.18f);

                lv_color_t col_core = wc_hsv(base_hue + (side > 0 ? 0 : 140) + r * 18.0f, 0.75f, 1.0f / (0.8f + r * 0.2f));
                lv_color_t col_glow = wc_hsv(base_hue + (side > 0 ? 0 : 140) + r * 18.0f, 0.85f, 0.42f);

                int cur_x[COLS], cur_y[COLS];
                for (int c = 0; c < COLS; c++) {
                    float u = ((float)c / (float)(COLS - 1)) * 2.0f - 1.0f;
                    int w_idx = (c * 127) / (COLS - 1);
                    float s_val = (side < 0 ? wl[w_idx] : wr[w_idx]) / 127.0f;
                    float dy = s_val * (38.0f / depth) +
                               10.0f * cosf(u * 5.0f + s_wc_phase * 1.6f + r);
                    cur_x[c] = (int)(cx + u * x_span);
                    cur_y[c] = (int)(y_base + dy * side);

                    if (c > 0) {
                        wc_draw_line(cur_x[c - 1], cur_y[c - 1], cur_x[c], cur_y[c], col_core, col_glow);
                    }
                    if (r > 0 && (c % 3 == 0)) {
                        wc_draw_line(prev_x[c], prev_y[c], cur_x[c], cur_y[c], col_glow, col_glow);
                    }
                }
                memcpy(prev_x, cur_x, sizeof(cur_x));
                memcpy(prev_y, cur_y, sizeof(cur_y));
            }
        }
    } else {
        // =====================================================================
        // PRESET 3: Supernova Halo (Radial Oscilloscope + Velocity Vectors)
        // =====================================================================
        const int N = 96;
        lv_color_t col_ring = wc_hsv(base_hue, 0.65f, 1.0f);
        lv_color_t col_ray  = wc_hsv(base_hue + 45.0f, 0.85f, 0.85f);
        lv_color_t col_glow = wc_hsv(base_hue + 90.0f, 0.9f, 0.45f);

        int fx = 0, fy = 0, px = 0, py = 0;
        float base_r = 78.0f + energy * 24.0f;
        for (int i = 0; i < N; i++) {
            int idx = (i * 127) / N;
            float ang = ((float)i / (float)N) * 6.2831853f + s_wc_phase * 0.25f;
            float sample = (wl[idx] + wr[idx]) * 0.38f;
            float r = base_r + sample;
            int x = (int)(cx + r * cosf(ang));
            int y = (int)(cy + r * sinf(ang));
            if (i == 0) { fx = x; fy = y; }
            else { wc_draw_line(px, py, x, y, col_ring, col_glow); }
            px = x; py = y;

            // Outward velocity spikes on peaks
            if ((i % 3 == 0) && fabsf(sample) > 12.0f) {
                float r_out = r + fabsf(sample) * 0.85f;
                int ox = (int)(cx + r_out * cosf(ang));
                int oy = (int)(cy + r_out * sinf(ang));
                wc_draw_line(x, y, ox, oy, col_ray, col_glow);
            }
        }
        wc_draw_line(px, py, fx, fy, col_ring, col_glow);
    }
}

// Splits ICY "Artist - Track Title" into clean separate strings
static void split_artist_track(const char *raw, const char *fallback_station,
                               char *artist_out, size_t artist_max,
                               char *track_out, size_t track_max) {
    if (!raw || raw[0] == '\0') {
        strncpy(track_out, fallback_station, track_max - 1);
        strncpy(artist_out, "SomaFM Radio", artist_max - 1);
        return;
    }
    const char *sep = strstr(raw, " - ");
    if (sep && sep > raw) {
        size_t alen = (size_t)(sep - raw);
        if (alen >= artist_max) alen = artist_max - 1;
        memcpy(artist_out, raw, alen);
        artist_out[alen] = '\0';

        strncpy(track_out, sep + 3, track_max - 1);
        track_out[track_max - 1] = '\0';
    } else {
        strncpy(track_out, raw, track_max - 1);
        track_out[track_max - 1] = '\0';
        strncpy(artist_out, fallback_station, artist_max - 1);
        artist_out[artist_max - 1] = '\0';
    }
}

static void switch_tab(ActiveTab tab) {
    s_active_tab = tab;
    for (int i = 0; i < 4; i++) {
        if (!s_pages[i]) continue;
        if (i == (int)tab) lv_obj_clear_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    haptic_play_effect(HAPTIC_SHARP_CLICK);
}

static void set_whitecap_visible(bool visible) {
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        g_app.whitecap_active = visible;
        xSemaphoreGive(g_app.mutex);
    }
    if (!s_wc_layer) return;
    if (visible) {
        if (s_wc_fb) memset(s_wc_fb, 0, WC_W * WC_H * sizeof(lv_color_t));
        lv_obj_clear_flag(s_wc_layer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_wc_layer);
        int preset = g_app.whitecap_preset & 3;
        show_whitecap_caption(lv_label_get_text(s_lbl_track), WHITECAP_PRESET_NAMES[preset], 4200);
        s_wc_last_morph_ms = millis();
        haptic_play_effect(HAPTIC_DOUBLE_CLICK);
    } else {
        lv_obj_add_flag(s_wc_layer, LV_OBJ_FLAG_HIDDEN);
        haptic_play_effect(HAPTIC_SHARP_CLICK);
    }
}

static void rebuild_station_list(void) {
    if (!s_station_list) return;
    lv_obj_clean(s_station_list);
    memset(s_station_items, 0, sizeof(s_station_items));

    int count = 0;
    int cur_idx = 0;
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        count = g_app.preset_count;
        cur_idx = g_app.current_preset;
        s_station_cursor = cur_idx;

        for (int i = 0; i < count; i++) {
            const StationPreset &st = g_app.presets[i];
            lv_obj_t *btn = lv_btn_create(s_station_list);
            lv_obj_set_size(btn, 276, 48);
            lv_obj_set_style_radius(btn, 14, 0);
            lv_obj_set_style_bg_color(btn, (i == cur_idx) ? hex_to_lv(0x161B26) : hex_to_lv(0x0B0E14), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(btn, (i == cur_idx) ? 2 : 1, 0);
            lv_obj_set_style_border_color(btn, (i == cur_idx) ? hex_to_lv(st.color) : hex_to_lv(0x1E293B), 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);

            lv_obj_t *dot = lv_obj_create(btn);
            lv_obj_set_size(dot, 8, 24);
            lv_obj_set_style_radius(dot, 4, 0);
            lv_obj_set_style_bg_color(dot, hex_to_lv(st.color), 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_align(dot, LV_ALIGN_LEFT_MID, -4, 0);

            lv_obj_t *title = lv_label_create(btn);
            lv_label_set_text(title, st.name);
            lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(title, hex_to_lv(0xF8FAFC), 0);
            lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 2);

            lv_obj_t *genre = lv_label_create(btn);
            lv_label_set_text(genre, st.genre);
            lv_obj_set_style_text_font(genre, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(genre, hex_to_lv(0x64748B), 0);
            lv_obj_align(genre, LV_ALIGN_BOTTOM_LEFT, 12, -1);

            lv_obj_add_event_cb(btn, [](lv_event_t *e) {
                int idx = (int)(intptr_t)lv_event_get_user_data(e);
                haptic_play_effect(HAPTIC_DOUBLE_CLICK);
                app_select_preset(idx, true);
                switch_tab(TAB_NOW_PLAYING);
            }, LV_EVENT_CLICKED, (void *)(intptr_t)i);

            s_station_items[i] = btn;
        }
        xSemaphoreGive(g_app.mutex);
    }
    s_station_rendered_count = count;
    if (cur_idx >= 0 && cur_idx < count && s_station_items[cur_idx]) {
        lv_obj_scroll_to_view(s_station_items[cur_idx], LV_ANIM_OFF);
    }
}

static void rebuild_speaker_list(void) {
    if (!s_speaker_list) return;
    lv_obj_clean(s_speaker_list);
    memset(s_speaker_items, 0, sizeof(s_speaker_items));

    int count = 0;
    int cur_idx = 0;
    bool scanning = false;
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        count = g_app.speaker_count;
        cur_idx = g_app.current_speaker;
        scanning = g_app.speaker_scanning;
        s_speaker_cursor = cur_idx;

        if (s_lbl_spkr_hdr) {
            lv_label_set_text(s_lbl_spkr_hdr, scanning ? "SCANNING AIRPLAY..." : "AIRPLAY SPEAKERS");
        }

        for (int i = 0; i < count; i++) {
            const AirPlaySpeaker &sp = g_app.speakers[i];
            lv_obj_t *btn = lv_btn_create(s_speaker_list);
            lv_obj_set_size(btn, 276, 48);
            lv_obj_set_style_radius(btn, 14, 0);
            lv_obj_set_style_bg_color(btn, (i == cur_idx) ? hex_to_lv(0x161B26) : hex_to_lv(0x0B0E14), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(btn, (i == cur_idx) ? 2 : 1, 0);
            lv_obj_set_style_border_color(btn, (i == cur_idx) ? hex_to_lv(0x38BDF8) : hex_to_lv(0x1E293B), 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);

            lv_obj_t *title = lv_label_create(btn);
            lv_label_set_text_fmt(title, "%s  %s", LV_SYMBOL_AUDIO, sp.name);
            lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(title, hex_to_lv(0xF8FAFC), 0);
            lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 2);

            lv_obj_t *sub = lv_label_create(btn);
            if (sp.is_local_dac) {
                lv_label_set_text(sub, "3.5mm Stereo Line Out");
            } else {
                lv_label_set_text_fmt(sub, "%s  •  AirPlay", sp.ip);
            }
            lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(sub, hex_to_lv(0x64748B), 0);
            lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 4, -1);

            lv_obj_add_event_cb(btn, [](lv_event_t *e) {
                int idx = (int)(intptr_t)lv_event_get_user_data(e);
                haptic_play_effect(HAPTIC_DOUBLE_CLICK);
                app_select_speaker(idx);
                switch_tab(TAB_NOW_PLAYING);
            }, LV_EVENT_CLICKED, (void *)(intptr_t)i);

            s_speaker_items[i] = btn;
        }
        xSemaphoreGive(g_app.mutex);
    }
    s_speaker_rendered_count = count;
}

static void wifi_kb_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        const char *pwd = lv_textarea_get_text(s_wifi_pass_ta);
        haptic_play_effect(HAPTIC_DOUBLE_CLICK);
        app_state_save_wifi(s_pending_ssid, pwd ? pwd : "");
        if (s_wifi_modal) {
            lv_obj_del(s_wifi_modal);
            s_wifi_modal = NULL;
            s_wifi_list = NULL;
        }
    } else if (code == LV_EVENT_CANCEL) {
        if (s_wifi_modal) {
            lv_obj_del(s_wifi_modal);
            s_wifi_modal = NULL;
            s_wifi_list = NULL;
        }
    }
}

static void wifi_ssid_click_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (idx >= 0 && idx < g_app.wifi_scan_count) {
            strncpy(s_pending_ssid, g_app.wifi_scan[idx].ssid, sizeof(s_pending_ssid) - 1);
        }
        xSemaphoreGive(g_app.mutex);
    }
    if (!s_wifi_modal) return;
    lv_obj_clean(s_wifi_modal);

    lv_obj_t *lbl = lv_label_create(s_wifi_modal);
    lv_label_set_text_fmt(lbl, "Join %s", s_pending_ssid);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, hex_to_lv(0xF8FAFC), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 26);

    s_wifi_pass_ta = lv_textarea_create(s_wifi_modal);
    lv_textarea_set_one_line(s_wifi_pass_ta, true);
    lv_textarea_set_password_mode(s_wifi_pass_ta, false);
    lv_textarea_set_placeholder_text(s_wifi_pass_ta, "Wi-Fi Password");
    lv_obj_set_size(s_wifi_pass_ta, 240, 38);
    lv_obj_align(s_wifi_pass_ta, LV_ALIGN_TOP_MID, 0, 52);

    s_wifi_kb = lv_keyboard_create(s_wifi_modal);
    lv_obj_set_size(s_wifi_kb, 316, 165);
    lv_obj_align(s_wifi_kb, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_keyboard_set_textarea(s_wifi_kb, s_wifi_pass_ta);
    lv_obj_add_event_cb(s_wifi_kb, wifi_kb_event_cb, LV_EVENT_ALL, NULL);
}

static void open_wifi_scan_modal(lv_event_t *e) {
    (void)e;
    haptic_play_effect(HAPTIC_SHARP_CLICK);
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_app.wifi_scan_requested = true;
        xSemaphoreGive(g_app.mutex);
    }

    if (s_wifi_modal) lv_obj_del(s_wifi_modal);
    s_wifi_modal = lv_obj_create(s_scr);
    lv_obj_set_size(s_wifi_modal, 360, 360);
    lv_obj_center(s_wifi_modal);
    lv_obj_set_style_radius(s_wifi_modal, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_wifi_modal, hex_to_lv(0x050608), 0);
    lv_obj_set_style_border_width(s_wifi_modal, 0, 0);

    lv_obj_t *hdr = lv_label_create(s_wifi_modal);
    lv_label_set_text(hdr, "Select Wi-Fi Network");
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hdr, hex_to_lv(0x94A3B8), 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 26);

    s_wifi_list = lv_obj_create(s_wifi_modal);
    lv_obj_set_size(s_wifi_list, 276, 210);
    lv_obj_align(s_wifi_list, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(s_wifi_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wifi_list, 0, 0);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_wifi_list, 6, 0);

    lv_obj_t *scanning_lbl = lv_label_create(s_wifi_list);
    lv_label_set_text(scanning_lbl, "Scanning networks...");
    lv_obj_set_style_text_color(scanning_lbl, hex_to_lv(0x64748B), 0);

    lv_obj_t *close_btn = lv_btn_create(s_wifi_modal);
    lv_obj_set_size(close_btn, 96, 32);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_style_radius(close_btn, 16, 0);
    lv_obj_set_style_bg_color(close_btn, hex_to_lv(0x1E293B), 0);
    lv_obj_t *cl = lv_label_create(close_btn);
    lv_label_set_text(cl, "Close");
    lv_obj_center(cl);
    lv_obj_add_event_cb(close_btn, [](lv_event_t *ev) {
        (void)ev;
        if (s_wifi_modal) {
            lv_obj_del(s_wifi_modal);
            s_wifi_modal = NULL;
            s_wifi_list = NULL;
        }
    }, LV_EVENT_CLICKED, NULL);
}

void knob_ui_init(void) {
    if (!bsp_lvgl_lock(-1)) return;

    s_scr = lv_scr_act();
    lv_obj_set_style_bg_color(s_scr, hex_to_lv(0x050608), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Subtle 3px 360-degree bezel progress ring
    s_outer_arc = lv_arc_create(s_scr);
    lv_obj_set_size(s_outer_arc, 354, 354);
    lv_obj_center(s_outer_arc);
    lv_arc_set_rotation(s_outer_arc, 270);
    lv_arc_set_bg_angles(s_outer_arc, 0, 360);
    lv_arc_set_range(s_outer_arc, 0, 100);
    lv_arc_set_value(s_outer_arc, 65);
    lv_obj_remove_style(s_outer_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_outer_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_outer_arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_outer_arc, hex_to_lv(0x111827), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_outer_arc, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_outer_arc, hex_to_lv(0x10B981), LV_PART_INDICATOR);

    for (int i = 0; i < 4; i++) {
        s_pages[i] = lv_obj_create(s_scr);
        lv_obj_set_size(s_pages[i], 344, 344);
        lv_obj_center(s_pages[i]);
        lv_obj_set_style_bg_opa(s_pages[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_pages[i], 0, 0);
        lv_obj_set_style_pad_all(s_pages[i], 0, 0);
        lv_obj_clear_flag(s_pages[i], LV_OBJ_FLAG_SCROLLABLE);
        if (i != 0) lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }

    // =========================================================================
    // PAGE 0: MINIMALIST NOW PLAYING (Artwork + Track Name Focus)
    // =========================================================================
    lv_obj_t *p0 = s_pages[0];

    // Centerpiece 156x156 Album Art / Stream Art Card
    s_art_cont = lv_obj_create(p0);
    lv_obj_set_size(s_art_cont, ARTWORK_SIZE, ARTWORK_SIZE);
    lv_obj_align(s_art_cont, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_set_style_radius(s_art_cont, 22, 0);
    lv_obj_set_style_bg_opa(s_art_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_art_cont, 0, 0);
    lv_obj_set_style_pad_all(s_art_cont, 0, 0);
    lv_obj_set_style_shadow_width(s_art_cont, 28, 0);
    lv_obj_set_style_shadow_color(s_art_cont, hex_to_lv(0x10B981), 0);
    lv_obj_set_style_shadow_opa(s_art_cont, LV_OPA_30, 0);
    lv_obj_clear_flag(s_art_cont, LV_OBJ_FLAG_SCROLLABLE);

    s_art_img = lv_img_create(s_art_cont);
    lv_img_set_src(s_art_img, artwork_get_dsc());
    lv_obj_center(s_art_img);
    lv_obj_add_flag(s_art_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_art_img, [](lv_event_t *e) {
        (void)e;
        haptic_play_effect(HAPTIC_STRONG_CLICK);
        app_toggle_play();
    }, LV_EVENT_CLICKED, NULL);

    // Subtle frosted pause indicator on artwork when paused
    s_pause_badge = lv_obj_create(s_art_cont);
    lv_obj_set_size(s_pause_badge, 44, 44);
    lv_obj_center(s_pause_badge);
    lv_obj_set_style_radius(s_pause_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_pause_badge, hex_to_lv(0x050608), 0);
    lv_obj_set_style_bg_opa(s_pause_badge, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_pause_badge, 1, 0);
    lv_obj_set_style_border_color(s_pause_badge, hex_to_lv(0xF8FAFC), 0);
    lv_obj_clear_flag(s_pause_badge, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *pl = lv_label_create(s_pause_badge);
    lv_label_set_text(pl, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(pl, hex_to_lv(0xF8FAFC), 0);
    lv_obj_center(pl);
    lv_obj_add_flag(s_pause_badge, LV_OBJ_FLAG_HIDDEN);

    // Primary Focus: Track Name (Clean bold white)
    s_lbl_track = lv_label_create(p0);
    lv_label_set_text(s_lbl_track, "Groove Salad");
    lv_obj_set_style_text_font(s_lbl_track, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_track, hex_to_lv(0xFFFFFF), 0);
    lv_label_set_long_mode(s_lbl_track, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_lbl_track, 286);
    lv_obj_set_style_text_align(s_lbl_track, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_track, LV_ALIGN_TOP_MID, 0, 184);

    // Secondary Focus: Artist Name
    s_lbl_artist = lv_label_create(p0);
    lv_label_set_text(s_lbl_artist, "SomaFM Radio");
    lv_obj_set_style_text_font(s_lbl_artist, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_artist, hex_to_lv(0x94A3B8), 0);
    lv_label_set_long_mode(s_lbl_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_lbl_artist, 260);
    lv_obj_set_style_text_align(s_lbl_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_artist, LV_ALIGN_TOP_MID, 0, 212);

    // Subtle Station • Speaker status line (Tap to cycle Knob Mode VOL -> TUNE -> SPKR)
    lv_obj_t *sub_btn = lv_btn_create(p0);
    lv_obj_set_size(sub_btn, 250, 24);
    lv_obj_align(sub_btn, LV_ALIGN_TOP_MID, 0, 236);
    lv_obj_set_style_bg_opa(sub_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sub_btn, 0, 0);
    lv_obj_set_style_shadow_width(sub_btn, 0, 0);
    lv_obj_set_style_pad_all(sub_btn, 0, 0);
    lv_obj_add_event_cb(sub_btn, [](lv_event_t *e) {
        (void)e;
        KnobMode next = KNOB_MODE_VOLUME;
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            next = (KnobMode)(((int)g_app.knob_mode + 1) % 3);
            g_app.knob_mode = next;
            g_app.preview_preset = g_app.current_preset;
            g_app.preview_speaker = g_app.current_speaker;
            xSemaphoreGive(g_app.mutex);
        }
        haptic_play_effect(HAPTIC_SHARP_CLICK);
        if (next == KNOB_MODE_VOLUME)       show_hud("KNOB MODE", "Volume", 0x10B981, 1000);
        else if (next == KNOB_MODE_STATION) show_hud("KNOB MODE", "Station Dial", 0xF59E0B, 1000);
        else                                show_hud("KNOB MODE", "Speaker Dial", 0x38BDF8, 1000);
    }, LV_EVENT_CLICKED, NULL);

    s_lbl_sub_meta = lv_label_create(sub_btn);
    lv_label_set_text(s_lbl_sub_meta, "GROOVE SALAD  •  AIRPLAY");
    lv_obj_set_style_text_font(s_lbl_sub_meta, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_sub_meta, hex_to_lv(0x10B981), 0);
    lv_label_set_long_mode(s_lbl_sub_meta, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_lbl_sub_meta, 244);
    lv_obj_set_style_text_align(s_lbl_sub_meta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_lbl_sub_meta);

    // Minimalist Bottom Action Dock (Stations  |  WhiteCap  |  AirPlay)
    s_btn_dock_stations = lv_btn_create(p0);
    lv_obj_set_size(s_btn_dock_stations, 78, 34);
    lv_obj_align(s_btn_dock_stations, LV_ALIGN_BOTTOM_MID, -88, -32);
    lv_obj_set_style_radius(s_btn_dock_stations, 17, 0);
    lv_obj_set_style_bg_color(s_btn_dock_stations, hex_to_lv(0x0F141D), 0);
    lv_obj_set_style_border_width(s_btn_dock_stations, 1, 0);
    lv_obj_set_style_border_color(s_btn_dock_stations, hex_to_lv(0x1E293B), 0);
    lv_obj_set_style_shadow_width(s_btn_dock_stations, 0, 0);
    lv_obj_t *l_st = lv_label_create(s_btn_dock_stations);
    lv_label_set_text(l_st, "Stations");
    lv_obj_set_style_text_font(l_st, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l_st, hex_to_lv(0xCBD5E1), 0);
    lv_obj_center(l_st);
    lv_obj_add_event_cb(s_btn_dock_stations, [](lv_event_t *e) {
        (void)e;
        rebuild_station_list();
        switch_tab(TAB_STATIONS);
    }, LV_EVENT_CLICKED, NULL);

    s_btn_dock_whitecap = lv_btn_create(p0);
    lv_obj_set_size(s_btn_dock_whitecap, 88, 34);
    lv_obj_align(s_btn_dock_whitecap, LV_ALIGN_BOTTOM_MID, 0, -32);
    lv_obj_set_style_radius(s_btn_dock_whitecap, 17, 0);
    lv_obj_set_style_bg_color(s_btn_dock_whitecap, hex_to_lv(0x131C2E), 0);
    lv_obj_set_style_border_width(s_btn_dock_whitecap, 1, 0);
    lv_obj_set_style_border_color(s_btn_dock_whitecap, hex_to_lv(0x334155), 0);
    lv_obj_set_style_shadow_width(s_btn_dock_whitecap, 0, 0);
    lv_obj_t *l_wc = lv_label_create(s_btn_dock_whitecap);
    lv_label_set_text(l_wc, "WhiteCap");
    lv_obj_set_style_text_font(l_wc, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l_wc, hex_to_lv(0xF8FAFC), 0);
    lv_obj_center(l_wc);
    lv_obj_add_event_cb(s_btn_dock_whitecap, [](lv_event_t *e) {
        (void)e;
        set_whitecap_visible(true);
    }, LV_EVENT_CLICKED, NULL);

    s_btn_dock_speakers = lv_btn_create(p0);
    lv_obj_set_size(s_btn_dock_speakers, 78, 34);
    lv_obj_align(s_btn_dock_speakers, LV_ALIGN_BOTTOM_MID, 88, -32);
    lv_obj_set_style_radius(s_btn_dock_speakers, 17, 0);
    lv_obj_set_style_bg_color(s_btn_dock_speakers, hex_to_lv(0x0F141D), 0);
    lv_obj_set_style_border_width(s_btn_dock_speakers, 1, 0);
    lv_obj_set_style_border_color(s_btn_dock_speakers, hex_to_lv(0x1E293B), 0);
    lv_obj_set_style_shadow_width(s_btn_dock_speakers, 0, 0);
    lv_obj_t *l_sp = lv_label_create(s_btn_dock_speakers);
    lv_label_set_text(l_sp, "AirPlay");
    lv_obj_set_style_text_font(l_sp, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l_sp, hex_to_lv(0xCBD5E1), 0);
    lv_obj_center(l_sp);
    lv_obj_add_event_cb(s_btn_dock_speakers, [](lv_event_t *e) {
        (void)e;
        rebuild_speaker_list();
        switch_tab(TAB_SPEAKERS);
    }, LV_EVENT_CLICKED, NULL);

    // =========================================================================
    // PAGE 1: MINIMALIST STATIONS SHEET
    // =========================================================================
    lv_obj_t *p1 = s_pages[1];

    lv_obj_t *st_hdr = lv_label_create(p1);
    lv_label_set_text(st_hdr, "SOMAFM STATIONS");
    lv_obj_set_style_text_font(st_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st_hdr, hex_to_lv(0x94A3B8), 0);
    lv_obj_align(st_hdr, LV_ALIGN_TOP_MID, 0, 20);

    s_station_list = lv_obj_create(p1);
    lv_obj_set_size(s_station_list, 290, 232);
    lv_obj_align(s_station_list, LV_ALIGN_CENTER, 0, -4);
    lv_obj_set_style_bg_opa(s_station_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_station_list, 0, 0);
    lv_obj_set_style_pad_all(s_station_list, 4, 0);
    lv_obj_set_style_pad_row(s_station_list, 6, 0);
    lv_obj_set_flex_flow(s_station_list, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *st_back = lv_btn_create(p1);
    lv_obj_set_size(st_back, 108, 32);
    lv_obj_align(st_back, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_radius(st_back, 16, 0);
    lv_obj_set_style_bg_color(st_back, hex_to_lv(0x161B26), 0);
    lv_obj_set_style_border_width(st_back, 1, 0);
    lv_obj_set_style_border_color(st_back, hex_to_lv(0x334155), 0);
    lv_obj_t *st_bl = lv_label_create(st_back);
    lv_label_set_text(st_bl, "Done");
    lv_obj_set_style_text_font(st_bl, &lv_font_montserrat_12, 0);
    lv_obj_center(st_bl);
    lv_obj_add_event_cb(st_back, [](lv_event_t *e) {
        (void)e;
        switch_tab(TAB_NOW_PLAYING);
    }, LV_EVENT_CLICKED, NULL);

    rebuild_station_list();

    // =========================================================================
    // PAGE 2: MINIMALIST AIRPLAY SPEAKERS SHEET
    // =========================================================================
    lv_obj_t *p2 = s_pages[2];

    s_lbl_spkr_hdr = lv_label_create(p2);
    lv_label_set_text(s_lbl_spkr_hdr, "AIRPLAY SPEAKERS");
    lv_obj_set_style_text_font(s_lbl_spkr_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_spkr_hdr, hex_to_lv(0x94A3B8), 0);
    lv_obj_align(s_lbl_spkr_hdr, LV_ALIGN_TOP_MID, 0, 20);

    s_speaker_list = lv_obj_create(p2);
    lv_obj_set_size(s_speaker_list, 290, 224);
    lv_obj_align(s_speaker_list, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_bg_opa(s_speaker_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_speaker_list, 0, 0);
    lv_obj_set_style_pad_all(s_speaker_list, 4, 0);
    lv_obj_set_style_pad_row(s_speaker_list, 6, 0);
    lv_obj_set_flex_flow(s_speaker_list, LV_FLEX_FLOW_COLUMN);

    // Bottom buttons on Speaker sheet: [Rescan] [Done] [Wi-Fi / Web]
    lv_obj_t *sp_scan = lv_btn_create(p2);
    lv_obj_set_size(sp_scan, 76, 30);
    lv_obj_align(sp_scan, LV_ALIGN_BOTTOM_MID, -82, -22);
    lv_obj_set_style_radius(sp_scan, 15, 0);
    lv_obj_set_style_bg_color(sp_scan, hex_to_lv(0x0F141D), 0);
    lv_obj_t *sl = lv_label_create(sp_scan);
    lv_label_set_text(sl, "Rescan");
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_12, 0);
    lv_obj_center(sl);
    lv_obj_add_event_cb(sp_scan, [](lv_event_t *e) {
        (void)e;
        haptic_play_effect(HAPTIC_SHARP_CLICK);
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_app.speaker_scan_requested = true;
            g_app.speaker_scanning = true;
            xSemaphoreGive(g_app.mutex);
        }
        if (s_lbl_spkr_hdr) lv_label_set_text(s_lbl_spkr_hdr, "SCANNING AIRPLAY...");
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sp_done = lv_btn_create(p2);
    lv_obj_set_size(sp_done, 76, 30);
    lv_obj_align(sp_done, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_set_style_radius(sp_done, 15, 0);
    lv_obj_set_style_bg_color(sp_done, hex_to_lv(0x161B26), 0);
    lv_obj_t *dl = lv_label_create(sp_done);
    lv_label_set_text(dl, "Done");
    lv_obj_set_style_text_font(dl, &lv_font_montserrat_12, 0);
    lv_obj_center(dl);
    lv_obj_add_event_cb(sp_done, [](lv_event_t *e) {
        (void)e;
        switch_tab(TAB_NOW_PLAYING);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sp_wifi = lv_btn_create(p2);
    lv_obj_set_size(sp_wifi, 76, 30);
    lv_obj_align(sp_wifi, LV_ALIGN_BOTTOM_MID, 82, -22);
    lv_obj_set_style_radius(sp_wifi, 15, 0);
    lv_obj_set_style_bg_color(sp_wifi, hex_to_lv(0x0F141D), 0);
    lv_obj_t *wl = lv_label_create(sp_wifi);
    lv_label_set_text(wl, LV_SYMBOL_WIFI " Info");
    lv_obj_set_style_text_font(wl, &lv_font_montserrat_12, 0);
    lv_obj_center(wl);
    lv_obj_add_event_cb(sp_wifi, [](lv_event_t *e) {
        (void)e;
        switch_tab(TAB_WIFI);
    }, LV_EVENT_CLICKED, NULL);

    rebuild_speaker_list();

    // =========================================================================
    // PAGE 3: WI-FI, WEB UI QR & OTA INFO SHEET
    // =========================================================================
    lv_obj_t *p3 = s_pages[3];

    s_lbl_wifi_ssid = lv_label_create(p3);
    lv_label_set_text(s_lbl_wifi_ssid, LV_SYMBOL_WIFI " SvensHaus");
    lv_obj_set_style_text_font(s_lbl_wifi_ssid, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_wifi_ssid, hex_to_lv(0xF8FAFC), 0);
    lv_obj_align(s_lbl_wifi_ssid, LV_ALIGN_TOP_MID, 0, 22);

    s_lbl_wifi_ip = lv_label_create(p3);
    lv_label_set_text(s_lbl_wifi_ip, "http://airplayknob.local/");
    lv_obj_set_style_text_font(s_lbl_wifi_ip, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_wifi_ip, hex_to_lv(0x38BDF8), 0);
    lv_obj_align(s_lbl_wifi_ip, LV_ALIGN_TOP_MID, 0, 44);

    lv_obj_t *qr_card = lv_obj_create(p3);
    lv_obj_set_size(qr_card, 128, 128);
    lv_obj_align(qr_card, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_set_style_radius(qr_card, 14, 0);
    lv_obj_set_style_bg_color(qr_card, hex_to_lv(0xFFFFFF), 0);
    lv_obj_set_style_border_width(qr_card, 0, 0);
    lv_obj_set_style_pad_all(qr_card, 6, 0);
    lv_obj_clear_flag(qr_card, LV_OBJ_FLAG_SCROLLABLE);

    s_qr_code = lv_qrcode_create(qr_card, 114, hex_to_lv(0x050608), hex_to_lv(0xFFFFFF));
    lv_qrcode_update(s_qr_code, "http://airplayknob.local/", strlen("http://airplayknob.local/"));
    lv_obj_center(s_qr_code);

    lv_obj_t *ota_lbl = lv_label_create(p3);
    lv_label_set_text(ota_lbl, "Web Presets & OTA Ready");
    lv_obj_set_style_text_font(ota_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ota_lbl, hex_to_lv(0x64748B), 0);
    lv_obj_align(ota_lbl, LV_ALIGN_TOP_MID, 0, 212);

    lv_obj_t *w_scan_btn = lv_btn_create(p3);
    lv_obj_set_size(w_scan_btn, 108, 30);
    lv_obj_align(w_scan_btn, LV_ALIGN_BOTTOM_MID, -60, -24);
    lv_obj_set_style_radius(w_scan_btn, 15, 0);
    lv_obj_set_style_bg_color(w_scan_btn, hex_to_lv(0x0F141D), 0);
    lv_obj_t *wsl = lv_label_create(w_scan_btn);
    lv_label_set_text(wsl, "Wi-Fi Scan");
    lv_obj_set_style_text_font(wsl, &lv_font_montserrat_12, 0);
    lv_obj_center(wsl);
    lv_obj_add_event_cb(w_scan_btn, open_wifi_scan_modal, LV_EVENT_CLICKED, NULL);

    lv_obj_t *w_done_btn = lv_btn_create(p3);
    lv_obj_set_size(w_done_btn, 92, 30);
    lv_obj_align(w_done_btn, LV_ALIGN_BOTTOM_MID, 54, -24);
    lv_obj_set_style_radius(w_done_btn, 15, 0);
    lv_obj_set_style_bg_color(w_done_btn, hex_to_lv(0x161B26), 0);
    lv_obj_t *wdl = lv_label_create(w_done_btn);
    lv_label_set_text(wdl, "Done");
    lv_obj_set_style_text_font(wdl, &lv_font_montserrat_12, 0);
    lv_obj_center(wdl);
    lv_obj_add_event_cb(w_done_btn, [](lv_event_t *e) {
        (void)e;
        switch_tab(TAB_NOW_PLAYING);
    }, LV_EVENT_CLICKED, NULL);

    // =========================================================================
    // FULL-SCREEN WHITECAP VISUALIZER LAYER (360x360)
    // =========================================================================
    s_wc_fb = (lv_color_t *)heap_caps_calloc(WC_W * WC_H, sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_wc_dsc.header.always_zero = 0;
    s_wc_dsc.header.w = WC_W;
    s_wc_dsc.header.h = WC_H;
    s_wc_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_wc_dsc.data_size = WC_W * WC_H * sizeof(lv_color_t);
    s_wc_dsc.data = (const uint8_t *)s_wc_fb;

    s_wc_layer = lv_obj_create(s_scr);
    lv_obj_set_size(s_wc_layer, WC_W, WC_H);
    lv_obj_center(s_wc_layer);
    lv_obj_set_style_bg_color(s_wc_layer, hex_to_lv(0x000000), 0);
    lv_obj_set_style_border_width(s_wc_layer, 0, 0);
    lv_obj_set_style_pad_all(s_wc_layer, 0, 0);
    lv_obj_clear_flag(s_wc_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wc_layer, LV_OBJ_FLAG_HIDDEN);

    s_wc_img = lv_img_create(s_wc_layer);
    lv_img_set_src(s_wc_img, &s_wc_dsc);
    lv_obj_center(s_wc_img);
    lv_obj_add_flag(s_wc_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wc_img, [](lv_event_t *e) {
        (void)e;
        lv_point_t pt;
        lv_indev_get_point(lv_indev_get_act(), &pt);
        // Tapping top half cycles WhiteCap visual preset; tapping lower half exits to Now Playing
        if (pt.y < 140) {
            int next_p = 0;
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                next_p = (g_app.whitecap_preset + 1) & 3;
                g_app.whitecap_preset = next_p;
                xSemaphoreGive(g_app.mutex);
            }
            s_wc_last_morph_ms = millis();
            haptic_play_effect(HAPTIC_LIGHT_TICK);
            show_whitecap_caption(lv_label_get_text(s_lbl_track), WHITECAP_PRESET_NAMES[next_p], 3200);
        } else {
            set_whitecap_visible(false);
        }
    }, LV_EVENT_CLICKED, NULL);

    s_wc_caption_box = lv_obj_create(s_wc_layer);
    lv_obj_set_size(s_wc_caption_box, 256, 48);
    lv_obj_align(s_wc_caption_box, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_style_radius(s_wc_caption_box, 24, 0);
    lv_obj_set_style_bg_color(s_wc_caption_box, hex_to_lv(0x050608), 0);
    lv_obj_set_style_bg_opa(s_wc_caption_box, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_wc_caption_box, 1, 0);
    lv_obj_set_style_border_color(s_wc_caption_box, hex_to_lv(0x1E293B), 0);
    lv_obj_set_style_pad_all(s_wc_caption_box, 4, 0);
    lv_obj_clear_flag(s_wc_caption_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_wc_lbl_track = lv_label_create(s_wc_caption_box);
    lv_label_set_text(s_wc_lbl_track, "Groove Salad");
    lv_obj_set_style_text_font(s_wc_lbl_track, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_wc_lbl_track, hex_to_lv(0xF8FAFC), 0);
    lv_label_set_long_mode(s_wc_lbl_track, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_wc_lbl_track, 232);
    lv_obj_set_style_text_align(s_wc_lbl_track, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wc_lbl_track, LV_ALIGN_TOP_MID, 0, 2);

    s_wc_lbl_sub = lv_label_create(s_wc_caption_box);
    lv_label_set_text(s_wc_lbl_sub, WHITECAP_PRESET_NAMES[0]);
    lv_obj_set_style_text_font(s_wc_lbl_sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_wc_lbl_sub, hex_to_lv(0x94A3B8), 0);
    lv_obj_align(s_wc_lbl_sub, LV_ALIGN_BOTTOM_MID, 0, -2);

    // =========================================================================
    // MINIMALIST FLOATING HUD PILL (Volume / Station Scrub / Speaker Scrub)
    // =========================================================================
    s_hud_pill = lv_obj_create(s_scr);
    lv_obj_set_size(s_hud_pill, 224, 52);
    lv_obj_align(s_hud_pill, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_style_radius(s_hud_pill, 26, 0);
    lv_obj_set_style_bg_color(s_hud_pill, hex_to_lv(0x0B0F19), 0);
    lv_obj_set_style_bg_opa(s_hud_pill, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_hud_pill, 1, 0);
    lv_obj_set_style_border_color(s_hud_pill, hex_to_lv(0x10B981), 0);
    lv_obj_set_style_pad_all(s_hud_pill, 4, 0);
    lv_obj_clear_flag(s_hud_pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_hud_title = lv_label_create(s_hud_pill);
    lv_label_set_text(s_lbl_hud_title, "VOLUME");
    lv_obj_set_style_text_font(s_lbl_hud_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_hud_title, hex_to_lv(0x10B981), 0);
    lv_obj_align(s_lbl_hud_title, LV_ALIGN_TOP_MID, 0, 2);

    s_lbl_hud_val = lv_label_create(s_hud_pill);
    lv_label_set_text(s_lbl_hud_val, "65%");
    lv_obj_set_style_text_font(s_lbl_hud_val, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_hud_val, hex_to_lv(0xFFFFFF), 0);
    lv_label_set_long_mode(s_lbl_hud_val, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_lbl_hud_val, 204);
    lv_obj_set_style_text_align(s_lbl_hud_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_hud_val, LV_ALIGN_BOTTOM_MID, 0, -2);

    lv_obj_add_flag(s_hud_pill, LV_OBJ_FLAG_HIDDEN);

    bsp_lvgl_unlock();
}

void knob_ui_on_rotate(int delta) {
    if (delta == 0) return;
    if (!bsp_lvgl_lock(30)) return;

    // If Stations sheet is open, knob scrolls through stations
    if (s_active_tab == TAB_STATIONS && s_station_rendered_count > 0) {
        int next = s_station_cursor + (delta > 0 ? 1 : -1);
        if (next < 0) next = 0;
        if (next >= s_station_rendered_count) next = s_station_rendered_count - 1;
        haptic_play_effect(HAPTIC_LIGHT_TICK);

        if (s_station_items[s_station_cursor]) {
            lv_obj_set_style_border_width(s_station_items[s_station_cursor], 1, 0);
            lv_obj_set_style_border_color(s_station_items[s_station_cursor], hex_to_lv(0x1E293B), 0);
        }
        s_station_cursor = next;
        if (s_station_items[s_station_cursor]) {
            lv_obj_set_style_border_width(s_station_items[s_station_cursor], 2, 0);
            lv_obj_set_style_border_color(s_station_items[s_station_cursor], hex_to_lv(0x10B981), 0);
            lv_obj_scroll_to_view(s_station_items[s_station_cursor], LV_ANIM_ON);
        }
        bsp_lvgl_unlock();
        return;
    }

    // If Speakers sheet is open, knob scrolls through speakers
    if (s_active_tab == TAB_SPEAKERS && s_speaker_rendered_count > 0) {
        int next = s_speaker_cursor + (delta > 0 ? 1 : -1);
        if (next < 0) next = 0;
        if (next >= s_speaker_rendered_count) next = s_speaker_rendered_count - 1;
        haptic_play_effect(HAPTIC_LIGHT_TICK);

        if (s_speaker_items[s_speaker_cursor]) {
            lv_obj_set_style_border_width(s_speaker_items[s_speaker_cursor], 1, 0);
            lv_obj_set_style_border_color(s_speaker_items[s_speaker_cursor], hex_to_lv(0x1E293B), 0);
        }
        s_speaker_cursor = next;
        if (s_speaker_items[s_speaker_cursor]) {
            lv_obj_set_style_border_width(s_speaker_items[s_speaker_cursor], 2, 0);
            lv_obj_set_style_border_color(s_speaker_items[s_speaker_cursor], hex_to_lv(0x38BDF8), 0);
            lv_obj_scroll_to_view(s_speaker_items[s_speaker_cursor], LV_ANIM_ON);
        }
        bsp_lvgl_unlock();
        return;
    }

    // Now Playing or WhiteCap Visualizer: Volume / Station Tuner / Speaker Selector
    KnobMode mode = KNOB_MODE_VOLUME;
    int vol = 65;
    int p_cnt = 1, p_preview = 0;
    int s_cnt = 1, s_preview = 0;
    StationPreset preview_st = {};
    AirPlaySpeaker preview_sp = {};

    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        mode = g_app.knob_mode;
        if (mode == KNOB_MODE_VOLUME) {
            vol = g_app.volume + delta * 2;
            if (vol < 0) vol = 0;
            if (vol > 100) vol = 100;
            g_app.volume = vol;
            g_app.volume_dirty = true;
            if (g_app.current_preset >= 0 && g_app.current_preset < g_app.preset_count) {
                preview_st = g_app.presets[g_app.current_preset];
            }
            if (g_app.current_speaker >= 0 && g_app.current_speaker < g_app.speaker_count) {
                preview_sp = g_app.speakers[g_app.current_speaker];
            }
        } else if (mode == KNOB_MODE_STATION) {
            p_cnt = g_app.preset_count;
            p_preview = (g_app.preview_preset + (delta > 0 ? 1 : -1) + p_cnt) % p_cnt;
            g_app.preview_preset = p_preview;
            g_app.preview_timestamp_ms = millis();
            preview_st = g_app.presets[p_preview];
        } else if (mode == KNOB_MODE_SPEAKER) {
            s_cnt = g_app.speaker_count;
            s_preview = (g_app.preview_speaker + (delta > 0 ? 1 : -1) + s_cnt) % s_cnt;
            g_app.preview_speaker = s_preview;
            g_app.preview_timestamp_ms = millis();
            preview_sp = g_app.speakers[s_preview];
        }
        xSemaphoreGive(g_app.mutex);
    }

    if (mode == KNOB_MODE_VOLUME) {
        haptic_play_effect((vol == 0 || vol == 100) ? HAPTIC_DOUBLE_CLICK : HAPTIC_SHARP_CLICK);
        char vstr[32];
        snprintf(vstr, sizeof(vstr), "%d%%", vol);
        show_hud(preview_sp.name[0] ? preview_sp.name : "VOLUME", vstr,
                 preview_st.color ? preview_st.color : 0x10B981, 950);
        lv_arc_set_range(s_outer_arc, 0, 100);
        lv_arc_set_value(s_outer_arc, vol);
    } else if (mode == KNOB_MODE_STATION) {
        haptic_play_effect(HAPTIC_LIGHT_TICK);
        show_hud("TUNE STATION", preview_st.name, preview_st.color ? preview_st.color : 0xF59E0B, 1150);
        lv_arc_set_range(s_outer_arc, 0, p_cnt > 1 ? p_cnt - 1 : 1);
        lv_arc_set_value(s_outer_arc, p_preview);
    } else if (mode == KNOB_MODE_SPEAKER) {
        haptic_play_effect(HAPTIC_LIGHT_TICK);
        show_hud("AIRPLAY SPEAKER", preview_sp.name, 0x38BDF8, 1200);
        lv_arc_set_range(s_outer_arc, 0, s_cnt > 1 ? s_cnt - 1 : 1);
        lv_arc_set_value(s_outer_arc, s_preview);
    }

    bsp_lvgl_unlock();
}

void knob_ui_tick(void) {
    int commit_preset = -1;
    int commit_speaker = -1;
    uint32_t now_ms = millis();

    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (g_app.preview_timestamp_ms > 0 && (now_ms - g_app.preview_timestamp_ms >= 900)) {
            g_app.preview_timestamp_ms = 0;
            if (g_app.knob_mode == KNOB_MODE_STATION && g_app.preview_preset != g_app.current_preset) {
                commit_preset = g_app.preview_preset;
            } else if (g_app.knob_mode == KNOB_MODE_SPEAKER && g_app.preview_speaker != g_app.current_speaker) {
                commit_speaker = g_app.preview_speaker;
            }
        }
        xSemaphoreGive(g_app.mutex);
    }

    if (commit_preset >= 0) {
        haptic_play_effect(HAPTIC_DOUBLE_CLICK);
        app_select_preset(commit_preset, true);
    }
    if (commit_speaker >= 0) {
        haptic_play_effect(HAPTIC_DOUBLE_CLICK);
        app_select_speaker(commit_speaker);
    }

    if (!bsp_lvgl_lock(15)) return;

    if (s_hud_hide_ms > 0 && now_ms >= s_hud_hide_ms) {
        s_hud_hide_ms = 0;
        if (s_hud_pill) lv_obj_add_flag(s_hud_pill, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wc_caption_hide_ms > 0 && now_ms >= s_wc_caption_hide_ms) {
        s_wc_caption_hide_ms = 0;
        if (s_wc_caption_box) lv_obj_add_flag(s_wc_caption_box, LV_OBJ_FLAG_HIDDEN);
    }

    // Snapshot state
    StationPreset cur_st = {};
    AirPlaySpeaker cur_sp = {};
    bool playing = false;
    int vol = 65;
    char raw_track[128] = {0};
    uint8_t vu[12] = {0};
    uint8_t peak = 0;
    int8_t wl[128] = {0};
    int8_t wr[128] = {0};
    bool wifi_ok = false;
    char ssid[36] = {0};
    char ip[20] = {0};
    bool wc_active = false;
    int wc_preset = 0;
    bool ota_active = false;
    int ota_pct = 0;
    int spk_count = 1;
    bool spk_scanning = false;

    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (g_app.current_preset >= 0 && g_app.current_preset < g_app.preset_count) {
            cur_st = g_app.presets[g_app.current_preset];
        }
        if (g_app.current_speaker >= 0 && g_app.current_speaker < g_app.speaker_count) {
            cur_sp = g_app.speakers[g_app.current_speaker];
        }
        playing = g_app.playing;
        vol = g_app.volume;
        strncpy(raw_track, g_app.track_title, sizeof(raw_track) - 1);
        memcpy(vu, g_app.vu_bars, 12);
        peak = g_app.vu_peak;
        memcpy(wl, g_app.wave_l, 128);
        memcpy(wr, g_app.wave_r, 128);
        wifi_ok = g_app.wifi_connected;
        strncpy(ssid, g_app.wifi_ssid, sizeof(ssid) - 1);
        strncpy(ip, g_app.wifi_ip, sizeof(ip) - 1);
        wc_active = g_app.whitecap_active;
        wc_preset = g_app.whitecap_preset & 3;
        ota_active = g_app.ota_in_progress;
        ota_pct = g_app.ota_progress_pct;
        spk_count = g_app.speaker_count;
        spk_scanning = g_app.speaker_scanning;
        xSemaphoreGive(g_app.mutex);
    }

    if (ota_active) {
        char pstr[32];
        snprintf(pstr, sizeof(pstr), "%d%% Complete", ota_pct);
        show_hud("OTA FIRMWARE UPDATE", pstr, 0x38BDF8, 1000);
        lv_arc_set_range(s_outer_arc, 0, 100);
        lv_arc_set_value(s_outer_arc, ota_pct);
        bsp_lvgl_unlock();
        return;
    }

    // Refresh Speakers list automatically when mDNS scan finishes
    static bool prev_spk_scanning = false;
    static int prev_spk_count = 1;
    if ((prev_spk_scanning && !spk_scanning) || (spk_count != prev_spk_count)) {
        prev_spk_scanning = spk_scanning;
        prev_spk_count = spk_count;
        if (s_active_tab == TAB_SPEAKERS) {
            rebuild_speaker_list();
        }
    }

    // Check if new Album Art or Stream Art arrived from artwork_fetcher
    static uint32_t last_art_seq = 0;
    uint32_t cur_art_seq = artwork_get_seq();
    if (cur_art_seq != last_art_seq) {
        last_art_seq = cur_art_seq;
        if (s_art_img) lv_obj_invalidate(s_art_img);
    }

    uint32_t theme_col = cur_st.color ? cur_st.color : 0x10B981;
    lv_obj_set_style_arc_color(s_outer_arc, hex_to_lv(theme_col), LV_PART_INDICATOR);
    lv_obj_set_style_shadow_color(s_art_cont, hex_to_lv(theme_col), 0);

    if (playing) lv_obj_add_flag(s_pause_badge, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_clear_flag(s_pause_badge, LV_OBJ_FLAG_HIDDEN);

    // Split "Artist - Track" and update minimalist typography
    char artist_str[80] = {0};
    char track_str[100] = {0};
    split_artist_track(raw_track, cur_st.name, artist_str, sizeof(artist_str), track_str, sizeof(track_str));

    static char prev_raw_track[128] = {0};
    if (strcmp(raw_track, prev_raw_track) != 0) {
        strncpy(prev_raw_track, raw_track, sizeof(prev_raw_track) - 1);
        lv_label_set_text(s_lbl_track, track_str);
        lv_label_set_text(s_lbl_artist, artist_str);
        if (wc_active) {
            show_whitecap_caption(track_str, artist_str, 4500);
        }
    }

    char sub_line[96];
    snprintf(sub_line, sizeof(sub_line), "%s  •  %s",
             cur_st.name, cur_sp.name[0] ? cur_sp.name : "AirPlay");
    static char prev_sub[96] = {0};
    if (strcmp(sub_line, prev_sub) != 0) {
        strncpy(prev_sub, sub_line, sizeof(prev_sub) - 1);
        lv_label_set_text(s_lbl_sub_meta, sub_line);
        lv_obj_set_style_text_color(s_lbl_sub_meta, hex_to_lv(theme_col), 0);
    }

    if (s_hud_hide_ms == 0) {
        lv_arc_set_range(s_outer_arc, 0, 100);
        lv_arc_set_value(s_outer_arc, vol);
    }

    // Update Full-Screen WhiteCap Visualizer if active
    if (wc_active && s_wc_layer) {
        if (lv_obj_has_flag(s_wc_layer, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_clear_flag(s_wc_layer, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_wc_layer);
        }
        // Auto-morph WhiteCap preset every 16 seconds
        if (now_ms - s_wc_last_morph_ms >= 16000) {
            s_wc_last_morph_ms = now_ms;
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                g_app.whitecap_preset = (g_app.whitecap_preset + 1) & 3;
                wc_preset = g_app.whitecap_preset;
                xSemaphoreGive(g_app.mutex);
            }
        }
        if (now_ms - s_wc_last_frame_ms >= 28) {
            s_wc_last_frame_ms = now_ms;
            render_whitecap_frame(wl, wr, vu, peak, wc_preset);
            lv_obj_invalidate(s_wc_img);
        }
    } else if (!wc_active && s_wc_layer && !lv_obj_has_flag(s_wc_layer, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_wc_layer, LV_OBJ_FLAG_HIDDEN);
    }

    // Update Wi-Fi & QR code tab when IP changes
    static char prev_ip[20] = {0};
    if (wifi_ok && strcmp(ip, prev_ip) != 0 && ip[0] != '\0') {
        strncpy(prev_ip, ip, sizeof(prev_ip) - 1);
        char url[64];
        snprintf(url, sizeof(url), "http://%s/", ip);
        lv_label_set_text_fmt(s_lbl_wifi_ssid, LV_SYMBOL_WIFI " %s", ssid);
        lv_label_set_text(s_lbl_wifi_ip, url);
        if (s_qr_code) {
            lv_qrcode_update(s_qr_code, url, strlen(url));
        }
    }

    // Populate Wi-Fi scan modal if open and scan finished
    if (s_wifi_modal && s_wifi_list) {
        static bool last_scanning = false;
        bool cur_scanning = false;
        int scan_cnt = 0;
        WifiScanItem items[MAX_WIFI_SCAN];
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            cur_scanning = g_app.wifi_scanning;
            scan_cnt = g_app.wifi_scan_count;
            memcpy(items, g_app.wifi_scan, sizeof(items));
            xSemaphoreGive(g_app.mutex);
        }
        if (last_scanning && !cur_scanning) {
            lv_obj_clean(s_wifi_list);
            for (int i = 0; i < scan_cnt; i++) {
                lv_obj_t *b = lv_btn_create(s_wifi_list);
                lv_obj_set_size(b, 256, 36);
                lv_obj_set_style_bg_color(b, hex_to_lv(0x161B26), 0);
                lv_obj_add_event_cb(b, wifi_ssid_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
                lv_obj_t *l = lv_label_create(b);
                lv_label_set_text_fmt(l, "%s (%ld dBm)", items[i].ssid, (long)items[i].rssi);
                lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
                lv_obj_align(l, LV_ALIGN_LEFT_MID, 6, 0);
            }
        }
        last_scanning = cur_scanning;
    }

    bsp_lvgl_unlock();
}

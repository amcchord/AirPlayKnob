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
static uint32_t s_prev_theme_col = 0;
static int s_prev_arc_max = -1;
static int s_prev_arc_val = -1;

// Tab 0: Full-Screen Album Art (360x360) + Bottom Scrim Typography Overlay
static lv_obj_t *s_art_cont = NULL;
static lv_obj_t *s_art_img = NULL;
static lv_obj_t *s_pause_badge = NULL;
static lv_obj_t *s_lbl_track = NULL;
static lv_obj_t *s_lbl_artist = NULL;
static lv_obj_t *s_lbl_sub_meta = NULL;

// =============================================================================
// 60 FPS Virtualized 5-Slot Dial Wheel for Stations (Tab 1) & Speakers (Tab 2)
// Zero dynamic allocation or flex-layout recalculation during scrolling!
// =============================================================================
#define WHEEL_SLOTS 5

// Tab 1: Virtualized Stations Wheel
static lv_obj_t *s_lbl_st_hdr = NULL;
static lv_obj_t *s_st_slots[WHEEL_SLOTS] = {NULL};
static lv_obj_t *s_st_dots[WHEEL_SLOTS] = {NULL};
static lv_obj_t *s_st_titles[WHEEL_SLOTS] = {NULL};
static lv_obj_t *s_st_tags[WHEEL_SLOTS] = {NULL};
static int s_station_cursor = 0;
static int s_st_drag_accum = 0;
static bool s_st_dragging = false;

// Tab 2: Virtualized AirPlay Speakers Wheel
static lv_obj_t *s_lbl_spkr_hdr = NULL;
static lv_obj_t *s_sp_slots[WHEEL_SLOTS] = {NULL};
static lv_obj_t *s_sp_dots[WHEEL_SLOTS] = {NULL};
static lv_obj_t *s_sp_titles[WHEEL_SLOTS] = {NULL};
static lv_obj_t *s_sp_tags[WHEEL_SLOTS] = {NULL};
static int s_speaker_cursor = 0;
static int s_sp_drag_accum = 0;
static bool s_sp_dragging = false;

// Tab 3: Settings (Wi-Fi, Web Portal QR, AirPlay Rescan & OTA Info)
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
// Full-Screen Old-School "WhiteCap" 3D Wireframe Terrain Visualizer (60 FPS)
// Zero-PSRAM Direct Internal-SRAM DMA Strip Renderer
// =============================================================================
#define WC_W 360
#define WC_H 360
#define WC_COLS 34
#define WC_ROWS 26
#define WC_DIRTY_Y1 92
#define WC_DIRTY_Y2 319

static lv_obj_t *s_wc_layer = NULL;
static lv_obj_t *s_wc_caption_box = NULL;
static lv_obj_t *s_wc_lbl_track = NULL;
static lv_obj_t *s_wc_lbl_sub = NULL;
static uint32_t s_wc_caption_hide_ms = 0;
static float s_wc_phase = 0.0f;

// Stored in fast 240MHz internal SRAM (~4.5 KB total, zero PSRAM traffic!)
static uint8_t s_wc_grid[WC_ROWS][WC_COLS] = {{0}};
static int16_t s_wc_vx[WC_ROWS][WC_COLS] = {{0}};
static int16_t s_wc_vy[WC_ROWS][WC_COLS] = {{0}};
static int16_t s_wc_row_ymin[WC_ROWS] = {0};
static int16_t s_wc_row_ymax[WC_ROWS] = {0};
static int16_t s_wc_prev_frame_ymin = WC_DIRTY_Y1;
static int16_t s_wc_cur_frame_ymin = WC_DIRTY_Y1;
static int s_wc_cur_preset = 0;

static const char *WHITECAP_PRESET_NAMES[4] = {
    "WhiteCap • Classic 3D Terrain",
    "WhiteCap • Cyber Horizon",
    "WhiteCap • Solar Flare",
    "WhiteCap • Matrix Emerald"
};

static lv_color_t hex_to_lv(uint32_t hex) {
    return lv_color_make((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF);
}

static inline uint16_t rgb_to_swapped565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t c = ((uint16_t)(r & 0xF8) << 8) |
                 ((uint16_t)(g & 0xFC) << 3) |
                 ((uint16_t)(b >> 3));
    return (uint16_t)((c >> 8) | (c << 8));
}

static void set_arc_state(int range_max, int val) {
    if (!s_outer_arc) return;
    if (range_max < 1) range_max = 1;
    if (range_max != s_prev_arc_max) {
        s_prev_arc_max = range_max;
        lv_arc_set_range(s_outer_arc, 0, range_max);
    }
    if (val != s_prev_arc_val) {
        s_prev_arc_val = val;
        lv_arc_set_value(s_outer_arc, val);
    }
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

// Direct Internal-SRAM Strip Bresenham line drawer (draws into LVGL's active DMA strip buffer)
static inline void wc_strip_line(uint16_t *buf, int by1, int by2,
                                 int x0, int y0, int x1, int y1, uint16_t col) {
    // Trivial vertical reject if segment is completely outside this 40-row DMA strip
    int seg_min_y = (y0 < y1) ? y0 : y1;
    int seg_max_y = (y0 > y1) ? y0 : y1;
    if (seg_max_y + 1 < by1 || seg_min_y > by2) return;

    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        if ((unsigned)x0 < WC_W - 1) {
            if (y0 >= by1 && y0 <= by2) {
                uint16_t *row = &buf[(y0 - by1) * WC_W + x0];
                row[0] = col;
                row[1] = col;
            }
            if (y0 + 1 >= by1 && y0 + 1 <= by2) {
                buf[(y0 + 1 - by1) * WC_W + x0] = col;
            }
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static uint16_t wc_row_color(int r, int preset, uint16_t *out_long_col) {
    // Interpolate t in [0..255] from front (r=0) to horizon (r=WC_ROWS-1)
    int t = (r * 255) / (WC_ROWS - 1);
    int r8 = 255, g8 = 24, b8 = 8;

    if (preset == 0) {
        // Exact Old-School WhiteCap palette from reference screenshot:
        // Front rows: Vivid Red (#FF1800) -> Mid-front: Red-Orange (#E84800) ->
        // Mid-back: Amber/Olive (#A89800) -> Far Horizon: Bright Neon Green (#08FF18)
        if (t < 96) {
            int u = (t * 255) / 96;
            r8 = 240 + ((15 * u) >> 8);
            g8 = 12 + ((56 * u) >> 8);
            b8 = 4;
        } else if (t < 185) {
            int u = ((t - 96) * 255) / 89;
            r8 = 255 - ((110 * u) >> 8);
            g8 = 68 + ((116 * u) >> 8);
            b8 = 4;
        } else {
            int u = ((t - 185) * 255) / 70;
            r8 = 145 - ((137 * u) >> 8);
            g8 = 184 + ((71 * u) >> 8);
            b8 = 8 + ((16 * u) >> 8);
        }
    } else if (preset == 1) {
        // Cyber Horizon: Cyan (#00F5D4) -> Electric Blue (#38BDF8) -> Neon Magenta (#FF1E8E)
        if (t < 128) {
            int u = t * 2;
            r8 = (56 * u) >> 8;
            g8 = 245 - ((65 * u) >> 8);
            b8 = 212 + ((43 * u) >> 8);
        } else {
            int u = (t - 128) * 2;
            r8 = 56 + ((199 * u) >> 8);
            g8 = 180 - ((150 * u) >> 8);
            b8 = 255 - ((95 * u) >> 8);
        }
    } else if (preset == 2) {
        // Solar Flare: Crimson (#FF0044) -> Gold (#FFAA00) -> Ice White (#E0F7FF)
        if (t < 140) {
            int u = (t * 255) / 140;
            r8 = 255;
            g8 = (180 * u) >> 8;
            b8 = 68 - ((68 * u) >> 8);
        } else {
            int u = ((t - 140) * 255) / 115;
            r8 = 255 - ((25 * u) >> 8);
            g8 = 180 + ((70 * u) >> 8);
            b8 = (255 * u) >> 8;
        }
    } else {
        // Matrix Emerald: Bright Lime (#A3E635) -> Emerald (#10B981) -> Deep Cyan (#06B6D4)
        r8 = 163 - ((150 * t) >> 8);
        g8 = 235 - ((45 * t) >> 8);
        b8 = 45 + ((170 * t) >> 8);
    }

    if (out_long_col) {
        // Slightly darker shade for longitudinal depth lines so horizontal ridges pop
        *out_long_col = rgb_to_swapped565((uint8_t)((r8 * 165) >> 8),
                                          (uint8_t)((g8 * 165) >> 8),
                                          (uint8_t)((b8 * 165) >> 8));
    }
    return rgb_to_swapped565((uint8_t)r8, (uint8_t)g8, (uint8_t)b8);
}

static void step_whitecap_simulation(const int8_t *wl, const int8_t *wr, const uint8_t *vu, uint8_t peak, int preset) {
    s_wc_cur_preset = preset;

    // 1. Shift 3D terrain history rows back by 1 (row 1 -> row 2 ... row 24 -> row 25)
    // Row 0 stays anchored at h=0 right on the green baseline!
    memmove(&s_wc_grid[2][0], &s_wc_grid[1][0], (WC_ROWS - 2) * WC_COLS);

    float energy = (float)peak / 100.0f;
    s_wc_phase += 0.12f + energy * 0.10f;

    // 2. Build new live audio row (row 1) from stereo waveform + VU spectrum
    uint8_t raw_row[WC_COLS];
    for (int c = 0; c < WC_COLS; c++) {
        int w_idx = (c * 127) / (WC_COLS - 1);
        int b_idx = (c * 11) / (WC_COLS - 1);

        int s_l = abs((int)wl[w_idx]);
        int s_r = abs((int)wr[127 - w_idx]);
        int wave_amp = (c < WC_COLS / 2) ? ((s_l * 3 + s_r) >> 2) : ((s_r * 3 + s_l) >> 2);
        int band_amp = (int)vu[b_idx];

        // Classic WhiteCap terrain shaping:
        // Tallest bass mountain peaks on the left-center (c = 2..12), finer harmonic ripples across center/right
        float env = (c < 12) ? (1.22f - 0.03f * abs(c - 4))
                             : (0.75f - 0.013f * (c - 12));
        if (env < 0.25f) env = 0.25f;

        float organic = 5.0f + 5.0f * sinf(c * 0.48f - s_wc_phase)
                             + 4.0f * cosf(c * 0.31f + s_wc_phase * 0.7f);

        float h_val = (band_amp * 0.45f + wave_amp * 0.56f) * env + organic;

        int w_signed = (c & 1) ? wl[w_idx] : wr[w_idx];
        if (w_signed > 14) {
            h_val += (w_signed - 14) * 0.38f * env;
        }

        if (h_val < 0.0f) h_val = 0.0f;
        if (h_val > 92.0f) h_val = 92.0f;
        raw_row[c] = (uint8_t)h_val;
    }

    // Smooth horizontally [1, 2, 1]/4 and blend with row 2 for cohesive mountain ridges
    for (int c = 0; c < WC_COLS; c++) {
        int prev_c = (c > 0) ? raw_row[c - 1] : raw_row[c];
        int next_c = (c + 1 < WC_COLS) ? raw_row[c + 1] : raw_row[c];
        int smoothed = (prev_c + (raw_row[c] << 1) + next_c) >> 2;
        int temporal = (smoothed * 3 + (int)s_wc_grid[2][c]) >> 2;

        // Taper left & right borders so the mesh edges sit cleanly like the reference screenshot
        if (c == 0 || c == WC_COLS - 1) temporal = 0;
        else if (c == 1 || c == WC_COLS - 2) temporal = (temporal * 2) / 5;
        else if (c == 2 || c == WC_COLS - 3) temporal = (temporal * 3) / 4;

        s_wc_grid[1][c] = (uint8_t)temporal;
        s_wc_grid[0][c] = 0; // Front edge anchored flush to the green baseline
    }

    // 3. Project 3D heightfield into 2D oblique perspective screen coordinates in internal SRAM
    const float max_r = (float)(WC_ROWS - 1);
    const float k_z = 0.041f;
    const float min_persp = 1.0f / (1.0f + max_r * k_z);
    int16_t frame_min_y = WC_DIRTY_Y2;

    for (int r = 0; r < WC_ROWS; r++) {
        float persp = 1.0f / (1.0f + (float)r * k_z);
        float depth_t = (1.0f - persp) / (1.0f - min_persp);

        float x_left  = 46.0f  + depth_t * 118.0f;
        float x_right = 308.0f - depth_t * 10.0f;
        float y_left  = 276.0f - depth_t * 154.0f;
        float y_right = 314.0f - depth_t * 174.0f;
        float h_scale = persp * 0.96f;

        int16_t r_min_y = 360;
        int16_t r_max_y = 0;

        for (int c = 0; c < WC_COLS; c++) {
            float u = (float)c / (float)(WC_COLS - 1);
            float bx = x_left + u * (x_right - x_left);
            float by = y_left + u * (y_right - y_left);
            float py = by - (float)s_wc_grid[r][c] * h_scale;
            if (py < (float)WC_DIRTY_Y1) py = (float)WC_DIRTY_Y1;
            if (py > (float)(WC_DIRTY_Y2 - 2)) py = (float)(WC_DIRTY_Y2 - 2);
            int16_t ix = (int16_t)bx;
            int16_t iy = (int16_t)py;
            s_wc_vx[r][c] = ix;
            s_wc_vy[r][c] = iy;
            if (iy < r_min_y) r_min_y = iy;
            if (iy > r_max_y) r_max_y = iy;
            if (r > 0) {
                int16_t py_prev = s_wc_vy[r - 1][c];
                if (py_prev < r_min_y) r_min_y = py_prev;
                if (py_prev > r_max_y) r_max_y = py_prev;
            }
        }
        s_wc_row_ymin[r] = r_min_y - 2;
        s_wc_row_ymax[r] = r_max_y + 2;
        if (s_wc_row_ymin[r] < frame_min_y) frame_min_y = s_wc_row_ymin[r];
    }
    if (frame_min_y < WC_DIRTY_Y1) frame_min_y = WC_DIRTY_Y1;
    s_wc_cur_frame_ymin = frame_min_y;
}

// LVGL custom draw callback: rasterizes visible 3D wireframe segments directly into internal DMA SRAM!
static void wc_layer_draw_event_cb(lv_event_t *e) {
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    if (!draw_ctx || !draw_ctx->buf || !draw_ctx->buf_area) return;

    uint16_t *buf = (uint16_t *)draw_ctx->buf;
    const int by1 = draw_ctx->buf_area->y1;
    const int by2 = draw_ctx->buf_area->y2;
    const int preset = s_wc_cur_preset;

    // Render back-to-front (painter's order r = WC_ROWS - 1 down to 1)
    for (int r = WC_ROWS - 1; r >= 1; r--) {
        if (s_wc_row_ymax[r] < by1 || s_wc_row_ymin[r] > by2) continue;

        uint16_t col_long = 0;
        uint16_t col_ridge = wc_row_color(r, preset, &col_long);

        // Longitudinal depth lines connecting row r to row r-1
        for (int c = 0; c < WC_COLS; c++) {
            wc_strip_line(buf, by1, by2,
                          s_wc_vx[r][c],     s_wc_vy[r][c],
                          s_wc_vx[r - 1][c], s_wc_vy[r - 1][c],
                          col_long);
        }

        // Transverse ridge line across row r
        for (int c = 1; c < WC_COLS; c++) {
            wc_strip_line(buf, by1, by2,
                          s_wc_vx[r][c - 1], s_wc_vy[r][c - 1],
                          s_wc_vx[r][c],     s_wc_vy[r][c],
                          col_ridge);
        }
    }

    // Crisp neon green front baseline along row 0 (exact match to reference screenshot)
    if (s_wc_row_ymax[0] >= by1 && s_wc_row_ymin[0] <= by2) {
        uint16_t baseline_col = (preset == 0) ? rgb_to_swapped565(32, 255, 32)
                              : (preset == 1) ? rgb_to_swapped565(0, 245, 212)
                              : (preset == 2) ? rgb_to_swapped565(255, 180, 0)
                                              : rgb_to_swapped565(16, 185, 129);
        for (int c = 1; c < WC_COLS; c++) {
            wc_strip_line(buf, by1, by2,
                          s_wc_vx[0][c - 1], s_wc_vy[0][c - 1],
                          s_wc_vx[0][c],     s_wc_vy[0][c],
                          baseline_col);
        }
    }
}

// LVGL timer callback: runs directly inside lv_timer_handler() right before display refresh for 60 FPS
static void whitecap_lv_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (!g_app.whitecap_active || !s_wc_layer || lv_obj_has_flag(s_wc_layer, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    uint8_t vu[12];
    uint8_t peak;
    int8_t wl[128];
    int8_t wr[128];
    int wc_preset;

    if (xSemaphoreTake(g_app.mutex, 0) == pdTRUE) {
        memcpy(vu, g_app.vu_bars, 12);
        peak = g_app.vu_peak;
        memcpy(wl, g_app.wave_l, 128);
        memcpy(wr, g_app.wave_r, 128);
        wc_preset = g_app.whitecap_preset & 3;
        xSemaphoreGive(g_app.mutex);
    } else {
        return;
    }

    step_whitecap_simulation(wl, wr, vu, peak, wc_preset);

    // Only invalidate the exact vertical bounding band of the previous + current 3D terrain mesh
    int16_t inv_y1 = (s_wc_cur_frame_ymin < s_wc_prev_frame_ymin) ? s_wc_cur_frame_ymin : s_wc_prev_frame_ymin;
    s_wc_prev_frame_ymin = s_wc_cur_frame_ymin;
    lv_area_t dirty = {0, inv_y1, WC_W - 1, WC_DIRTY_Y2};
    _lv_inv_area(lv_disp_get_default(), &dirty);
}

static void split_artist_track(const char *raw, const char *fallback_station,
                               char *artist_out, size_t artist_max,
                               char *track_out, size_t track_max) {
    if (!raw || raw[0] == '\0') {
        strncpy(track_out, fallback_station, track_max - 1);
        track_out[track_max - 1] = '\0';
        strncpy(artist_out, "SomaFM Radio", artist_max - 1);
        artist_out[artist_max - 1] = '\0';
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

// =============================================================================
// O(1) 60 FPS Virtualized Wheel Updaters (Zero DOM rebuilds!)
// =============================================================================
static void update_station_wheel(bool sync_cursor_to_active) {
    if (!s_st_slots[0]) return;

    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(30)) != pdTRUE) return;
    int count = g_app.preset_count;
    int active_idx = g_app.current_preset;
    if (count > MAX_PRESETS) count = MAX_PRESETS;
    if (count <= 0) {
        xSemaphoreGive(g_app.mutex);
        return;
    }

    if (sync_cursor_to_active || s_station_cursor < 0 || s_station_cursor >= count) {
        s_station_cursor = active_idx;
    }

    static const int SLOT_W[WHEEL_SLOTS] = {252, 282, 300, 282, 252};
    const StationPreset &focused_st = g_app.presets[s_station_cursor];
    if (s_lbl_st_hdr) {
        lv_label_set_text_fmt(s_lbl_st_hdr, "STATIONS  •  %d / %d  •  %s",
                              s_station_cursor + 1, count,
                              focused_st.genre[0] ? focused_st.genre : "SomaFM");
    }

    for (int slot = 0; slot < WHEEL_SLOTS; slot++) {
        int rel = slot - 2; // -2, -1, 0, +1, +2
        int idx = (s_station_cursor + rel + count * 4) % count;
        const StationPreset &st = g_app.presets[idx];
        bool is_center = (slot == 2);
        bool is_playing = (idx == active_idx);

        lv_obj_set_user_data(s_st_slots[slot], (void *)(intptr_t)idx);
        lv_obj_set_style_bg_color(s_st_dots[slot], hex_to_lv(st.color ? st.color : 0x10B981), 0);

        if (is_center) {
            lv_obj_set_style_bg_color(s_st_slots[slot], hex_to_lv(0x141B28), 0);
            lv_obj_set_style_border_width(s_st_slots[slot], 2, 0);
            lv_obj_set_style_border_color(s_st_slots[slot], hex_to_lv(st.color ? st.color : 0x10B981), 0);
            lv_obj_set_style_text_color(s_st_titles[slot], hex_to_lv(0xFFFFFF), 0);
        } else {
            lv_obj_set_style_bg_color(s_st_slots[slot], hex_to_lv(0x0A0E15), 0);
            lv_obj_set_style_border_width(s_st_slots[slot], 1, 0);
            lv_obj_set_style_border_color(s_st_slots[slot], hex_to_lv(is_playing ? 0x10B981 : 0x151D2A), 0);
            lv_obj_set_style_text_color(s_st_titles[slot], hex_to_lv((slot == 1 || slot == 3) ? 0xCBD5E1 : 0x64748B), 0);
        }

        lv_label_set_text(s_st_titles[slot], st.name);
        if (is_playing) {
            lv_obj_set_width(s_st_titles[slot], SLOT_W[slot] - 102);
            lv_label_set_text(s_st_tags[slot], "PLAYING");
            lv_obj_set_style_text_color(s_st_tags[slot], hex_to_lv(0x10B981), 0);
        } else {
            lv_obj_set_width(s_st_titles[slot], SLOT_W[slot] - 38);
            lv_label_set_text(s_st_tags[slot], "");
        }
    }
    xSemaphoreGive(g_app.mutex);

    set_arc_state(count > 1 ? count - 1 : 1, s_station_cursor);
}

static void update_speaker_wheel(bool sync_cursor_to_active) {
    if (!s_sp_slots[0]) return;

    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(30)) != pdTRUE) return;
    int count = g_app.speaker_count;
    int active_idx = g_app.current_speaker;
    bool scanning = g_app.speaker_scanning;
    if (count > MAX_SPEAKERS) count = MAX_SPEAKERS;
    if (count <= 0) {
        xSemaphoreGive(g_app.mutex);
        return;
    }

    if (sync_cursor_to_active || s_speaker_cursor < 0 || s_speaker_cursor >= count) {
        s_speaker_cursor = active_idx;
    }

    if (s_lbl_spkr_hdr) {
        if (scanning) {
            lv_label_set_text(s_lbl_spkr_hdr, "SCANNING AIRPLAY...");
        } else {
            lv_label_set_text_fmt(s_lbl_spkr_hdr, "AIRPLAY  •  %d / %d", s_speaker_cursor + 1, count);
        }
    }

    for (int slot = 0; slot < WHEEL_SLOTS; slot++) {
        if (slot >= count) {
            lv_obj_add_flag(s_sp_slots[slot], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_sp_slots[slot], LV_OBJ_FLAG_HIDDEN);

        int rel = (count >= WHEEL_SLOTS) ? (slot - 2) : slot;
        int idx = (count >= WHEEL_SLOTS)
                  ? ((s_speaker_cursor + rel + count * 4) % count)
                  : slot;

        const AirPlaySpeaker &sp = g_app.speakers[idx];
        bool is_center = (count >= WHEEL_SLOTS) ? (slot == 2) : (idx == s_speaker_cursor);
        bool is_active = (idx == active_idx);

        lv_obj_set_user_data(s_sp_slots[slot], (void *)(intptr_t)idx);
        lv_obj_set_style_bg_color(s_sp_dots[slot], hex_to_lv(is_active ? 0x10B981 : (is_center ? 0x38BDF8 : 0x334155)), 0);

        if (is_center) {
            lv_obj_set_style_bg_color(s_sp_slots[slot], hex_to_lv(0x141B28), 0);
            lv_obj_set_style_border_width(s_sp_slots[slot], 2, 0);
            lv_obj_set_style_border_color(s_sp_slots[slot], hex_to_lv(0x38BDF8), 0);
            lv_obj_set_style_text_color(s_sp_titles[slot], hex_to_lv(0xFFFFFF), 0);
        } else {
            lv_obj_set_style_bg_color(s_sp_slots[slot], hex_to_lv(0x0A0E15), 0);
            lv_obj_set_style_border_width(s_sp_slots[slot], 1, 0);
            lv_obj_set_style_border_color(s_sp_slots[slot], hex_to_lv(is_active ? 0x10B981 : 0x151D2A), 0);
            lv_obj_set_style_text_color(s_sp_titles[slot], hex_to_lv((slot == 1 || slot == 3) ? 0xCBD5E1 : 0x64748B), 0);
        }

        static const int SP_SLOT_W[WHEEL_SLOTS] = {252, 282, 300, 282, 252};
        lv_label_set_text(s_sp_titles[slot], sp.name);
        if (is_active) {
            lv_obj_set_width(s_sp_titles[slot], SP_SLOT_W[slot] - 96);
            lv_label_set_text(s_sp_tags[slot], "ACTIVE");
            lv_obj_set_style_text_color(s_sp_tags[slot], hex_to_lv(0x10B981), 0);
        } else if (is_center) {
            lv_obj_set_width(s_sp_titles[slot], SP_SLOT_W[slot] - 104);
            if (sp.is_local_dac) {
                lv_label_set_text(s_sp_tags[slot], "3.5MM");
                lv_obj_set_style_text_color(s_sp_tags[slot], hex_to_lv(0x94A3B8), 0);
            } else if (strcasestr(sp.am, "AudioAccessory") != NULL) {
                lv_label_set_text(s_sp_tags[slot], "HOMEPOD");
                lv_obj_set_style_text_color(s_sp_tags[slot], hex_to_lv(0x38BDF8), 0);
            } else {
                lv_label_set_text(s_sp_tags[slot], "AIRPLAY");
                lv_obj_set_style_text_color(s_sp_tags[slot], hex_to_lv(0x64748B), 0);
            }
        } else {
            lv_obj_set_width(s_sp_titles[slot], SP_SLOT_W[slot] - 38);
            lv_label_set_text(s_sp_tags[slot], "");
        }
    }
    xSemaphoreGive(g_app.mutex);

    set_arc_state(count > 1 ? count - 1 : 1, s_speaker_cursor);
}

static void switch_tab(ActiveTab tab) {
    s_active_tab = tab;
    for (int i = 0; i < 4; i++) {
        if (!s_pages[i]) continue;
        if (i == (int)tab) lv_obj_clear_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (tab == TAB_STATIONS) {
        update_station_wheel(true);
    } else if (tab == TAB_SPEAKERS) {
        update_speaker_wheel(true);
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
        // Pre-seed terrain history with gentle organic waves so frame 1 looks alive immediately
        memset(s_wc_grid, 0, sizeof(s_wc_grid));
        for (int r = 1; r < WC_ROWS; r++) {
            for (int c = 1; c < WC_COLS - 1; c++) {
                s_wc_grid[r][c] = (uint8_t)(6.0f + 5.0f * sinf(c * 0.35f + r * 0.25f));
            }
        }
        int8_t zero_w[128] = {0};
        uint8_t zero_vu[12] = {0};
        step_whitecap_simulation(zero_w, zero_w, zero_vu, 18, g_app.whitecap_preset & 3);
        s_wc_prev_frame_ymin = 0;

        // Hide underlying page and perimeter arc while full-screen WhiteCap is running so LVGL
        // does zero redundant background/arc composition work during 60 FPS frame blits!
        if (s_pages[s_active_tab]) lv_obj_add_flag(s_pages[s_active_tab], LV_OBJ_FLAG_HIDDEN);
        if (s_outer_arc) lv_obj_add_flag(s_outer_arc, LV_OBJ_FLAG_HIDDEN);

        lv_obj_clear_flag(s_wc_layer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_wc_layer);
        int preset = g_app.whitecap_preset & 3;
        show_whitecap_caption(lv_label_get_text(s_lbl_track), WHITECAP_PRESET_NAMES[preset], 2600);
        haptic_play_effect(HAPTIC_DOUBLE_CLICK);
    } else {
        lv_obj_add_flag(s_wc_layer, LV_OBJ_FLAG_HIDDEN);
        if (s_pages[s_active_tab]) lv_obj_clear_flag(s_pages[s_active_tab], LV_OBJ_FLAG_HIDDEN);
        if (s_outer_arc) lv_obj_clear_flag(s_outer_arc, LV_OBJ_FLAG_HIDDEN);
        haptic_play_effect(HAPTIC_SHARP_CLICK);
    }
}

static void handle_swipe_gesture(bsp_swipe_dir_t dir) {
    if (dir == BSP_SWIPE_NONE) return;

    // Close Wi-Fi scan modal if open and swiped
    if (s_wifi_modal && !s_wifi_kb) {
        lv_obj_del(s_wifi_modal);
        s_wifi_modal = NULL;
        s_wifi_list = NULL;
        haptic_play_effect(HAPTIC_SHARP_CLICK);
        return;
    }

    // WhiteCap Visualizer active:
    // - Swipe Down (or Up) -> Exit WhiteCap back to Home
    // - Swipe Left / Right -> Cycle 3D terrain color palette
    if (g_app.whitecap_active) {
        if (dir == BSP_SWIPE_DOWN || dir == BSP_SWIPE_UP) {
            set_whitecap_visible(false);
        } else if (dir == BSP_SWIPE_LEFT || dir == BSP_SWIPE_RIGHT) {
            int next_p = 0;
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                next_p = (g_app.whitecap_preset + (dir == BSP_SWIPE_LEFT ? 1 : 3)) & 3;
                g_app.whitecap_preset = next_p;
                xSemaphoreGive(g_app.mutex);
            }
            haptic_play_effect(HAPTIC_LIGHT_TICK);
            show_whitecap_caption(lv_label_get_text(s_lbl_track), WHITECAP_PRESET_NAMES[next_p], 2800);
        }
        return;
    }

    // Home Screen (TAB_NOW_PLAYING): 4-Direction Swipe Navigation
    // - Swipe Left  -> Stations Wheel
    // - Swipe Right -> AirPlay Speakers Wheel
    // - Swipe Up    -> Old-School WhiteCap 3D Visualizer
    // - Swipe Down  -> Settings (Wi-Fi / Web QR / AirPlay Rescan)
    if (s_active_tab == TAB_NOW_PLAYING) {
        if (dir == BSP_SWIPE_LEFT) {
            switch_tab(TAB_STATIONS);
        } else if (dir == BSP_SWIPE_RIGHT) {
            switch_tab(TAB_SPEAKERS);
        } else if (dir == BSP_SWIPE_UP) {
            set_whitecap_visible(true);
        } else if (dir == BSP_SWIPE_DOWN) {
            switch_tab(TAB_WIFI);
        }
        return;
    }

    // Stations or AirPlay Speakers Wheel: Horizontal swipe returns to Home
    if (s_active_tab == TAB_STATIONS || s_active_tab == TAB_SPEAKERS) {
        if (dir == BSP_SWIPE_LEFT || dir == BSP_SWIPE_RIGHT) {
            switch_tab(TAB_NOW_PLAYING);
        }
        return;
    }

    // Settings (TAB_WIFI): Swipe Up / Left / Right returns to Home
    if (s_active_tab == TAB_WIFI) {
        if (dir == BSP_SWIPE_UP || dir == BSP_SWIPE_LEFT || dir == BSP_SWIPE_RIGHT) {
            switch_tab(TAB_NOW_PLAYING);
        }
        return;
    }
}

void knob_ui_set_tab(int tab) {
    if (tab < 0 || tab > 3) return;
    if (!bsp_lvgl_lock(50)) return;
    set_whitecap_visible(false);
    switch_tab((ActiveTab)tab);
    bsp_lvgl_unlock();
}

int knob_ui_get_tab(void) {
    return (int)s_active_tab;
}

void knob_ui_set_whitecap(bool visible) {
    if (!bsp_lvgl_lock(50)) return;
    set_whitecap_visible(visible);
    bsp_lvgl_unlock();
}

void knob_ui_on_swipe(int dir) {
    if (dir < (int)BSP_SWIPE_LEFT || dir > (int)BSP_SWIPE_DOWN) return;
    bsp_push_swipe_gesture((bsp_swipe_dir_t)dir);
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
            s_wifi_kb = NULL;
        }
    } else if (code == LV_EVENT_CANCEL) {
        if (s_wifi_modal) {
            lv_obj_del(s_wifi_modal);
            s_wifi_modal = NULL;
            s_wifi_list = NULL;
            s_wifi_kb = NULL;
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
    if (bsp_was_touch_dragged()) return;
    haptic_play_effect(HAPTIC_SHARP_CLICK);
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        g_app.wifi_scan_requested = true;
        xSemaphoreGive(g_app.mutex);
    }

    if (s_wifi_modal) lv_obj_del(s_wifi_modal);
    s_wifi_kb = NULL;
    s_wifi_modal = lv_obj_create(s_scr);
    lv_obj_set_size(s_wifi_modal, 360, 360);
    lv_obj_center(s_wifi_modal);
    lv_obj_set_style_radius(s_wifi_modal, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_wifi_modal, hex_to_lv(0x050608), 0);
    lv_obj_set_style_border_width(s_wifi_modal, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_modal, 0, 0);

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
    lv_obj_set_style_pad_all(close_btn, 0, 0);
    lv_obj_t *cl = lv_label_create(close_btn);
    lv_label_set_text(cl, "Close");
    lv_obj_center(cl);
    lv_obj_add_event_cb(close_btn, [](lv_event_t *ev) {
        (void)ev;
        if (s_wifi_modal) {
            lv_obj_del(s_wifi_modal);
            s_wifi_modal = NULL;
            s_wifi_list = NULL;
            s_wifi_kb = NULL;
        }
    }, LV_EVENT_CLICKED, NULL);
}

// Helper to create a clean zero-padding pill button
static lv_obj_t *create_pill_btn(lv_obj_t *parent, int w, int h, uint32_t bg_hex, uint32_t border_hex, const char *text, uint32_t text_hex) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, h / 2, 0);
    lv_obj_set_style_bg_color(btn, hex_to_lv(bg_hex), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, hex_to_lv(border_hex), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, hex_to_lv(text_hex), 0);
    lv_obj_center(lbl);
    return btn;
}

// Touch drag + tap handlers for 60 FPS virtualized station wheel
static void station_slot_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_st_drag_accum = 0;
        s_st_dragging = false;
    } else if (code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_get_act();
        if (!indev) return;
        lv_point_t vect;
        lv_indev_get_vect(indev, &vect);
        s_st_drag_accum += vect.y;
        if (abs(s_st_drag_accum) >= 24) {
            int steps = s_st_drag_accum / 24;
            s_st_drag_accum -= steps * 24;
            s_st_dragging = true;
            int count = g_app.preset_count > 0 ? g_app.preset_count : 1;
            s_station_cursor = (s_station_cursor - steps + count * 8) % count;
            haptic_play_effect(HAPTIC_LIGHT_TICK);
            update_station_wheel(false);
        }
    } else if (code == LV_EVENT_CLICKED) {
        if (s_st_dragging || bsp_was_touch_dragged()) return;
        lv_obj_t *target = lv_event_get_current_target(e);
        int idx = (int)(intptr_t)lv_obj_get_user_data(target);
        s_station_cursor = idx;
        haptic_play_effect(HAPTIC_DOUBLE_CLICK);
        app_select_preset(idx, true);
        switch_tab(TAB_NOW_PLAYING);
    }
}

// Touch drag + tap handlers for 60 FPS virtualized speaker wheel
static void speaker_slot_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_sp_drag_accum = 0;
        s_sp_dragging = false;
    } else if (code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_get_act();
        if (!indev) return;
        lv_point_t vect;
        lv_indev_get_vect(indev, &vect);
        s_sp_drag_accum += vect.y;
        if (abs(s_sp_drag_accum) >= 24) {
            int steps = s_sp_drag_accum / 24;
            s_sp_drag_accum -= steps * 24;
            s_sp_dragging = true;
            int count = g_app.speaker_count > 0 ? g_app.speaker_count : 1;
            s_speaker_cursor = (s_speaker_cursor - steps + count * 8) % count;
            haptic_play_effect(HAPTIC_LIGHT_TICK);
            update_speaker_wheel(false);
        }
    } else if (code == LV_EVENT_CLICKED) {
        if (s_sp_dragging || bsp_was_touch_dragged()) return;
        lv_obj_t *target = lv_event_get_current_target(e);
        int idx = (int)(intptr_t)lv_obj_get_user_data(target);
        s_speaker_cursor = idx;
        haptic_play_effect(HAPTIC_DOUBLE_CLICK);
        app_select_speaker(idx);
        switch_tab(TAB_NOW_PLAYING);
    }
}

void knob_ui_init(void) {
    if (!bsp_lvgl_lock(-1)) return;

    s_scr = lv_scr_act();
    lv_obj_set_style_bg_color(s_scr, hex_to_lv(0x050608), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 4; i++) {
        s_pages[i] = lv_obj_create(s_scr);
        lv_obj_set_size(s_pages[i], 360, 360);
        lv_obj_center(s_pages[i]);
        lv_obj_set_style_bg_opa(s_pages[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_pages[i], 0, 0);
        lv_obj_set_style_pad_all(s_pages[i], 0, 0);
        lv_obj_clear_flag(s_pages[i], LV_OBJ_FLAG_SCROLLABLE);
        if (i != 0) lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Subtle 3px 360-degree bezel progress ring floating above pages
    s_outer_arc = lv_arc_create(s_scr);
    lv_obj_set_size(s_outer_arc, 356, 356);
    lv_obj_center(s_outer_arc);
    lv_arc_set_rotation(s_outer_arc, 270);
    lv_arc_set_bg_angles(s_outer_arc, 0, 360);
    lv_obj_remove_style(s_outer_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_outer_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_outer_arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_outer_arc, hex_to_lv(0x111827), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_outer_arc, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_outer_arc, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_outer_arc, hex_to_lv(0x10B981), LV_PART_INDICATOR);
    s_prev_theme_col = 0x10B981;
    set_arc_state(100, 18);

    // =========================================================================
    // PAGE 0: FULL-SCREEN ALBUM ART (360x360) + BOTTOM SCRIM OVERLAY
    // Swipe Left -> Stations | Swipe Right -> AirPlay | Swipe Up -> WhiteCap | Swipe Down -> Settings
    // =========================================================================
    lv_obj_t *p0 = s_pages[0];

    s_art_cont = lv_obj_create(p0);
    lv_obj_set_size(s_art_cont, ARTWORK_SIZE, ARTWORK_SIZE);
    lv_obj_center(s_art_cont);
    lv_obj_set_style_radius(s_art_cont, 0, 0);
    lv_obj_set_style_bg_opa(s_art_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_art_cont, 0, 0);
    lv_obj_set_style_pad_all(s_art_cont, 0, 0);
    lv_obj_clear_flag(s_art_cont, LV_OBJ_FLAG_SCROLLABLE);

    s_art_img = lv_img_create(s_art_cont);
    lv_img_set_src(s_art_img, artwork_get_dsc());
    lv_obj_center(s_art_img);
    lv_obj_add_flag(s_art_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_art_img, [](lv_event_t *e) {
        (void)e;
        if (bsp_was_touch_dragged()) return;
        lv_point_t pt;
        lv_indev_get_point(lv_indev_get_act(), &pt);
        if (pt.y >= 285) {
            // Tapping the bottom station/speaker line cycles Knob Mode
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
        } else {
            haptic_play_effect(HAPTIC_STRONG_CLICK);
            app_toggle_play();
        }
    }, LV_EVENT_CLICKED, NULL);

    // Subtle frosted pause indicator on artwork when paused
    s_pause_badge = lv_obj_create(p0);
    lv_obj_set_size(s_pause_badge, 52, 52);
    lv_obj_align(s_pause_badge, LV_ALIGN_CENTER, 0, -24);
    lv_obj_set_style_radius(s_pause_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_pause_badge, hex_to_lv(0x050608), 0);
    lv_obj_set_style_bg_opa(s_pause_badge, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_pause_badge, 1, 0);
    lv_obj_set_style_border_color(s_pause_badge, hex_to_lv(0xF8FAFC), 0);
    lv_obj_set_style_pad_all(s_pause_badge, 0, 0);
    lv_obj_clear_flag(s_pause_badge, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *pl = lv_label_create(s_pause_badge);
    lv_label_set_text(pl, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(pl, hex_to_lv(0xF8FAFC), 0);
    lv_obj_center(pl);
    lv_obj_add_flag(s_pause_badge, LV_OBJ_FLAG_HIDDEN);

    // Bottom Overlay Typography over the pre-baked full-screen artwork scrim:
    // 1. Track Name (y = 244..268, bold white, single-line ellipsis)
    s_lbl_track = lv_label_create(p0);
    lv_label_set_text(s_lbl_track, "Groove Salad");
    lv_obj_set_style_text_font(s_lbl_track, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_track, hex_to_lv(0xFFFFFF), 0);
    lv_label_set_long_mode(s_lbl_track, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_lbl_track, 292, 24);
    lv_obj_set_style_text_align(s_lbl_track, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_track, LV_ALIGN_TOP_MID, 0, 244);
    lv_obj_clear_flag(s_lbl_track, LV_OBJ_FLAG_CLICKABLE);

    // 2. Artist Name (y = 272..290, crisp silver, single-line ellipsis)
    s_lbl_artist = lv_label_create(p0);
    lv_label_set_text(s_lbl_artist, "SomaFM Radio");
    lv_obj_set_style_text_font(s_lbl_artist, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_artist, hex_to_lv(0xE2E8F0), 0);
    lv_label_set_long_mode(s_lbl_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_lbl_artist, 264, 18);
    lv_obj_set_style_text_align(s_lbl_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_artist, LV_ALIGN_TOP_MID, 0, 272);
    lv_obj_clear_flag(s_lbl_artist, LV_OBJ_FLAG_CLICKABLE);

    // 3. Station • AirPlay Speaker status line (y = 296..312, station theme accent color)
    s_lbl_sub_meta = lv_label_create(p0);
    lv_label_set_text(s_lbl_sub_meta, "Groove Salad  •  Tree House");
    lv_obj_set_style_text_font(s_lbl_sub_meta, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_sub_meta, hex_to_lv(0x10B981), 0);
    lv_label_set_long_mode(s_lbl_sub_meta, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_lbl_sub_meta, 248, 16);
    lv_obj_set_style_text_align(s_lbl_sub_meta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_sub_meta, LV_ALIGN_TOP_MID, 0, 296);
    lv_obj_clear_flag(s_lbl_sub_meta, LV_OBJ_FLAG_CLICKABLE);

    // =========================================================================
    // PAGE 1: 60 FPS VIRTUALIZED SOMAFM STATIONS WHEEL (Swipe Left from Home)
    // =========================================================================
    lv_obj_t *p1 = s_pages[1];

    s_lbl_st_hdr = lv_label_create(p1);
    lv_label_set_text(s_lbl_st_hdr, "STATIONS  •  1 / 30");
    lv_obj_set_style_text_font(s_lbl_st_hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_st_hdr, hex_to_lv(0x94A3B8), 0);
    lv_obj_align(s_lbl_st_hdr, LV_ALIGN_TOP_MID, 0, 26);

    static const int SLOT_Y[WHEEL_SLOTS] = {56, 98, 140, 188, 230};
    static const int SLOT_W[WHEEL_SLOTS] = {252, 282, 300, 282, 252};
    static const int SLOT_H[WHEEL_SLOTS] = {38, 38, 44, 38, 38};

    for (int i = 0; i < WHEEL_SLOTS; i++) {
        lv_obj_t *row = lv_obj_create(p1);
        lv_obj_set_size(row, SLOT_W[i], SLOT_H[i]);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, SLOT_Y[i]);
        lv_obj_set_style_radius(row, SLOT_H[i] / 2, 0);
        lv_obj_set_style_bg_color(row, hex_to_lv(i == 2 ? 0x141B28 : 0x0A0E15), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, i == 2 ? 2 : 1, 0);
        lv_obj_set_style_border_color(row, hex_to_lv(i == 2 ? 0x10B981 : 0x151D2A), 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, station_slot_event_cb, LV_EVENT_ALL, NULL);

        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_set_size(dot, 6, i == 2 ? 22 : 16);
        lv_obj_set_style_radius(dot, 3, 0);
        lv_obj_set_style_bg_color(dot, hex_to_lv(0x10B981), 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(row);
        lv_obj_set_style_text_font(title, i == 2 ? &lv_font_montserrat_16 : &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(title, hex_to_lv(0xFFFFFF), 0);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_size(title, SLOT_W[i] - 102, i == 2 ? 20 : 18);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 26, 0);

        lv_obj_t *tag = lv_label_create(row);
        lv_obj_set_style_text_font(tag, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(tag, hex_to_lv(0x64748B), 0);
        lv_label_set_long_mode(tag, LV_LABEL_LONG_CLIP);
        lv_obj_set_size(tag, 64, 16);
        lv_obj_set_style_text_align(tag, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(tag, LV_ALIGN_RIGHT_MID, -12, 0);

        s_st_slots[i] = row;
        s_st_dots[i] = dot;
        s_st_titles[i] = title;
        s_st_tags[i] = tag;
    }

    lv_obj_t *st_hint = lv_label_create(p1);
    lv_label_set_text(st_hint, "TAP TO TUNE  •  SWIPE BACK");
    lv_obj_set_style_text_font(st_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(st_hint, hex_to_lv(0x475569), 0);
    lv_obj_align(st_hint, LV_ALIGN_TOP_MID, 0, 288);

    update_station_wheel(true);

    // =========================================================================
    // PAGE 2: 60 FPS VIRTUALIZED AIRPLAY SPEAKERS WHEEL (Swipe Right from Home)
    // =========================================================================
    lv_obj_t *p2 = s_pages[2];

    s_lbl_spkr_hdr = lv_label_create(p2);
    lv_label_set_text(s_lbl_spkr_hdr, "AIRPLAY SPEAKERS");
    lv_obj_set_style_text_font(s_lbl_spkr_hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_spkr_hdr, hex_to_lv(0x94A3B8), 0);
    lv_obj_align(s_lbl_spkr_hdr, LV_ALIGN_TOP_MID, 0, 26);

    for (int i = 0; i < WHEEL_SLOTS; i++) {
        lv_obj_t *row = lv_obj_create(p2);
        lv_obj_set_size(row, SLOT_W[i], SLOT_H[i]);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, SLOT_Y[i]);
        lv_obj_set_style_radius(row, SLOT_H[i] / 2, 0);
        lv_obj_set_style_bg_color(row, hex_to_lv(i == 2 ? 0x141B28 : 0x0A0E15), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, i == 2 ? 2 : 1, 0);
        lv_obj_set_style_border_color(row, hex_to_lv(i == 2 ? 0x38BDF8 : 0x151D2A), 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, speaker_slot_event_cb, LV_EVENT_ALL, NULL);

        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_set_size(dot, 6, i == 2 ? 22 : 16);
        lv_obj_set_style_radius(dot, 3, 0);
        lv_obj_set_style_bg_color(dot, hex_to_lv(0x38BDF8), 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(row);
        lv_obj_set_style_text_font(title, i == 2 ? &lv_font_montserrat_16 : &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(title, hex_to_lv(0xFFFFFF), 0);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_size(title, SLOT_W[i] - 104, i == 2 ? 20 : 18);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 26, 0);

        lv_obj_t *tag = lv_label_create(row);
        lv_obj_set_style_text_font(tag, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(tag, hex_to_lv(0x38BDF8), 0);
        lv_label_set_long_mode(tag, LV_LABEL_LONG_CLIP);
        lv_obj_set_size(tag, 66, 16);
        lv_obj_set_style_text_align(tag, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(tag, LV_ALIGN_RIGHT_MID, -12, 0);

        s_sp_slots[i] = row;
        s_sp_dots[i] = dot;
        s_sp_titles[i] = title;
        s_sp_tags[i] = tag;
    }

    lv_obj_t *sp_hint = lv_label_create(p2);
    lv_label_set_text(sp_hint, "TAP TO CONNECT  •  SWIPE BACK");
    lv_obj_set_style_text_font(sp_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sp_hint, hex_to_lv(0x475569), 0);
    lv_obj_align(sp_hint, LV_ALIGN_TOP_MID, 0, 288);

    update_speaker_wheel(true);

    // =========================================================================
    // PAGE 3: SETTINGS SHEET (Swipe Down from Home: Wi-Fi, Web QR, AirPlay Scan)
    // =========================================================================
    lv_obj_t *p3 = s_pages[3];

    s_lbl_wifi_ssid = lv_label_create(p3);
    lv_label_set_text(s_lbl_wifi_ssid, LV_SYMBOL_WIFI " SvensHaus");
    lv_obj_set_style_text_font(s_lbl_wifi_ssid, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_wifi_ssid, hex_to_lv(0xF8FAFC), 0);
    lv_obj_align(s_lbl_wifi_ssid, LV_ALIGN_TOP_MID, 0, 24);

    s_lbl_wifi_ip = lv_label_create(p3);
    lv_label_set_text(s_lbl_wifi_ip, "http://airplayknob.local/");
    lv_obj_set_style_text_font(s_lbl_wifi_ip, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_wifi_ip, hex_to_lv(0x38BDF8), 0);
    lv_obj_align(s_lbl_wifi_ip, LV_ALIGN_TOP_MID, 0, 46);

    lv_obj_t *qr_card = lv_obj_create(p3);
    lv_obj_set_size(qr_card, 132, 132);
    lv_obj_align(qr_card, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_set_style_radius(qr_card, 16, 0);
    lv_obj_set_style_bg_color(qr_card, hex_to_lv(0xFFFFFF), 0);
    lv_obj_set_style_border_width(qr_card, 0, 0);
    lv_obj_set_style_pad_all(qr_card, 0, 0);
    lv_obj_clear_flag(qr_card, LV_OBJ_FLAG_SCROLLABLE);

    s_qr_code = lv_qrcode_create(qr_card, 114, hex_to_lv(0x050608), hex_to_lv(0xFFFFFF));
    lv_qrcode_update(s_qr_code, "http://airplayknob.local/", strlen("http://airplayknob.local/"));
    lv_obj_center(s_qr_code);

    lv_obj_t *ota_lbl = lv_label_create(p3);
    lv_label_set_text(ota_lbl, "Web Presets & OTA Ready");
    lv_obj_set_style_text_font(ota_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ota_lbl, hex_to_lv(0x64748B), 0);
    lv_obj_align(ota_lbl, LV_ALIGN_TOP_MID, 0, 214);

    lv_obj_t *w_scan_btn = create_pill_btn(p3, 102, 32, 0x0F141D, 0x1E293B, "Wi-Fi Scan", 0xCBD5E1);
    lv_obj_align(w_scan_btn, LV_ALIGN_TOP_MID, -56, 244);
    lv_obj_add_event_cb(w_scan_btn, open_wifi_scan_modal, LV_EVENT_CLICKED, NULL);

    lv_obj_t *w_airplay_btn = create_pill_btn(p3, 106, 32, 0x0F141D, 0x1E293B, "Scan AirPlay", 0x38BDF8);
    lv_obj_align(w_airplay_btn, LV_ALIGN_TOP_MID, 56, 244);
    lv_obj_add_event_cb(w_airplay_btn, [](lv_event_t *e) {
        (void)e;
        if (bsp_was_touch_dragged()) return;
        haptic_play_effect(HAPTIC_SHARP_CLICK);
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_app.speaker_scan_requested = true;
            g_app.speaker_scanning = true;
            xSemaphoreGive(g_app.mutex);
        }
        show_hud("AIRPLAY", "Scanning Network...", 0x38BDF8, 1400);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *set_hint = lv_label_create(p3);
    lv_label_set_text(set_hint, "SWIPE UP FOR HOME");
    lv_obj_set_style_text_font(set_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(set_hint, hex_to_lv(0x475569), 0);
    lv_obj_align(set_hint, LV_ALIGN_TOP_MID, 0, 290);

    // =========================================================================
    // FULL-SCREEN WHITECAP VISUALIZER LAYER (360x360, Swipe Up from Home)
    // Renders directly into internal DMA SRAM strips via LV_EVENT_DRAW_MAIN!
    // =========================================================================
    s_wc_layer = lv_obj_create(s_scr);
    lv_obj_set_size(s_wc_layer, WC_W, WC_H);
    lv_obj_center(s_wc_layer);
    lv_obj_set_style_bg_color(s_wc_layer, hex_to_lv(0x000000), 0);
    lv_obj_set_style_bg_opa(s_wc_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wc_layer, 0, 0);
    lv_obj_set_style_pad_all(s_wc_layer, 0, 0);
    lv_obj_clear_flag(s_wc_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wc_layer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_wc_layer, wc_layer_draw_event_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_wc_layer, [](lv_event_t *e) {
        (void)e;
        if (bsp_was_touch_dragged()) return;
        lv_point_t pt;
        lv_indev_get_point(lv_indev_get_act(), &pt);
        if (pt.y < 120) {
            int next_p = 0;
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                next_p = (g_app.whitecap_preset + 1) & 3;
                g_app.whitecap_preset = next_p;
                xSemaphoreGive(g_app.mutex);
            }
            haptic_play_effect(HAPTIC_LIGHT_TICK);
            show_whitecap_caption(lv_label_get_text(s_lbl_track), WHITECAP_PRESET_NAMES[next_p], 2800);
        } else {
            set_whitecap_visible(false);
        }
    }, LV_EVENT_CLICKED, NULL);

    s_wc_caption_box = lv_obj_create(s_wc_layer);
    lv_obj_set_size(s_wc_caption_box, 256, 52);
    lv_obj_align(s_wc_caption_box, LV_ALIGN_TOP_MID, 0, 272);
    lv_obj_set_style_radius(s_wc_caption_box, 26, 0);
    lv_obj_set_style_bg_color(s_wc_caption_box, hex_to_lv(0x050608), 0);
    lv_obj_set_style_bg_opa(s_wc_caption_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wc_caption_box, 1, 0);
    lv_obj_set_style_border_color(s_wc_caption_box, hex_to_lv(0x1E293B), 0);
    lv_obj_set_style_pad_all(s_wc_caption_box, 0, 0);
    lv_obj_clear_flag(s_wc_caption_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_wc_lbl_track = lv_label_create(s_wc_caption_box);
    lv_label_set_text(s_wc_lbl_track, "Groove Salad");
    lv_obj_set_style_text_font(s_wc_lbl_track, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_wc_lbl_track, hex_to_lv(0xF8FAFC), 0);
    lv_label_set_long_mode(s_wc_lbl_track, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_wc_lbl_track, 228, 18);
    lv_obj_set_style_text_align(s_wc_lbl_track, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wc_lbl_track, LV_ALIGN_TOP_MID, 0, 8);

    s_wc_lbl_sub = lv_label_create(s_wc_caption_box);
    lv_label_set_text(s_wc_lbl_sub, WHITECAP_PRESET_NAMES[0]);
    lv_obj_set_style_text_font(s_wc_lbl_sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_wc_lbl_sub, hex_to_lv(0x94A3B8), 0);
    lv_label_set_long_mode(s_wc_lbl_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_wc_lbl_sub, 228, 16);
    lv_obj_set_style_text_align(s_wc_lbl_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wc_lbl_sub, LV_ALIGN_TOP_MID, 0, 28);

    // Register 60+ FPS WhiteCap render timer inside LVGL
    lv_timer_create(whitecap_lv_timer_cb, 1, NULL);

    // =========================================================================
    // MINIMALIST FLOATING HUD PILL (Opaque cover when turning physical knob)
    // =========================================================================
    s_hud_pill = lv_obj_create(s_scr);
    lv_obj_set_size(s_hud_pill, 236, 56);
    lv_obj_align(s_hud_pill, LV_ALIGN_TOP_MID, 0, 258);
    lv_obj_set_style_radius(s_hud_pill, 28, 0);
    lv_obj_set_style_bg_color(s_hud_pill, hex_to_lv(0x0B0F19), 0);
    lv_obj_set_style_bg_opa(s_hud_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_hud_pill, 1, 0);
    lv_obj_set_style_border_color(s_hud_pill, hex_to_lv(0x10B981), 0);
    lv_obj_set_style_pad_all(s_hud_pill, 0, 0);
    lv_obj_clear_flag(s_hud_pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_hud_title = lv_label_create(s_hud_pill);
    lv_label_set_text(s_lbl_hud_title, "VOLUME");
    lv_obj_set_style_text_font(s_lbl_hud_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_hud_title, hex_to_lv(0x10B981), 0);
    lv_label_set_long_mode(s_lbl_hud_title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_lbl_hud_title, 196, 16);
    lv_obj_set_style_text_align(s_lbl_hud_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_hud_title, LV_ALIGN_TOP_MID, 0, 8);

    s_lbl_hud_val = lv_label_create(s_hud_pill);
    lv_label_set_text(s_lbl_hud_val, "18%");
    lv_obj_set_style_text_font(s_lbl_hud_val, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_hud_val, hex_to_lv(0xFFFFFF), 0);
    lv_label_set_long_mode(s_lbl_hud_val, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_lbl_hud_val, 204, 20);
    lv_obj_set_style_text_align(s_lbl_hud_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_hud_val, LV_ALIGN_TOP_MID, 0, 27);

    lv_obj_add_flag(s_hud_pill, LV_OBJ_FLAG_HIDDEN);

    bsp_lvgl_unlock();
}

void knob_ui_on_rotate(int delta) {
    if (delta == 0) return;
    if (!bsp_lvgl_lock(30)) return;

    // 60 FPS O(1) Virtualized Station Wheel Scroll
    if (!g_app.whitecap_active && s_active_tab == TAB_STATIONS) {
        int count = g_app.preset_count > 0 ? g_app.preset_count : 1;
        s_station_cursor = (s_station_cursor + (delta > 0 ? 1 : -1) + count * 8) % count;
        haptic_play_effect(HAPTIC_LIGHT_TICK);
        update_station_wheel(false);
        bsp_lvgl_unlock();
        return;
    }

    // 60 FPS O(1) Virtualized AirPlay Speaker Wheel Scroll
    if (!g_app.whitecap_active && s_active_tab == TAB_SPEAKERS) {
        int count = g_app.speaker_count > 0 ? g_app.speaker_count : 1;
        s_speaker_cursor = (s_speaker_cursor + (delta > 0 ? 1 : -1) + count * 8) % count;
        haptic_play_effect(HAPTIC_LIGHT_TICK);
        update_speaker_wheel(false);
        bsp_lvgl_unlock();
        return;
    }

    // Now Playing or WhiteCap Visualizer: Volume / Station Tuner / Speaker Selector
    KnobMode mode = KNOB_MODE_VOLUME;
    int vol = 18;
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
        set_arc_state(100, vol);
    } else if (mode == KNOB_MODE_STATION) {
        haptic_play_effect(HAPTIC_LIGHT_TICK);
        show_hud("TUNE STATION", preview_st.name, preview_st.color ? preview_st.color : 0xF59E0B, 1150);
        set_arc_state(p_cnt > 1 ? p_cnt - 1 : 1, p_preview);
    } else if (mode == KNOB_MODE_SPEAKER) {
        haptic_play_effect(HAPTIC_LIGHT_TICK);
        show_hud("AIRPLAY SPEAKER", preview_sp.name, 0x38BDF8, 1200);
        set_arc_state(s_cnt > 1 ? s_cnt - 1 : 1, s_preview);
    }

    bsp_lvgl_unlock();
}

void knob_ui_tick(void) {
    uint32_t now_ms = millis();

    // Fast path: check swipe gesture immediately without takingLVGL lock if idle
    bsp_swipe_dir_t swipe = bsp_pop_swipe_gesture();
    static uint32_t s_last_slow_tick_ms = 0;
    if (swipe == BSP_SWIPE_NONE && (now_ms - s_last_slow_tick_ms < 35)) {
        return;
    }
    s_last_slow_tick_ms = now_ms;

    int commit_preset = -1;
    int commit_speaker = -1;

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

    if (swipe != BSP_SWIPE_NONE) {
        handle_swipe_gesture(swipe);
    }

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
    int vol = 18;
    char raw_track[128] = {0};
    bool wifi_ok = false;
    char ssid[36] = {0};
    char ip[20] = {0};
    bool wc_active = false;
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
        wifi_ok = g_app.wifi_connected;
        strncpy(ssid, g_app.wifi_ssid, sizeof(ssid) - 1);
        strncpy(ip, g_app.wifi_ip, sizeof(ip) - 1);
        wc_active = g_app.whitecap_active;
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
        set_arc_state(100, ota_pct);
        bsp_lvgl_unlock();
        return;
    }

    // Refresh Speakers wheel automatically when mDNS scan finishes
    static bool prev_spk_scanning = false;
    static int prev_spk_count = 1;
    if ((prev_spk_scanning != spk_scanning) || (spk_count != prev_spk_count)) {
        prev_spk_scanning = spk_scanning;
        prev_spk_count = spk_count;
        if (s_active_tab == TAB_SPEAKERS) {
            update_speaker_wheel(false);
        }
    }

    // Check if new Album Art or Stream Art arrived from artwork_fetcher
    static uint32_t last_art_seq = 0;
    uint32_t cur_art_seq = artwork_get_seq();
    if (cur_art_seq != last_art_seq) {
        last_art_seq = cur_art_seq;
        if (s_art_img && !wc_active && s_active_tab == TAB_NOW_PLAYING) {
            lv_obj_invalidate(s_art_img);
        }
    }

    uint32_t theme_col = cur_st.color ? cur_st.color : 0x10B981;
    if (theme_col != s_prev_theme_col) {
        s_prev_theme_col = theme_col;
        lv_obj_set_style_arc_color(s_outer_arc, hex_to_lv(theme_col), LV_PART_INDICATOR);
        lv_obj_set_style_text_color(s_lbl_sub_meta, hex_to_lv(theme_col), 0);
    }

    static bool prev_playing = true;
    if (playing != prev_playing) {
        prev_playing = playing;
        if (playing) lv_obj_add_flag(s_pause_badge, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_clear_flag(s_pause_badge, LV_OBJ_FLAG_HIDDEN);
    }

    // Split "Artist - Track" and update minimalist bottom overlay typography
    char artist_str[80] = {0};
    char track_str[100] = {0};
    split_artist_track(raw_track, cur_st.name, artist_str, sizeof(artist_str), track_str, sizeof(track_str));

    static char prev_raw_track[128] = {0};
    if (strcmp(raw_track, prev_raw_track) != 0) {
        strncpy(prev_raw_track, raw_track, sizeof(prev_raw_track) - 1);
        lv_label_set_text(s_lbl_track, track_str);
        lv_label_set_text(s_lbl_artist, artist_str);
        if (wc_active) {
            show_whitecap_caption(track_str, artist_str, 4000);
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

    if (!wc_active && s_active_tab == TAB_NOW_PLAYING && s_hud_hide_ms == 0) {
        set_arc_state(100, vol);
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
                lv_obj_set_style_pad_all(b, 0, 0);
                lv_obj_add_event_cb(b, wifi_ssid_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
                lv_obj_t *l = lv_label_create(b);
                lv_label_set_text_fmt(l, "%s (%ld dBm)", items[i].ssid, (long)items[i].rssi);
                lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
                lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
            }
        }
        last_scanning = cur_scanning;
    }

    bsp_lvgl_unlock();
}

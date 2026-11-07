#include "knob_ui.h"
#include "ui_fonts.h"
#include "app_state.h"
#include "artwork_fetcher.h"
#include "audio_buffer_config.h"
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
// 60 FPS Virtualized 7-Slot Animated Dial Wheel for Stations (Tab 1) & Speakers (Tab 2)
// Zero dynamic allocation or flex-layout recalculation during scrolling!
// =============================================================================
#define WHEEL_SLOTS 7
#define WHEEL_CENTER_SLOT 3

struct WheelSlotCache {
    bool valid;
    bool hidden;
    bool tag_hidden;
    int item_idx;
    int tag_kind;
    int16_t x, y, w, h;
    int16_t dot_h;
    int16_t title_w;
    uint32_t bg_hex;
    uint32_t border_hex;
    uint8_t border_w;
    uint32_t dot_hex;
    uint32_t title_hex;
    uint32_t tag_hex;
    bool is_lg_font;
};

// Tab 1: Virtualized Stations Wheel
static lv_obj_t *s_lbl_st_hdr = NULL;
static lv_obj_t *s_st_slots[WHEEL_SLOTS] = {NULL};
static lv_obj_t *s_st_dots[WHEEL_SLOTS] = {NULL};
static lv_obj_t *s_st_titles[WHEEL_SLOTS] = {NULL};
static lv_obj_t *s_st_tags[WHEEL_SLOTS] = {NULL};
static WheelSlotCache s_st_cache[WHEEL_SLOTS] = {};
static int s_station_cursor = 0;
static int s_st_hdr_cursor = -1;
static int s_st_hdr_count = -1;
static float s_st_anim_offset = 0.0f;
static int s_st_drag_accum = 0;
static bool s_st_dragging = false;

// Tab 2: Virtualized AirPlay Speakers Wheel
static lv_obj_t *s_lbl_spkr_hdr = NULL;
static lv_obj_t *s_sp_slots[WHEEL_SLOTS] = {NULL};
static lv_obj_t *s_sp_dots[WHEEL_SLOTS] = {NULL};
static lv_obj_t *s_sp_titles[WHEEL_SLOTS] = {NULL};
static lv_obj_t *s_sp_tags[WHEEL_SLOTS] = {NULL};
static WheelSlotCache s_sp_cache[WHEEL_SLOTS] = {};
static int s_speaker_cursor = 0;
static int s_sp_hdr_cursor = -1;
static int s_sp_hdr_count = -1;
static bool s_sp_hdr_scanning = false;
static float s_sp_anim_offset = 0.0f;
static int s_sp_drag_accum = 0;
static bool s_sp_dragging = false;

static int64_t  s_wheel_anim_last_us = 0;
static bool     s_wheel_touch_armed  = false;
static bool     s_wheel_horiz_locked = false;
static int16_t  s_wheel_start_x      = 180;
static int16_t  s_wheel_start_y      = 180;
static int16_t  s_wheel_last_x       = 180;
static int16_t  s_wheel_last_y       = 180;
static int64_t  s_wheel_start_us     = 0;
static int64_t  s_wheel_last_us      = 0;
static float    s_wheel_vel_y        = 0.0f;
static int      s_wheel_items_moved  = 0;
static uint32_t s_wheel_last_tick_ms = 0;

static void play_wheel_tick_haptic(void) {
    uint32_t now_ms = millis();
    if (now_ms - s_wheel_last_tick_ms >= 35) {
        s_wheel_last_tick_ms = now_ms;
        haptic_play_effect(HAPTIC_LIGHT_TICK);
    }
}

static void handle_swipe_gesture(bsp_swipe_dir_t dir);

// Menu / Tab Swipe Transition Animation State
static bool      s_page_anim_active   = false;
static lv_obj_t *s_page_anim_from     = NULL;
static lv_obj_t *s_page_anim_to       = NULL;
static int8_t    s_page_anim_dir_x    = 0;
static int8_t    s_page_anim_dir_y    = 0;
static float     s_page_anim_progress = 0.0f;
static bool      s_prev_wc_active     = false;

// Tab 3: Settings (Live Buffers, Streaming Telemetry, Wi-Fi, Tap-for-QR & PIN Pairing)
static lv_obj_t *s_lbl_wifi_ssid = NULL;
static lv_obj_t *s_lbl_wifi_link = NULL;
static lv_obj_t *s_lbl_wifi_ip = NULL;
static lv_obj_t *s_lbl_buf_pcm = NULL;
static lv_obj_t *s_bar_buf_pcm = NULL;
static lv_obj_t *s_lbl_buf_mp3 = NULL;
static lv_obj_t *s_bar_buf_mp3 = NULL;
static lv_obj_t *s_lbl_stream_state = NULL;
static lv_obj_t *s_lbl_stream_pkts = NULL;
static lv_obj_t *s_qr_modal = NULL;
static lv_obj_t *s_qr_code = NULL;
static lv_obj_t *s_qr_lbl_url = NULL;
static lv_obj_t *s_wifi_modal = NULL;
static lv_obj_t *s_wifi_list = NULL;
static lv_obj_t *s_wifi_pass_ta = NULL;
static lv_obj_t *s_wifi_kb = NULL;
static char s_pending_ssid[36] = {0};
static bool s_pending_secure = true;
static uint32_t s_wifi_rendered_scan_seq = 0;

// Animated Startup / Wi-Fi Connecting Overlay
static lv_obj_t *s_conn_layer = NULL;
static lv_obj_t *s_conn_spinner_arc = NULL;
static lv_obj_t *s_conn_pulse_ring = NULL;
static lv_obj_t *s_conn_lbl_icon = NULL;
static lv_obj_t *s_conn_lbl_title = NULL;
static lv_obj_t *s_conn_lbl_ssid = NULL;
static lv_obj_t *s_conn_lbl_status = NULL;
static lv_obj_t *s_conn_lbl_speed = NULL;
static lv_obj_t *s_conn_lbl_ip = NULL;
static lv_obj_t *s_conn_wifi_btn = NULL;
static bool s_conn_dismissed = false;
static uint32_t s_conn_ready_ms = 0;
static uint16_t s_conn_anim_angle = 0;

// 4-Digit AirPlay PIN Entry Modal
static lv_obj_t *s_pin_modal = NULL;
static lv_obj_t *s_pin_lbl_spkr = NULL;
static lv_obj_t *s_pin_boxes[4] = {NULL, NULL, NULL, NULL};
static lv_obj_t *s_pin_box_lbls[4] = {NULL, NULL, NULL, NULL};
static char s_pin_digits[5] = {'0', '0', '0', '0', '\0'};
static int s_pin_cursor = 0;
static int s_pin_entered_len = 0;

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
static float s_wc_cam_time = 0.0f;
static float s_wc_color_drift = 0.0f;
static float s_wc_agc_peak = 32.0f;

// Stored in fast 240MHz internal SRAM (~4.6 KB total, zero PSRAM traffic!)
static int8_t  s_wc_wave[WC_ROWS][WC_COLS] = {{0}};
static int16_t s_wc_vx[WC_ROWS][WC_COLS] = {{0}};
static int16_t s_wc_vy[WC_ROWS][WC_COLS] = {{0}};
static int16_t s_wc_row_ymin[WC_ROWS] = {0};
static int16_t s_wc_row_ymax[WC_ROWS] = {0};
static uint16_t s_wc_col_ridge[WC_ROWS] = {0};
static uint16_t s_wc_col_long[WC_ROWS] = {0};
static int16_t s_wc_prev_frame_ymin = 0;
static int16_t s_wc_prev_frame_ymax = WC_H - 1;
static int16_t s_wc_cur_frame_ymin = 0;
static int16_t s_wc_cur_frame_ymax = WC_H - 1;
static bool s_wc_front_to_back = false;
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
static inline void wc_strip_line(uint16_t *buf, int bx1, int bx2, int by1, int by2, int bw,
                                 int x0, int y0, int x1, int y1, uint16_t col) {
    // Trivial vertical reject if segment is completely outside this DMA strip
    int seg_min_y = (y0 < y1) ? y0 : y1;
    int seg_max_y = (y0 > y1) ? y0 : y1;
    if (seg_max_y + 1 < by1 || seg_min_y > by2) return;

    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        if (x0 >= bx1 && x0 < bx2) {
            if (y0 >= by1 && y0 <= by2) {
                uint16_t *row = &buf[(y0 - by1) * bw + (x0 - bx1)];
                row[0] = col;
                row[1] = col;
            }
            if (y0 + 1 >= by1 && y0 + 1 <= by2) {
                buf[(y0 + 1 - by1) * bw + (x0 - bx1)] = col;
            }
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void wc_hsv_to_rgb(float h, float s, float v, uint8_t *r_out, uint8_t *g_out, uint8_t *b_out) {
    h = fmodf(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (h < 60.0f)       { r = c; g = x; b = 0; }
    else if (h < 120.0f) { r = x; g = c; b = 0; }
    else if (h < 180.0f) { r = 0; g = c; b = x; }
    else if (h < 240.0f) { r = 0; g = x; b = c; }
    else if (h < 300.0f) { r = x; g = 0; b = c; }
    else                 { r = c; g = 0; b = x; }
    *r_out = (uint8_t)((r + m) * 255.0f);
    *g_out = (uint8_t)((g + m) * 255.0f);
    *b_out = (uint8_t)((b + m) * 255.0f);
}

static void update_whitecap_palette(int preset) {
    // Base hue shift slowly drifts over time (~5 deg/sec) + 90 deg per manual preset
    float shift = s_wc_color_drift + (float)preset * 90.0f;

    for (int r = 0; r < WC_ROWS; r++) {
        uint8_t r8 = 255, g8 = 32, b8 = 16;
        if (r == 0) {
            // Row 0 is the live front-edge real-time oscilloscope waveform:
            // Starts at vibrant Neon Green (122 deg) and subtly shifts with the palette!
            float front_hue = 122.0f + shift;
            wc_hsv_to_rgb(front_hue, 0.88f, 1.0f, &r8, &g8, &b8);
            s_wc_col_ridge[0] = rgb_to_swapped565(r8, g8, b8);
            s_wc_col_long[0]  = s_wc_col_ridge[0];
        } else {
            // Rows 1..WC_ROWS-1 sweep from Red (4 deg) -> Orange/Gold (42 deg) -> Neon Green (118 deg)
            // plus the subtle continuous color drift `shift`
            float t = (float)(r - 1) / (float)(WC_ROWS - 2);
            float row_hue = 4.0f + t * 114.0f + shift;
            float sat = 0.95f - 0.08f * sinf(t * 3.14159f);
            float val = 0.98f - 0.14f * t;
            wc_hsv_to_rgb(row_hue, sat, val, &r8, &g8, &b8);
            s_wc_col_ridge[r] = rgb_to_swapped565(r8, g8, b8);
            s_wc_col_long[r]  = rgb_to_swapped565((uint8_t)((r8 * 165) >> 8),
                                                  (uint8_t)((g8 * 165) >> 8),
                                                  (uint8_t)((b8 * 165) >> 8));
        }
    }
}

static void step_whitecap_simulation(const int8_t *wl, const int8_t *wr, const uint8_t *vu, uint8_t peak, int preset) {
    s_wc_cur_preset = preset;

    // 1. Shift 3D terrain waterfall history back by 1 row (r = 0 -> 1 -> 2 ... -> 25)
    // Row 0 flows directly into Row 1..25 so every real-time waveform excursion on the front line
    // propagates backward across the 3D terrain with no discontinuity!
    for (int r = WC_ROWS - 1; r >= 2; r--) {
        memcpy(s_wc_wave[r], s_wc_wave[r - 1], WC_COLS * sizeof(int8_t));
    }
    for (int c = 0; c < WC_COLS; c++) {
        int prev_c = (c > 0) ? s_wc_wave[0][c - 1] : s_wc_wave[0][c];
        int next_c = (c + 1 < WC_COLS) ? s_wc_wave[0][c + 1] : s_wc_wave[0][c];
        s_wc_wave[1][c] = (int8_t)((prev_c + ((int)s_wc_wave[0][c] * 6) + next_c) >> 3);
    }

    // 2. Advance animation clocks:
    // - s_wc_phase drives organic ridge detail
    // - s_wc_cam_time smoothly rotates/orbits the 3D viewpoint
    // - s_wc_color_drift subtly evolves the color palette over time
    float energy = (float)peak / 100.0f;
    s_wc_phase       += 0.10f + energy * 0.08f;
    s_wc_cam_time    += 0.012f + energy * 0.006f;
    s_wc_color_drift += 0.075f + energy * 0.045f; // ~4.5 deg/sec subtle color morph
    if (s_wc_color_drift >= 360.0f) s_wc_color_drift -= 360.0f;

    update_whitecap_palette(preset);

    // 3. Automatic Gain Control (AGC) on live stereo PCM (wl, wr) so quiet and loud tracks
    // both produce a vivid real-time front waveform and 3D ridge heights
    int max_sample = 1;
    for (int i = 0; i < 128; i += 2) {
        int a = abs((int)wl[i]) + abs((int)wr[i]);
        if (a > max_sample) max_sample = a;
    }
    float target_peak = (float)max_sample * 0.55f;
    if (target_peak < 12.0f) target_peak = 12.0f;
    if (target_peak > s_wc_agc_peak) {
        s_wc_agc_peak = s_wc_agc_peak * 0.72f + target_peak * 0.28f;
    } else {
        s_wc_agc_peak = s_wc_agc_peak * 0.96f + target_peak * 0.04f;
    }
    float gain = 34.0f / s_wc_agc_peak;
    if (gain > 2.6f) gain = 2.6f;
    if (gain < 0.45f) gain = 0.45f;

    int8_t raw_front[WC_COLS];

    for (int c = 0; c < WC_COLS; c++) {
        int w_idx = (c * 127) / (WC_COLS - 1);
        int b_idx = (c * 11) / (WC_COLS - 1);

        float u = (float)c / (float)(WC_COLS - 1);
        // Classic WhiteCap profile: stronger bass/low-mid mountain ridges on the left, finer ripples on the right
        float left_boost = 1.55f - 0.95f * sqrtf(u);

        // Real-time signed stereo oscilloscope sample + positive spectrum carrier so the front line (r=0)
        // is a live real-time waveform that stays above the ground plane (y >= 0) as it flows into r=1..25
        float pcm_signed = ((float)wl[w_idx] * 0.65f + (float)wr[127 - w_idx] * 0.35f) * gain;
        float spec_lift  = (float)vu[b_idx] * 0.34f * left_boost + 14.0f * left_boost;
        float carrier    = 4.5f * sinf(c * 0.48f - s_wc_phase * 1.5f)
                         + 3.5f * cosf(c * 0.27f + s_wc_phase * 0.9f);

        float val = (spec_lift + pcm_signed * 0.85f * left_boost + carrier)
                  * (0.15f + 0.85f * sinf(u * 3.14159265f));
        if (c == 0 || c == WC_COLS - 1) val *= 0.20f;
        if (val < 0.0f)  val = 0.0f;
        if (val > 72.0f) val = 72.0f;
        raw_front[c] = (int8_t)val;
    }

    // Populate Row 0 (live real-time oscilloscope front line)
    for (int c = 0; c < WC_COLS; c++) {
        int pf = (c > 0) ? raw_front[c - 1] : raw_front[c];
        int nf = (c + 1 < WC_COLS) ? raw_front[c + 1] : raw_front[c];
        s_wc_wave[0][c] = (int8_t)((pf + (raw_front[c] << 1) + nf) >> 2);
    }

    // 4. 3D Rotating Viewpoint Projection (Yaw + Pitch + Roll + Perspective)
    // Smoothly sweeps yaw angle around the classic oblique view (-36 deg .. +28 deg + slow orbital sway),
    // while pitch looks down at the terrain (23 deg .. 33 deg) and roll banks subtly (-5 deg .. +5 deg)
    float yaw   = -0.26f + 0.52f * sinf(s_wc_cam_time * 0.65f) + 0.20f * cosf(s_wc_cam_time * 0.29f);
    float pitch =  0.48f + 0.10f * sinf(s_wc_cam_time * 0.43f + 1.1f);
    float roll  =  0.09f * sinf(s_wc_cam_time * 0.55f);

    float cy = cosf(yaw),   sy = sinf(yaw);
    float cp = cosf(pitch), sp = sinf(pitch);
    float cr = cosf(roll),  sr = sinf(roll);

    s_wc_front_to_back = (cy < 0.0f);

    int16_t frame_min_y = WC_H - 1;
    int16_t frame_max_y = 0;

    for (int r = 0; r < WC_ROWS; r++) {
        // Normalized z in [-1.0 (front live waveform r=0) .. +1.0 (far horizon r=WC_ROWS-1)]
        float z3 = (((float)r / (float)(WC_ROWS - 1)) * 2.0f - 1.0f) * 118.0f;

        int16_t r_min_y = WC_H - 1;
        int16_t r_max_y = 0;

        for (int c = 0; c < WC_COLS; c++) {
            // Normalized x in [-1.0 .. +1.0]
            float x3 = (((float)c / (float)(WC_COLS - 1)) * 2.0f - 1.0f) * 122.0f;
            // Vertical height y3 above the ground plane (-14 baseline centers the 3D terrain vertically)
            float y3 = (float)s_wc_wave[r][c] * 1.12f - 14.0f;

            // 1) Yaw around Y axis
            float x1 =  x3 * cy + z3 * sy;
            float z1 = -x3 * sy + z3 * cy;
            float y1 =  y3;

            // 2) Pitch around X axis (camera elevated looking down at ground plane:
            //    z1 < 0 [r=0] projects to bottom foreground; z1 > 0 [r=25] projects to top horizon)
            float x2 =  x1;
            float y2 =  y1 * cp + z1 * sp;
            float z2 = -y1 * sp + z1 * cp;

            // 3) Roll around Z axis
            float x_rot = x2 * cr - y2 * sr;
            float y_rot = x2 * sr + y2 * cr;

            // 4) Perspective projection
            float dist = 340.0f + z2;
            if (dist < 130.0f) dist = 130.0f;
            float persp = 325.0f / dist;

            float sx = 180.0f + x_rot * persp;
            float sy_scr = 160.0f - y_rot * persp;

            if (sx < 6.0f) sx = 6.0f;
            if (sx > 353.0f) sx = 353.0f;
            if (sy_scr < 12.0f) sy_scr = 12.0f;
            if (sy_scr > 347.0f) sy_scr = 347.0f;

            int16_t ix = (int16_t)sx;
            int16_t iy = (int16_t)sy_scr;
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
        if (s_wc_row_ymax[r] > frame_max_y) frame_max_y = s_wc_row_ymax[r];
    }

    if (frame_min_y < 0) frame_min_y = 0;
    if (frame_max_y >= WC_H) frame_max_y = WC_H - 1;
    s_wc_cur_frame_ymin = frame_min_y;
    s_wc_cur_frame_ymax = frame_max_y;
}

// LVGL custom draw callback: rasterizes visible 3D wireframe segments directly into internal DMA SRAM!
static void wc_layer_draw_event_cb(lv_event_t *e) {
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    if (!draw_ctx || !draw_ctx->buf || !draw_ctx->buf_area) return;

    lv_obj_t *obj = lv_event_get_target(e);
    const int ox = obj ? obj->coords.x1 : 0;
    const int oy = obj ? obj->coords.y1 : 0;

    uint16_t *buf = (uint16_t *)draw_ctx->buf;
    const int bx1 = draw_ctx->buf_area->x1;
    const int bx2 = draw_ctx->buf_area->x2;
    const int by1 = draw_ctx->buf_area->y1;
    const int by2 = draw_ctx->buf_area->y2;
    const int bw  = bx2 - bx1 + 1;

    // Render from farthest depth row to nearest depth row (painter's order)
    int r_start = s_wc_front_to_back ? 1 : (WC_ROWS - 1);
    int r_end   = s_wc_front_to_back ? WC_ROWS : 0;
    int r_step  = s_wc_front_to_back ? 1 : -1;

    for (int r = r_start; r != r_end; r += r_step) {
        if (s_wc_row_ymax[r] + oy < by1 || s_wc_row_ymin[r] + oy > by2) continue;

        uint16_t col_long  = s_wc_col_long[r];
        uint16_t col_ridge = s_wc_col_ridge[r];

        // Longitudinal depth lines connecting row r to row r-1
        for (int c = 0; c < WC_COLS; c++) {
            wc_strip_line(buf, bx1, bx2, by1, by2, bw,
                          s_wc_vx[r][c] + ox,     s_wc_vy[r][c] + oy,
                          s_wc_vx[r - 1][c] + ox, s_wc_vy[r - 1][c] + oy,
                          col_long);
        }

        // Transverse ridge line across row r
        for (int c = 1; c < WC_COLS; c++) {
            wc_strip_line(buf, bx1, bx2, by1, by2, bw,
                          s_wc_vx[r][c - 1] + ox, s_wc_vy[r][c - 1] + oy,
                          s_wc_vx[r][c] + ox,     s_wc_vy[r][c] + oy,
                          col_ridge);
        }
    }

    // Draw Row 0 (the live real-time front oscilloscope waveform!) on top in its bright highlight color
    if (s_wc_row_ymax[0] + oy >= by1 && s_wc_row_ymin[0] + oy <= by2) {
        uint16_t front_col = s_wc_col_ridge[0];
        for (int c = 1; c < WC_COLS; c++) {
            wc_strip_line(buf, bx1, bx2, by1, by2, bw,
                          s_wc_vx[0][c - 1] + ox, s_wc_vy[0][c - 1] + oy,
                          s_wc_vx[0][c] + ox,     s_wc_vy[0][c] + oy,
                          front_col);
        }
    }
}

// LVGL timer callback: runs directly inside lv_timer_handler() right before display refresh for 60 FPS
static void whitecap_lv_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (!g_app.whitecap_active || s_page_anim_active || !s_wc_layer || lv_obj_has_flag(s_wc_layer, LV_OBJ_FLAG_HIDDEN)) {
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
    int16_t inv_y2 = (s_wc_cur_frame_ymax > s_wc_prev_frame_ymax) ? s_wc_cur_frame_ymax : s_wc_prev_frame_ymax;
    s_wc_prev_frame_ymin = s_wc_cur_frame_ymin;
    s_wc_prev_frame_ymax = s_wc_cur_frame_ymax;
    lv_area_t dirty = {0, inv_y1, WC_W - 1, inv_y2};
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
// O(1) 60 FPS Virtualized Animated Wheel Updaters (Zero DOM rebuilds!)
// =============================================================================
static inline uint32_t lerp_hex_color(uint32_t c0, uint32_t c1, float t) {
    if (t <= 0.0f) return c0;
    if (t >= 1.0f) return c1;
    int ti = (int)(t * 256.0f);
    int r0 = (int)((c0 >> 16) & 0xFF), g0 = (int)((c0 >> 8) & 0xFF), b0 = (int)(c0 & 0xFF);
    int r1 = (int)((c1 >> 16) & 0xFF), g1 = (int)((c1 >> 8) & 0xFF), b1 = (int)(c1 & 0xFF);
    int r = r0 + (((r1 - r0) * ti) >> 8);
    int g = g0 + (((g1 - g0) * ti) >> 8);
    int b = b0 + (((b1 - b0) * ti) >> 8);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline void eval_wheel_geometry(float pos, int16_t *out_y, int16_t *out_w, int16_t *out_h, int16_t *out_dot_h) {
    // 7 knots for pos = -3.0, -2.0, -1.0, 0.0, +1.0, +2.0, +3.0 along the 3D cylindrical dial
    static const float KNOT_Y[7] = { 40.0f,  56.0f,  98.0f, 140.0f, 188.0f, 230.0f, 254.0f };
    static const float KNOT_W[7] = {208.0f, 252.0f, 282.0f, 300.0f, 282.0f, 252.0f, 208.0f };

    float fk = pos + 3.0f;
    if (fk < 0.0f) fk = 0.0f;
    if (fk > 6.0f) fk = 6.0f;
    int k0 = (int)floorf(fk);
    if (k0 > 5) k0 = 5;
    float t = fk - (float)k0;

    int16_t w = (int16_t)lroundf(KNOT_W[k0] + (KNOT_W[k0 + 1] - KNOT_W[k0]) * t);
    w = (int16_t)((w + 1) & ~3); // Quantize width to 4px steps to avoid per-frame layout thrash

    bool is_center = (fabsf(pos) < 0.45f);
    *out_y     = (int16_t)lroundf(KNOT_Y[k0] + (KNOT_Y[k0 + 1] - KNOT_Y[k0]) * t);
    *out_w     = w;
    *out_h     = is_center ? 44 : 38;
    *out_dot_h = is_center ? 22 : 16;
}

static void set_wheel_arc_state(int count, int cursor, float anim_offset) {
    (void)count;
    (void)cursor;
    (void)anim_offset;
}

static void apply_wheel_slot_visuals(
    lv_obj_t *row, lv_obj_t *dot, lv_obj_t *title, lv_obj_t *tag,
    WheelSlotCache &cache,
    int idx, float pos, bool settling,
    const char *title_str, const char *tag_str, int tag_kind,
    uint32_t center_border_hex, uint32_t side_border_hex,
    uint32_t center_dot_hex, uint32_t side_dot_hex,
    uint32_t tag_base_hex, bool tag_only_near_center)
{
    float apos = fabsf(pos);
    if (apos >= 2.85f) {
        if (!cache.valid || !cache.hidden) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            cache.hidden = true;
            cache.valid = true;
        }
        return;
    }

    if (!cache.valid || cache.hidden) {
        lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
        cache.hidden = false;
    }

    lv_obj_set_user_data(row, (void *)(intptr_t)idx);

    int16_t y = 0, w = 280, h = 38, dot_h = 16;
    eval_wheel_geometry(pos, &y, &w, &h, &dot_h);
    int16_t x = (int16_t)((360 - w) >> 1);

    // Quantize apos for color interpolation (0.20 steps) so intermediate frames only
    // move widget Y coordinates without triggering LVGL style cascade refreshes every frame.
    float q_apos = settling ? apos : (floorf(apos * 5.0f + 0.5f) * 0.20f);

    // Smoothly blend center highlight (apos 0.0 -> 1.0) and outer slate dimming (apos 1.0 -> 2.0)
    float center_t = (q_apos < 1.0f) ? (1.0f - q_apos) : 0.0f;
    float focus_t = center_t * center_t; // crisp focus peak at center
    uint32_t bg_hex = lerp_hex_color(0x0A0E15, 0x141B28, focus_t);
    uint32_t border_hex = lerp_hex_color(side_border_hex, center_border_hex, focus_t);
    uint32_t dot_hex = lerp_hex_color(side_dot_hex, center_dot_hex, center_t);

    uint32_t title_hex;
    if (q_apos <= 1.0f) {
        title_hex = lerp_hex_color(0xFFFFFF, 0xCBD5E1, q_apos);
    } else {
        float outer_t = (q_apos - 1.0f);
        if (outer_t > 1.0f) outer_t = 1.0f;
        title_hex = lerp_hex_color(0xCBD5E1, 0x64748B, outer_t);
    }

    bool has_tag = (tag_str && tag_str[0] != '\0');
    uint32_t tag_hex = tag_base_hex;
    if (has_tag && tag_only_near_center) {
        if (q_apos >= 0.85f) {
            has_tag = false;
        } else {
            tag_hex = lerp_hex_color(tag_base_hex, bg_hex, q_apos / 0.85f);
        }
    }

    // Smooth fade into pure screen background (0x050608) at top/bottom 3D drum horizons (apos 2.0 -> 2.85)
    if (q_apos > 2.0f) {
        float fade = (q_apos - 2.0f) / 0.85f;
        if (fade > 1.0f) fade = 1.0f;
        bg_hex     = lerp_hex_color(bg_hex,     0x050608, fade);
        border_hex = lerp_hex_color(border_hex, 0x050608, fade);
        dot_hex    = lerp_hex_color(dot_hex,    0x050608, fade);
        title_hex  = lerp_hex_color(title_hex,  0x050608, fade);
        tag_hex    = lerp_hex_color(tag_hex,    0x050608, fade);
    }

    uint8_t border_w = (apos < 0.48f) ? 2 : 1;
    bool is_lg_font = (apos < 0.45f);
    int16_t title_w = settling
                      ? (int16_t)(w - (has_tag ? 102 : 38))
                      : (int16_t)(has_tag ? 156 : 214);

    if (!cache.valid || cache.x != x || cache.y != y) {
        lv_obj_set_pos(row, x, y);
        cache.x = x;
        cache.y = y;
    }
    if (!cache.valid || cache.w != w || cache.h != h) {
        lv_obj_set_size(row, w, h);
        cache.w = w;
        cache.h = h;
    }
    if (!cache.valid || cache.dot_h != dot_h) {
        lv_obj_set_height(dot, dot_h);
        cache.dot_h = dot_h;
    }
    if (!cache.valid || cache.bg_hex != bg_hex) {
        lv_obj_set_style_bg_color(row, hex_to_lv(bg_hex), 0);
        cache.bg_hex = bg_hex;
    }
    if (!cache.valid || cache.border_w != border_w) {
        lv_obj_set_style_border_width(row, border_w, 0);
        cache.border_w = border_w;
    }
    if (!cache.valid || cache.border_hex != border_hex) {
        lv_obj_set_style_border_color(row, hex_to_lv(border_hex), 0);
        cache.border_hex = border_hex;
    }
    if (!cache.valid || cache.dot_hex != dot_hex) {
        lv_obj_set_style_bg_color(dot, hex_to_lv(dot_hex), 0);
        cache.dot_hex = dot_hex;
    }
    if (!cache.valid || cache.is_lg_font != is_lg_font) {
        lv_obj_set_style_text_font(title, is_lg_font ? &ui_font_lg : &ui_font_md, 0);
        lv_obj_set_height(title, is_lg_font ? 24 : 20);
        cache.is_lg_font = is_lg_font;
    }
    if (!cache.valid || cache.title_w != title_w) {
        lv_obj_set_width(title, title_w);
        cache.title_w = title_w;
    }
    if (!cache.valid || cache.item_idx != idx) {
        lv_label_set_text(title, title_str);
        cache.item_idx = idx;
    }
    if (!cache.valid || cache.title_hex != title_hex) {
        lv_obj_set_style_text_color(title, hex_to_lv(title_hex), 0);
        cache.title_hex = title_hex;
    }

    if (!has_tag) {
        if (!cache.valid || !cache.tag_hidden) {
            lv_obj_add_flag(tag, LV_OBJ_FLAG_HIDDEN);
            cache.tag_hidden = true;
        }
    } else {
        if (!cache.valid || cache.tag_hidden) {
            lv_obj_clear_flag(tag, LV_OBJ_FLAG_HIDDEN);
            cache.tag_hidden = false;
        }
        if (!cache.valid || cache.tag_kind != tag_kind) {
            lv_label_set_text(tag, tag_str);
            cache.tag_kind = tag_kind;
        }
        if (!cache.valid || cache.tag_hex != tag_hex) {
            lv_obj_set_style_text_color(tag, hex_to_lv(tag_hex), 0);
            cache.tag_hex = tag_hex;
        }
    }

    cache.valid = true;
}

static void update_station_wheel(bool sync_cursor_to_active) {
    if (!s_st_slots[0]) return;

    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(15)) != pdTRUE) return;
    int count = g_app.preset_count;
    int active_idx = g_app.current_preset;
    if (count > MAX_PRESETS) count = MAX_PRESETS;
    if (count <= 0) {
        xSemaphoreGive(g_app.mutex);
        return;
    }

    if (sync_cursor_to_active || s_station_cursor < 0 || s_station_cursor >= count) {
        s_station_cursor = active_idx;
        s_st_anim_offset = 0.0f;
        for (int i = 0; i < WHEEL_SLOTS; i++) s_st_cache[i].item_idx = -1;
    }

    const StationPreset &focused_st = g_app.presets[s_station_cursor];
    if (s_lbl_st_hdr && (s_st_hdr_cursor != s_station_cursor || s_st_hdr_count != count)) {
        s_st_hdr_cursor = s_station_cursor;
        s_st_hdr_count = count;
        lv_label_set_text_fmt(s_lbl_st_hdr, "STATIONS  •  %d / %d  •  %s",
                              s_station_cursor + 1, count,
                              focused_st.genre[0] ? focused_st.genre : "SomaFM");
    }

    int int_shift = (int)floorf(s_st_anim_offset + 0.5f);
    float frac = s_st_anim_offset - (float)int_shift;
    bool settling = (fabsf(s_st_anim_offset) <= 0.01f);

    for (int slot = 0; slot < WHEEL_SLOTS; slot++) {
        int base_rel = slot - WHEEL_CENTER_SLOT; // -3..+3
        int rel_item = base_rel - int_shift;
        int idx = ((s_station_cursor + rel_item) % count + count) % count;
        float pos = (float)base_rel + frac;

        const StationPreset &st = g_app.presets[idx];
        bool is_playing = (idx == active_idx);
        uint32_t st_col = st.color ? st.color : 0x10B981;

        apply_wheel_slot_visuals(
            s_st_slots[slot], s_st_dots[slot], s_st_titles[slot], s_st_tags[slot],
            s_st_cache[slot],
            idx, pos, settling,
            st.name,
            is_playing ? "PLAYING" : "",
            is_playing ? 1 : 0,
            st_col,
            is_playing ? 0x10B981 : 0x151D2A,
            st_col,
            st_col,
            0x10B981,
            false);
    }
    xSemaphoreGive(g_app.mutex);

    set_wheel_arc_state(count, s_station_cursor, s_st_anim_offset);
}

static void update_speaker_wheel(bool sync_cursor_to_active) {
    if (!s_sp_slots[0]) return;

    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(15)) != pdTRUE) return;
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
        s_sp_anim_offset = 0.0f;
        for (int i = 0; i < WHEEL_SLOTS; i++) s_sp_cache[i].item_idx = -1;
    }

    if (s_lbl_spkr_hdr && (s_sp_hdr_cursor != s_speaker_cursor || s_sp_hdr_count != count || s_sp_hdr_scanning != scanning)) {
        s_sp_hdr_cursor = s_speaker_cursor;
        s_sp_hdr_count = count;
        s_sp_hdr_scanning = scanning;
        if (scanning && count <= 1) {
            lv_label_set_text(s_lbl_spkr_hdr, "SCANNING AIRPLAY...");
        } else {
            lv_label_set_text_fmt(s_lbl_spkr_hdr, "AIRPLAY  •  %d / %d  •  LIVE", s_speaker_cursor + 1, count);
        }
    }

    int int_shift = (int)floorf(s_sp_anim_offset + 0.5f);
    float frac = s_sp_anim_offset - (float)int_shift;
    bool settling = (fabsf(s_sp_anim_offset) <= 0.01f);
    bool wrap_list = (count >= 5);

    for (int slot = 0; slot < WHEEL_SLOTS; slot++) {
        int base_rel = slot - WHEEL_CENTER_SLOT; // -3..+3
        int rel_item = base_rel - int_shift;
        int idx = wrap_list
                  ? (((s_speaker_cursor + rel_item) % count + count) % count)
                  : (s_speaker_cursor + rel_item);
        float pos = (float)base_rel + frac;

        if (!wrap_list && (idx < 0 || idx >= count)) {
            if (!s_sp_cache[slot].valid || !s_sp_cache[slot].hidden) {
                lv_obj_add_flag(s_sp_slots[slot], LV_OBJ_FLAG_HIDDEN);
                s_sp_cache[slot].hidden = true;
                s_sp_cache[slot].valid = true;
            }
            continue;
        }

        const AirPlaySpeaker &sp = g_app.speakers[idx];
        bool is_active = (idx == active_idx);

        const char *tag_str = "";
        int tag_kind = 0;
        uint32_t tag_col = 0x38BDF8;
        bool tag_only_center = false;

        if (is_active) {
            tag_str = "ACTIVE";
            tag_kind = 1;
            tag_col = 0x10B981;
        } else if (sp.requires_pin) {
            tag_str = "PIN";
            tag_kind = 2;
            tag_col = 0xF59E0B;
        } else {
            tag_only_center = true;
            if (sp.is_local_dac) {
                tag_str = "3.5MM";
                tag_kind = 3;
                tag_col = 0x94A3B8;
            } else if (strcasestr(sp.am, "AudioAccessory") != NULL) {
                tag_str = "HOMEPOD";
                tag_kind = 4;
                tag_col = 0x38BDF8;
            } else if (strncasecmp(sp.host, "Sonos-", 6) == 0) {
                tag_str = "SONOS";
                tag_kind = 5;
                tag_col = 0x38BDF8;
            } else if (sp.is_ap2) {
                tag_str = "AIRPLAY 2";
                tag_kind = 6;
                tag_col = 0x38BDF8;
            } else {
                tag_str = "AIRPLAY";
                tag_kind = 7;
                tag_col = 0x64748B;
            }
        }

        uint32_t center_dot = is_active ? 0x10B981 : (sp.requires_pin ? 0xF59E0B : 0x38BDF8);
        uint32_t side_dot   = is_active ? 0x10B981 : (sp.requires_pin ? 0xF59E0B : 0x334155);

        apply_wheel_slot_visuals(
            s_sp_slots[slot], s_sp_dots[slot], s_sp_titles[slot], s_sp_tags[slot],
            s_sp_cache[slot],
            idx, pos, settling,
            sp.name,
            tag_str,
            tag_kind,
            sp.requires_pin ? 0xF59E0B : 0x38BDF8,
            is_active ? 0x10B981 : 0x151D2A,
            center_dot,
            side_dot,
            tag_col,
            tag_only_center);
    }
    xSemaphoreGive(g_app.mutex);

    set_wheel_arc_state(count, s_speaker_cursor, s_sp_anim_offset);
}

static void finish_page_transition(void) {
    if (!s_page_anim_active) return;
    s_page_anim_active = false;
    if (s_page_anim_from && s_page_anim_from != s_page_anim_to) {
        lv_obj_add_flag(s_page_anim_from, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_page_anim_from, 0, 0);
    }
    if (s_page_anim_to) {
        lv_obj_clear_flag(s_page_anim_to, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_page_anim_to, 0, 0);
    }
    s_page_anim_from = NULL;
    s_page_anim_to = NULL;
    if (s_outer_arc) {
        if (s_active_tab == TAB_STATIONS || s_active_tab == TAB_SPEAKERS || g_app.whitecap_active) {
            lv_obj_add_flag(s_outer_arc, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_outer_arc, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void start_page_transition(lv_obj_t *from_obj, lv_obj_t *to_obj, int dir_x, int dir_y) {
    finish_page_transition();
    if (!from_obj || !to_obj || from_obj == to_obj || (dir_x == 0 && dir_y == 0)) {
        if (from_obj && from_obj != to_obj) {
            lv_obj_add_flag(from_obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(from_obj, 0, 0);
        }
        if (to_obj) {
            lv_obj_clear_flag(to_obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(to_obj, 0, 0);
        }
        if (s_outer_arc) {
            if (s_active_tab == TAB_STATIONS || s_active_tab == TAB_SPEAKERS || g_app.whitecap_active) {
                lv_obj_add_flag(s_outer_arc, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(s_outer_arc, LV_OBJ_FLAG_HIDDEN);
            }
        }
        return;
    }

    // Hide full-screen 356x356 outer lv_arc while pages are sliding so LVGL skips
    // full-screen trigonometric arc mask evaluation during the transition!
    if (s_outer_arc) {
        lv_obj_add_flag(s_outer_arc, LV_OBJ_FLAG_HIDDEN);
    }

    s_page_anim_active = true;
    s_page_anim_from = from_obj;
    s_page_anim_to = to_obj;
    s_page_anim_dir_x = (int8_t)dir_x;
    s_page_anim_dir_y = (int8_t)dir_y;
    s_page_anim_progress = 0.05f;
    s_wheel_anim_last_us = esp_timer_get_time();

    float inv = 1.0f - s_page_anim_progress;
    float ease = 1.0f - inv * inv * inv;
    int16_t fx = (int16_t)((int)lrintf((float)dir_x * 360.0f * ease) & ~1);
    int16_t fy = (int16_t)((int)lrintf((float)dir_y * 360.0f * ease) & ~1);
    int16_t tx = (int16_t)(fx - dir_x * 360);
    int16_t ty = (int16_t)(fy - dir_y * 360);

    lv_obj_clear_flag(from_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(to_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(from_obj, fx, fy);
    lv_obj_set_pos(to_obj, tx, ty);
}

// 60 FPS LVGL timer callback for smooth page swipe & wheel scrolling animations
static void wheel_anim_lv_timer_cb(lv_timer_t *timer) {
    (void)timer;
    bsp_swipe_dir_t pending_swipe = bsp_pop_swipe_gesture();
    if (pending_swipe != BSP_SWIPE_NONE) {
        handle_swipe_gesture(pending_swipe);
    }

    if (s_pin_modal || s_qr_modal || s_wifi_modal) {
        s_wheel_anim_last_us = 0;
        return;
    }
    if (!s_page_anim_active && g_app.whitecap_active) {
        s_wheel_anim_last_us = 0;
        return;
    }

    int64_t now_us = esp_timer_get_time();
    float dt = (s_wheel_anim_last_us > 0) ? ((float)(now_us - s_wheel_anim_last_us) * 1e-6f) : 0.016f;
    s_wheel_anim_last_us = now_us;
    if (dt < 0.001f) dt = 0.001f;
    if (dt > 0.045f) dt = 0.045f;

    if (s_page_anim_active) {
        s_page_anim_progress += dt / 0.23f;
        if (s_page_anim_progress >= 1.0f) {
            finish_page_transition();
        } else {
            float inv = 1.0f - s_page_anim_progress;
            float ease = 1.0f - inv * inv * inv;
            int16_t fx = (int16_t)((int)lrintf((float)s_page_anim_dir_x * 360.0f * ease) & ~1);
            int16_t fy = (int16_t)((int)lrintf((float)s_page_anim_dir_y * 360.0f * ease) & ~1);
            int16_t tx = (int16_t)(fx - (int)s_page_anim_dir_x * 360);
            int16_t ty = (int16_t)(fy - (int)s_page_anim_dir_y * 360);
            if (s_page_anim_from) lv_obj_set_pos(s_page_anim_from, fx, fy);
            if (s_page_anim_to)   lv_obj_set_pos(s_page_anim_to, tx, ty);
        }
        return;
    }

    if (s_active_tab == TAB_STATIONS && !s_st_dragging && s_st_anim_offset != 0.0f) {
        int prev_shift = (int)floorf(s_st_anim_offset + 0.5f);
        float mag = fabsf(s_st_anim_offset);
        float step = (mag * 8.5f + 0.55f) * dt;
        if (mag <= step || mag < 0.008f) {
            s_st_anim_offset = 0.0f;
        } else {
            s_st_anim_offset += (s_st_anim_offset > 0.0f) ? -step : step;
        }
        int new_shift = (int)floorf(s_st_anim_offset + 0.5f);
        if (new_shift != prev_shift) {
            play_wheel_tick_haptic();
        }
        update_station_wheel(false);
    } else if (s_active_tab == TAB_SPEAKERS && !s_sp_dragging && s_sp_anim_offset != 0.0f) {
        int prev_shift = (int)floorf(s_sp_anim_offset + 0.5f);
        float mag = fabsf(s_sp_anim_offset);
        float step = (mag * 8.5f + 0.55f) * dt;
        if (mag <= step || mag < 0.008f) {
            s_sp_anim_offset = 0.0f;
        } else {
            s_sp_anim_offset += (s_sp_anim_offset > 0.0f) ? -step : step;
        }
        int new_shift = (int)floorf(s_sp_anim_offset + 0.5f);
        if (new_shift != prev_shift) {
            play_wheel_tick_haptic();
        }
        update_speaker_wheel(false);
    }
}

static void switch_tab(ActiveTab tab, int dir_x = 0, int dir_y = 0) {
    ActiveTab prev_tab = s_active_tab;
    s_active_tab = tab;

    // Infer natural spatial slide direction if not explicitly specified
    if (dir_x == 0 && dir_y == 0 && prev_tab != tab) {
        if (prev_tab == TAB_NOW_PLAYING && tab == TAB_STATIONS)      dir_x = -1;
        else if (prev_tab == TAB_STATIONS && tab == TAB_NOW_PLAYING) dir_x = +1;
        else if (prev_tab == TAB_NOW_PLAYING && tab == TAB_SPEAKERS) dir_x = +1;
        else if (prev_tab == TAB_SPEAKERS && tab == TAB_NOW_PLAYING) dir_x = -1;
        else if (prev_tab == TAB_NOW_PLAYING && tab == TAB_WIFI)     dir_y = +1;
        else if (prev_tab == TAB_WIFI && tab == TAB_NOW_PLAYING)     dir_y = -1;
        else dir_x = ((int)tab > (int)prev_tab) ? -1 : +1;
    }

    // Reset wheel drag state and populate destination wheel before sliding it on-screen
    s_wheel_touch_armed = false;
    s_wheel_horiz_locked = false;
    s_st_dragging = false;
    s_sp_dragging = false;
    if (tab == TAB_STATIONS) {
        s_st_anim_offset = 0.0f;
        s_st_hdr_cursor = -1;
        update_station_wheel(true);
    } else if (tab == TAB_SPEAKERS) {
        s_sp_anim_offset = 0.0f;
        s_sp_hdr_cursor = -1;
        update_speaker_wheel(true);
    }

    for (int i = 0; i < 4; i++) {
        if (!s_pages[i]) continue;
        if (i != (int)prev_tab && i != (int)tab) {
            lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_pages[i], 0, 0);
        }
    }

    start_page_transition(s_pages[prev_tab], s_pages[tab], dir_x, dir_y);
    haptic_play_effect(HAPTIC_SHARP_CLICK);
    s_wheel_anim_last_us = esp_timer_get_time();
}

static void set_whitecap_visible(bool visible, int dir_x = 0, int dir_y = 0) {
    bool was_visible = g_app.whitecap_active;
    s_prev_wc_active = visible;
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        g_app.whitecap_active = visible;
        xSemaphoreGive(g_app.mutex);
    }
    app_state_save_playback();
    if (!s_wc_layer) return;

    if (dir_x == 0 && dir_y == 0 && visible != was_visible) {
        dir_y = visible ? -1 : +1;
    }

    if (visible) {
        // Pre-seed terrain history with gentle organic waves so frame 1 looks alive immediately
        memset(s_wc_wave, 0, sizeof(s_wc_wave));
        for (int r = 0; r < WC_ROWS; r++) {
            for (int c = 1; c < WC_COLS - 1; c++) {
                s_wc_wave[r][c] = (int8_t)(8.0f * sinf(c * 0.35f + r * 0.28f));
            }
        }
        int8_t zero_w[128] = {0};
        uint8_t zero_vu[12] = {0};
        step_whitecap_simulation(zero_w, zero_w, zero_vu, 18, g_app.whitecap_preset & 3);
        s_wc_prev_frame_ymin = 0;
        s_wc_prev_frame_ymax = WC_H - 1;

        lv_obj_move_foreground(s_wc_layer);
        if (was_visible) {
            if (s_pages[s_active_tab]) lv_obj_add_flag(s_pages[s_active_tab], LV_OBJ_FLAG_HIDDEN);
            if (s_outer_arc) lv_obj_add_flag(s_outer_arc, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_wc_layer, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_wc_layer, 0, 0);
        } else {
            start_page_transition(s_pages[s_active_tab], s_wc_layer, dir_x, dir_y);
        }
        int preset = g_app.whitecap_preset & 3;
        show_whitecap_caption(lv_label_get_text(s_lbl_track), WHITECAP_PRESET_NAMES[preset], 2600);
        haptic_play_effect(HAPTIC_DOUBLE_CLICK);
        s_wheel_anim_last_us = esp_timer_get_time();
    } else {
        if (!was_visible) {
            lv_obj_add_flag(s_wc_layer, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_wc_layer, 0, 0);
            if (s_pages[s_active_tab]) {
                lv_obj_clear_flag(s_pages[s_active_tab], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(s_pages[s_active_tab], 0, 0);
            }
            if (s_outer_arc && s_active_tab != TAB_STATIONS && s_active_tab != TAB_SPEAKERS) {
                lv_obj_clear_flag(s_outer_arc, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            start_page_transition(s_wc_layer, s_pages[s_active_tab], dir_x, dir_y);
        }
        haptic_play_effect(HAPTIC_SHARP_CLICK);
        s_wheel_anim_last_us = esp_timer_get_time();
    }
}

static void close_pin_modal(bool cancel_request) {
    if (s_pin_modal) {
        lv_obj_del(s_pin_modal);
        s_pin_modal = NULL;
        s_pin_lbl_spkr = NULL;
        for (int i = 0; i < 4; i++) {
            s_pin_boxes[i] = NULL;
            s_pin_box_lbls[i] = NULL;
        }
    }
    if (cancel_request) {
        app_cancel_airplay_pin();
        show_hud("AIRPLAY PIN", "Cancelled", 0x94A3B8, 1400);
    }
}

static void refresh_pin_boxes(void) {
    for (int i = 0; i < 4; i++) {
        if (!s_pin_boxes[i] || !s_pin_box_lbls[i]) continue;
        bool is_cur = (i == s_pin_cursor);
        bool is_filled = (i < s_pin_entered_len);
        lv_obj_set_style_border_color(s_pin_boxes[i], hex_to_lv(is_cur ? 0x38BDF8 : (is_filled ? 0x10B981 : 0x1E293B)), 0);
        lv_obj_set_style_border_width(s_pin_boxes[i], is_cur ? 2 : 1, 0);
        lv_obj_set_style_bg_color(s_pin_boxes[i], hex_to_lv(is_cur ? 0x132032 : 0x0B0F17), 0);
        char d[2] = {s_pin_digits[i], '\0'};
        lv_label_set_text(s_pin_box_lbls[i], d);
        lv_obj_set_style_text_color(s_pin_box_lbls[i], hex_to_lv(is_cur ? 0xFFFFFF : (is_filled ? 0x10B981 : 0x94A3B8)), 0);
    }
}

static void submit_current_pin(void) {
    s_pin_digits[4] = '\0';
    char pin_copy[8];
    strncpy(pin_copy, s_pin_digits, sizeof(pin_copy) - 1);
    pin_copy[sizeof(pin_copy) - 1] = '\0';
    haptic_play_effect(HAPTIC_DOUBLE_CLICK);
    close_pin_modal(false);
    app_submit_airplay_pin(pin_copy);
    show_hud("AIRPLAY PIN", "Pairing Speaker...", 0x10B981, 2000);
}

static void open_pin_modal(const char *spkr_name) {
    if (s_pin_modal) {
        if (s_pin_lbl_spkr && spkr_name && spkr_name[0]) {
            lv_label_set_text(s_pin_lbl_spkr, spkr_name);
        }
        lv_obj_move_foreground(s_pin_modal);
        return;
    }

    s_pin_digits[0] = '0';
    s_pin_digits[1] = '0';
    s_pin_digits[2] = '0';
    s_pin_digits[3] = '0';
    s_pin_digits[4] = '\0';
    s_pin_cursor = 0;
    s_pin_entered_len = 0;

    s_pin_modal = lv_obj_create(s_scr);
    lv_obj_set_size(s_pin_modal, 360, 360);
    lv_obj_center(s_pin_modal);
    lv_obj_set_style_radius(s_pin_modal, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_pin_modal, hex_to_lv(0x050608), 0);
    lv_obj_set_style_bg_opa(s_pin_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pin_modal, 0, 0);
    lv_obj_set_style_pad_all(s_pin_modal, 0, 0);
    lv_obj_clear_flag(s_pin_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(s_pin_modal);
    lv_label_set_text(hdr, "ENTER 4-DIGIT AIRPLAY PIN");
    lv_obj_set_style_text_font(hdr, &ui_font_sm, 0);
    lv_obj_set_style_text_color(hdr, hex_to_lv(0xF59E0B), 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 22);

    s_pin_lbl_spkr = lv_label_create(s_pin_modal);
    lv_label_set_text(s_pin_lbl_spkr, (spkr_name && spkr_name[0]) ? spkr_name : "AirPlay Speaker");
    lv_obj_set_style_text_font(s_pin_lbl_spkr, &ui_font_md, 0);
    lv_obj_set_style_text_color(s_pin_lbl_spkr, hex_to_lv(0xFFFFFF), 0);
    lv_label_set_long_mode(s_pin_lbl_spkr, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_pin_lbl_spkr, 240, 22);
    lv_obj_set_style_text_align(s_pin_lbl_spkr, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_pin_lbl_spkr, LV_ALIGN_TOP_MID, 0, 40);

    // 4 Digit Boxes (tappable to select digit, or turn knob to dial 0-9)
    static const int BOX_X[4] = {-81, -27, 27, 81};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *box = lv_obj_create(s_pin_modal);
        lv_obj_set_size(box, 46, 44);
        lv_obj_align(box, LV_ALIGN_TOP_MID, BOX_X[i], 64);
        lv_obj_set_style_radius(box, 10, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(box, (void *)(intptr_t)i);
        lv_obj_add_event_cb(box, [](lv_event_t *ev) {
            int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(ev));
            s_pin_cursor = idx;
            haptic_play_effect(HAPTIC_LIGHT_TICK);
            refresh_pin_boxes();
        }, LV_EVENT_CLICKED, NULL);

        lv_obj_t *dl = lv_label_create(box);
        lv_obj_set_style_text_font(dl, &ui_font_xl, 0);
        lv_obj_center(dl);

        s_pin_boxes[i] = box;
        s_pin_box_lbls[i] = dl;
    }
    refresh_pin_boxes();

    // Compact 3x4 Touch Keypad (y = 116..272)
    static const char *KEY_LBLS[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", LV_SYMBOL_BACKSPACE, "0", "OK"};
    static const int KEY_COL_X[3] = {-74, 0, 74};
    for (int k = 0; k < 12; k++) {
        int r = k / 3;
        int c = k % 3;
        lv_obj_t *kb_btn = lv_btn_create(s_pin_modal);
        lv_obj_set_size(kb_btn, 64, 34);
        lv_obj_align(kb_btn, LV_ALIGN_TOP_MID, KEY_COL_X[c], 116 + r * 39);
        lv_obj_set_style_radius(kb_btn, 17, 0);
        uint32_t bg = (k == 11) ? 0x10B981 : ((k == 9) ? 0x1E293B : 0x111827);
        uint32_t fg = (k == 11) ? 0x050608 : 0xF8FAFC;
        lv_obj_set_style_bg_color(kb_btn, hex_to_lv(bg), 0);
        lv_obj_set_style_border_width(kb_btn, 1, 0);
        lv_obj_set_style_border_color(kb_btn, hex_to_lv(k == 11 ? 0x10B981 : 0x1F2937), 0);
        lv_obj_set_style_shadow_width(kb_btn, 0, 0);
        lv_obj_set_style_pad_all(kb_btn, 0, 0);
        lv_obj_set_user_data(kb_btn, (void *)(intptr_t)k);

        lv_obj_t *kl = lv_label_create(kb_btn);
        lv_label_set_text(kl, KEY_LBLS[k]);
        lv_obj_set_style_text_font(kl, &ui_font_md, 0);
        lv_obj_set_style_text_color(kl, hex_to_lv(fg), 0);
        lv_obj_center(kl);

        lv_obj_add_event_cb(kb_btn, [](lv_event_t *ev) {
            if (bsp_was_touch_dragged()) return;
            int k_idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(ev));
            if (k_idx == 9) {
                // Backspace
                haptic_play_effect(HAPTIC_LIGHT_TICK);
                if (s_pin_entered_len > 0) s_pin_entered_len--;
                if (s_pin_cursor > 0) s_pin_cursor--;
                s_pin_digits[s_pin_cursor] = '0';
                refresh_pin_boxes();
            } else if (k_idx == 11) {
                // OK / Submit
                submit_current_pin();
            } else {
                char digit = (k_idx == 10) ? '0' : (char)('1' + k_idx);
                haptic_play_effect(HAPTIC_SHARP_CLICK);
                s_pin_digits[s_pin_cursor] = digit;
                if (s_pin_entered_len <= s_pin_cursor) {
                    s_pin_entered_len = s_pin_cursor + 1;
                }
                if (s_pin_cursor < 3) {
                    s_pin_cursor++;
                    refresh_pin_boxes();
                } else {
                    refresh_pin_boxes();
                    submit_current_pin();
                }
            }
        }, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *cancel_btn = lv_btn_create(s_pin_modal);
    lv_obj_set_size(cancel_btn, 108, 30);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_radius(cancel_btn, 15, 0);
    lv_obj_set_style_bg_color(cancel_btn, hex_to_lv(0x1E293B), 0);
    lv_obj_set_style_shadow_width(cancel_btn, 0, 0);
    lv_obj_set_style_pad_all(cancel_btn, 0, 0);
    lv_obj_t *cl = lv_label_create(cancel_btn);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_font(cl, &ui_font_sm, 0);
    lv_obj_set_style_text_color(cl, hex_to_lv(0xCBD5E1), 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(cancel_btn, [](lv_event_t *ev) {
        (void)ev;
        haptic_play_effect(HAPTIC_SHARP_CLICK);
        close_pin_modal(true);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_move_foreground(s_pin_modal);
    haptic_play_effect(HAPTIC_DOUBLE_CLICK);
}

static void close_qr_modal(void) {
    if (s_qr_modal) {
        lv_obj_del(s_qr_modal);
        s_qr_modal = NULL;
        s_qr_code = NULL;
        s_qr_lbl_url = NULL;
    }
}

static void open_qr_modal(lv_event_t *e) {
    (void)e;
    if (bsp_was_touch_dragged()) return;
    haptic_play_effect(HAPTIC_SHARP_CLICK);

    if (s_qr_modal) {
        close_qr_modal();
        return;
    }

    char ip[20] = {0};
    char ssid[36] = {0};
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        strncpy(ip, g_app.wifi_ip, sizeof(ip) - 1);
        strncpy(ssid, g_app.wifi_ssid, sizeof(ssid) - 1);
        xSemaphoreGive(g_app.mutex);
    }
    char url[64];
    if (ip[0]) {
        snprintf(url, sizeof(url), "http://%s/", ip);
    } else {
        snprintf(url, sizeof(url), "http://airplayknob.local/");
    }

    s_qr_modal = lv_obj_create(s_scr);
    lv_obj_set_size(s_qr_modal, 360, 360);
    lv_obj_center(s_qr_modal);
    lv_obj_set_style_radius(s_qr_modal, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_qr_modal, hex_to_lv(0x050608), 0);
    lv_obj_set_style_bg_opa(s_qr_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_qr_modal, 0, 0);
    lv_obj_set_style_pad_all(s_qr_modal, 0, 0);
    lv_obj_clear_flag(s_qr_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_qr_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_qr_modal, [](lv_event_t *ev) {
        (void)ev;
        if (bsp_was_touch_dragged()) return;
        haptic_play_effect(HAPTIC_SHARP_CLICK);
        close_qr_modal();
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *hdr = lv_label_create(s_qr_modal);
    lv_label_set_text_fmt(hdr, LV_SYMBOL_WIFI " %s  •  WEB PORTAL", ssid[0] ? ssid : "Wi-Fi");
    lv_obj_set_style_text_font(hdr, &ui_font_sm, 0);
    lv_obj_set_style_text_color(hdr, hex_to_lv(0x10B981), 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 32);

    lv_obj_t *qr_card = lv_obj_create(s_qr_modal);
    lv_obj_set_size(qr_card, 184, 184);
    lv_obj_align(qr_card, LV_ALIGN_CENTER, 0, -4);
    lv_obj_set_style_radius(qr_card, 20, 0);
    lv_obj_set_style_bg_color(qr_card, hex_to_lv(0xFFFFFF), 0);
    lv_obj_set_style_border_width(qr_card, 0, 0);
    lv_obj_set_style_pad_all(qr_card, 0, 0);
    lv_obj_clear_flag(qr_card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_qr_code = lv_qrcode_create(qr_card, 160, hex_to_lv(0x050608), hex_to_lv(0xFFFFFF));
    lv_qrcode_update(s_qr_code, url, strlen(url));
    lv_obj_center(s_qr_code);

    s_qr_lbl_url = lv_label_create(s_qr_modal);
    lv_label_set_text(s_qr_lbl_url, url);
    lv_obj_set_style_text_font(s_qr_lbl_url, &ui_font_md, 0);
    lv_obj_set_style_text_color(s_qr_lbl_url, hex_to_lv(0x38BDF8), 0);
    lv_obj_align(s_qr_lbl_url, LV_ALIGN_TOP_MID, 0, 276);

    lv_obj_t *hint = lv_label_create(s_qr_modal);
    lv_label_set_text(hint, "TAP ANYWHERE TO CLOSE");
    lv_obj_set_style_text_font(hint, &ui_font_sm, 0);
    lv_obj_set_style_text_color(hint, hex_to_lv(0x64748B), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 304);

    lv_obj_move_foreground(s_qr_modal);
}

static void handle_swipe_gesture(bsp_swipe_dir_t dir) {
    if (dir == BSP_SWIPE_NONE) return;

    // Close QR modal if open and swiped
    if (s_qr_modal) {
        close_qr_modal();
        haptic_play_effect(HAPTIC_SHARP_CLICK);
        return;
    }

    // Close PIN modal if open and swiped
    if (s_pin_modal) {
        close_pin_modal(true);
        haptic_play_effect(HAPTIC_SHARP_CLICK);
        return;
    }

    // Close Wi-Fi scan modal if open and horizontally swiped (keep vertical drag for scrolling list!)
    if (s_wifi_modal && !s_wifi_kb) {
        if (dir == BSP_SWIPE_LEFT || dir == BSP_SWIPE_RIGHT) {
            lv_obj_del(s_wifi_modal);
            s_wifi_modal = NULL;
            s_wifi_list = NULL;
            s_wifi_pass_ta = NULL;
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                g_app.wifi_modal_open = false;
                xSemaphoreGive(g_app.mutex);
            }
            haptic_play_effect(HAPTIC_SHARP_CLICK);
        }
        return;
    }

    // WhiteCap Visualizer active:
    // - Swipe Down (or Up) -> Exit WhiteCap back to Home
    // - Swipe Left / Right -> Cycle 3D terrain color palette
    if (g_app.whitecap_active) {
        if (dir == BSP_SWIPE_DOWN || dir == BSP_SWIPE_UP) {
            set_whitecap_visible(false, 0, (dir == BSP_SWIPE_DOWN) ? +1 : -1);
        } else if (dir == BSP_SWIPE_LEFT || dir == BSP_SWIPE_RIGHT) {
            int next_p = 0;
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                next_p = (g_app.whitecap_preset + (dir == BSP_SWIPE_LEFT ? 1 : 3)) & 3;
                g_app.whitecap_preset = next_p;
                xSemaphoreGive(g_app.mutex);
            }
            app_state_save_playback();
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
            switch_tab(TAB_STATIONS, -1, 0);
        } else if (dir == BSP_SWIPE_RIGHT) {
            switch_tab(TAB_SPEAKERS, +1, 0);
        } else if (dir == BSP_SWIPE_UP) {
            set_whitecap_visible(true, 0, -1);
        } else if (dir == BSP_SWIPE_DOWN) {
            switch_tab(TAB_WIFI, 0, +1);
        }
        return;
    }

    // Stations or AirPlay Speakers Wheel: Horizontal swipe returns to Home
    if (s_active_tab == TAB_STATIONS || s_active_tab == TAB_SPEAKERS) {
        if (dir == BSP_SWIPE_LEFT || dir == BSP_SWIPE_RIGHT) {
            switch_tab(TAB_NOW_PLAYING, (dir == BSP_SWIPE_LEFT) ? -1 : +1, 0);
        } else if ((dir == BSP_SWIPE_UP || dir == BSP_SWIPE_DOWN) && !s_st_dragging && !s_sp_dragging) {
            int step = (dir == BSP_SWIPE_UP) ? 3 : -3;
            if (s_active_tab == TAB_STATIONS) {
                int count = g_app.preset_count > 0 ? g_app.preset_count : 1;
                s_station_cursor = ((s_station_cursor + step) % count + count) % count;
                s_st_anim_offset += (float)step;
                if (s_st_anim_offset > 3.5f) s_st_anim_offset = 3.5f;
                if (s_st_anim_offset < -3.5f) s_st_anim_offset = -3.5f;
                s_wheel_anim_last_us = esp_timer_get_time();
                update_station_wheel(false);
                play_wheel_tick_haptic();
            } else {
                int count = g_app.speaker_count > 0 ? g_app.speaker_count : 1;
                if (count >= 5) {
                    s_speaker_cursor = ((s_speaker_cursor + step) % count + count) % count;
                    s_sp_anim_offset += (float)step;
                } else {
                    int prev_c = s_speaker_cursor;
                    s_speaker_cursor = s_speaker_cursor + (step > 0 ? 1 : -1);
                    if (s_speaker_cursor < 0) s_speaker_cursor = 0;
                    if (s_speaker_cursor >= count) s_speaker_cursor = count - 1;
                    s_sp_anim_offset += (float)(s_speaker_cursor - prev_c);
                }
                if (s_sp_anim_offset > 3.5f) s_sp_anim_offset = 3.5f;
                if (s_sp_anim_offset < -3.5f) s_sp_anim_offset = -3.5f;
                s_wheel_anim_last_us = esp_timer_get_time();
                update_speaker_wheel(false);
                play_wheel_tick_haptic();
            }
        }
        return;
    }

    // Settings (TAB_WIFI): Swipe Up / Down / Left / Right returns to Home
    if (s_active_tab == TAB_WIFI) {
        if (dir == BSP_SWIPE_UP || dir == BSP_SWIPE_DOWN) {
            switch_tab(TAB_NOW_PLAYING, 0, (dir == BSP_SWIPE_UP) ? -1 : +1);
        } else if (dir == BSP_SWIPE_LEFT || dir == BSP_SWIPE_RIGHT) {
            switch_tab(TAB_NOW_PLAYING, (dir == BSP_SWIPE_LEFT) ? -1 : +1, 0);
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

static void close_wifi_modal(void) {
    if (s_wifi_modal) {
        lv_obj_del(s_wifi_modal);
        s_wifi_modal = NULL;
    }
    s_wifi_list = NULL;
    s_wifi_pass_ta = NULL;
    s_wifi_kb = NULL;
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(30)) == pdTRUE) {
        g_app.wifi_modal_open = false;
        xSemaphoreGive(g_app.mutex);
    }
}

static void wifi_kb_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        const char *pwd = s_wifi_pass_ta ? lv_textarea_get_text(s_wifi_pass_ta) : "";
        haptic_play_effect(HAPTIC_DOUBLE_CLICK);
        app_state_save_wifi(s_pending_ssid, pwd ? pwd : "");
        s_conn_dismissed = false;
        s_conn_ready_ms = 0;
        close_wifi_modal();
    } else if (code == LV_EVENT_CANCEL) {
        close_wifi_modal();
    }
}

static void wifi_ssid_click_cb(lv_event_t *e) {
    if (bsp_was_touch_dragged()) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    char prefill_pass[64] = {0};
    s_pending_secure = true;
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (idx >= 0 && idx < g_app.wifi_scan_count) {
            strncpy(s_pending_ssid, g_app.wifi_scan[idx].ssid, sizeof(s_pending_ssid) - 1);
            s_pending_ssid[sizeof(s_pending_ssid) - 1] = '\0';
            s_pending_secure = g_app.wifi_scan[idx].secure;
        }
        if (strcasecmp(s_pending_ssid, g_app.wifi_ssid) == 0 && g_app.wifi_pass[0] != '\0') {
            strncpy(prefill_pass, g_app.wifi_pass, sizeof(prefill_pass) - 1);
        }
        xSemaphoreGive(g_app.mutex);
    }
    if (prefill_pass[0] == '\0') {
        if (strcasecmp(s_pending_ssid, DEFAULT_WIFI_SSID) == 0) {
            strncpy(prefill_pass, DEFAULT_WIFI_PASS, sizeof(prefill_pass) - 1);
        }
#ifdef FALLBACK_WIFI_SSID
        else if (strcasecmp(s_pending_ssid, FALLBACK_WIFI_SSID) == 0) {
            strncpy(prefill_pass, FALLBACK_WIFI_PASS, sizeof(prefill_pass) - 1);
        }
#endif
#ifdef FALLBACK2_WIFI_SSID
        else if (strcasecmp(s_pending_ssid, FALLBACK2_WIFI_SSID) == 0) {
            strncpy(prefill_pass, FALLBACK2_WIFI_PASS, sizeof(prefill_pass) - 1);
        }
#endif
    }

    if (!s_wifi_modal) return;
    haptic_play_effect(HAPTIC_SHARP_CLICK);

    if (!s_pending_secure) {
        app_state_save_wifi(s_pending_ssid, "");
        s_conn_dismissed = false;
        s_conn_ready_ms = 0;
        close_wifi_modal();
        return;
    }

    // Clear s_wifi_list pointer BEFORE cleaning s_wifi_modal so knob_ui_tick never sees a dangling pointer!
    s_wifi_list = NULL;
    lv_obj_clean(s_wifi_modal);

    lv_obj_t *lbl = lv_label_create(s_wifi_modal);
    lv_label_set_text_fmt(lbl, "Join %s", s_pending_ssid);
    lv_obj_set_style_text_font(lbl, &ui_font_md, 0);
    lv_obj_set_style_text_color(lbl, hex_to_lv(0xF8FAFC), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 26);

    s_wifi_pass_ta = lv_textarea_create(s_wifi_modal);
    lv_textarea_set_one_line(s_wifi_pass_ta, true);
    lv_textarea_set_password_mode(s_wifi_pass_ta, false);
    lv_textarea_set_placeholder_text(s_wifi_pass_ta, "Wi-Fi Password");
    if (prefill_pass[0] != '\0') {
        lv_textarea_set_text(s_wifi_pass_ta, prefill_pass);
    }
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
        g_app.wifi_scanning = true;
        g_app.wifi_modal_open = true;
        s_wifi_rendered_scan_seq = g_app.wifi_scan_done_seq;
        xSemaphoreGive(g_app.mutex);
    }

    if (s_wifi_modal) lv_obj_del(s_wifi_modal);
    s_wifi_kb = NULL;
    s_wifi_pass_ta = NULL;
    s_wifi_modal = lv_obj_create(s_scr);
    lv_obj_set_size(s_wifi_modal, 360, 360);
    lv_obj_center(s_wifi_modal);
    lv_obj_set_style_radius(s_wifi_modal, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_wifi_modal, hex_to_lv(0x050608), 0);
    lv_obj_set_style_border_width(s_wifi_modal, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_modal, 0, 0);
    lv_obj_clear_flag(s_wifi_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(s_wifi_modal);
    lv_label_set_text(hdr, "Select Wi-Fi Network");
    lv_obj_set_style_text_font(hdr, &ui_font_md, 0);
    lv_obj_set_style_text_color(hdr, hex_to_lv(0x94A3B8), 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 24);

    s_wifi_list = lv_obj_create(s_wifi_modal);
    lv_obj_set_size(s_wifi_list, 276, 204);
    lv_obj_align(s_wifi_list, LV_ALIGN_CENTER, 0, -4);
    lv_obj_set_style_bg_opa(s_wifi_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wifi_list, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_list, 4, 0);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_wifi_list, 6, 0);
    lv_obj_set_scroll_dir(s_wifi_list, LV_DIR_VER);

    lv_obj_t *scanning_lbl = lv_label_create(s_wifi_list);
    lv_label_set_text(scanning_lbl, "Scanning 2.4 GHz networks...");
    lv_obj_set_style_text_font(scanning_lbl, &ui_font_md, 0);
    lv_obj_set_style_text_color(scanning_lbl, hex_to_lv(0x38BDF8), 0);

    lv_obj_t *rescan_btn = lv_btn_create(s_wifi_modal);
    lv_obj_set_size(rescan_btn, 92, 32);
    lv_obj_align(rescan_btn, LV_ALIGN_BOTTOM_MID, -52, -24);
    lv_obj_set_style_radius(rescan_btn, 16, 0);
    lv_obj_set_style_bg_color(rescan_btn, hex_to_lv(0x0F172A), 0);
    lv_obj_set_style_border_width(rescan_btn, 1, 0);
    lv_obj_set_style_border_color(rescan_btn, hex_to_lv(0x38BDF8), 0);
    lv_obj_set_style_pad_all(rescan_btn, 0, 0);
    lv_obj_t *rl = lv_label_create(rescan_btn);
    lv_label_set_text(rl, "Rescan");
    lv_obj_set_style_text_font(rl, &ui_font_sm, 0);
    lv_obj_set_style_text_color(rl, hex_to_lv(0x38BDF8), 0);
    lv_obj_center(rl);
    lv_obj_add_event_cb(rescan_btn, [](lv_event_t *ev) {
        (void)ev;
        if (bsp_was_touch_dragged()) return;
        haptic_play_effect(HAPTIC_SHARP_CLICK);
        if (s_wifi_list) {
            lv_obj_clean(s_wifi_list);
            lv_obj_t *sl = lv_label_create(s_wifi_list);
            lv_label_set_text(sl, "Scanning 2.4 GHz networks...");
            lv_obj_set_style_text_font(sl, &ui_font_md, 0);
            lv_obj_set_style_text_color(sl, hex_to_lv(0x38BDF8), 0);
        }
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_app.wifi_scan_requested = true;
            g_app.wifi_scanning = true;
            s_wifi_rendered_scan_seq = g_app.wifi_scan_done_seq;
            xSemaphoreGive(g_app.mutex);
        }
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close_btn = lv_btn_create(s_wifi_modal);
    lv_obj_set_size(close_btn, 92, 32);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_MID, 52, -24);
    lv_obj_set_style_radius(close_btn, 16, 0);
    lv_obj_set_style_bg_color(close_btn, hex_to_lv(0x1E293B), 0);
    lv_obj_set_style_pad_all(close_btn, 0, 0);
    lv_obj_t *cl = lv_label_create(close_btn);
    lv_label_set_text(cl, "Close");
    lv_obj_set_style_text_font(cl, &ui_font_sm, 0);
    lv_obj_center(cl);
    lv_obj_add_event_cb(close_btn, [](lv_event_t *ev) {
        (void)ev;
        if (bsp_was_touch_dragged()) return;
        haptic_play_effect(HAPTIC_SHARP_CLICK);
        close_wifi_modal();
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_move_foreground(s_wifi_modal);
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
    lv_obj_set_style_text_font(lbl, &ui_font_sm, 0);
    lv_obj_set_style_text_color(lbl, hex_to_lv(text_hex), 0);
    lv_obj_center(lbl);
    return btn;
}

// Unified full-screen touch drag + inertia flick + hit-tested tap handler for 60 FPS 3D wheels
static void wheel_touch_event_cb(lv_event_t *e, bool is_speakers) {
    lv_event_code_t code = lv_event_get_code(e);
    bool &dragging    = is_speakers ? s_sp_dragging    : s_st_dragging;
    float &anim_off   = is_speakers ? s_sp_anim_offset : s_st_anim_offset;
    int &cursor       = is_speakers ? s_speaker_cursor : s_station_cursor;
    int count         = is_speakers ? (g_app.speaker_count > 0 ? g_app.speaker_count : 1)
                                    : (g_app.preset_count > 0 ? g_app.preset_count : 1);
    bool wrap_list    = !is_speakers || (count >= 5);

    if (code == LV_EVENT_PRESSED) {
        if (bsp_is_swipe_consumed()) {
            s_wheel_touch_armed = false;
            return;
        }
        lv_indev_t *indev = lv_indev_get_act();
        lv_point_t pt = {180, 180};
        if (indev) lv_indev_get_point(indev, &pt);

        s_wheel_touch_armed  = true;
        s_wheel_horiz_locked = false;
        s_wheel_start_x      = pt.x;
        s_wheel_start_y      = pt.y;
        s_wheel_last_x       = pt.x;
        s_wheel_last_y       = pt.y;
        s_wheel_start_us     = esp_timer_get_time();
        s_wheel_last_us      = s_wheel_start_us;
        s_wheel_vel_y        = 0.0f;
        s_wheel_items_moved  = 0;
        dragging             = false;

        // If the wheel was mid-momentum spin, catch it smoothly at its current visual item
        int int_shift = (int)floorf(anim_off + 0.5f);
        if (int_shift != 0) {
            if (wrap_list) {
                cursor = ((cursor - int_shift) % count + count) % count;
            } else {
                cursor -= int_shift;
                if (cursor < 0) cursor = 0;
                if (cursor >= count) cursor = count - 1;
            }
            anim_off -= (float)int_shift;
        }
    } else if (code == LV_EVENT_PRESSING) {
        if (!s_wheel_touch_armed || s_wheel_horiz_locked) return;
        lv_indev_t *indev = lv_indev_get_act();
        if (!indev) return;
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);

        int16_t dy       = pt.y - s_wheel_last_y;
        int16_t total_dx = pt.x - s_wheel_start_x;
        int16_t total_dy = pt.y - s_wheel_start_y;

        if (!dragging) {
            if (abs(total_dx) >= 14 && abs(total_dx) > abs(total_dy) + 6) {
                s_wheel_horiz_locked = true;
                return;
            }
            if (abs(total_dy) >= 3 && abs(total_dy) >= abs(total_dx)) {
                dragging = true;
                bsp_suppress_swipe_gesture();
            }
        }

        if (dragging && dy != 0) {
            bsp_suppress_swipe_gesture();
            int64_t now_us = esp_timer_get_time();
            float dt = (float)(now_us - s_wheel_last_us) * 1e-6f;
            if (dt < 0.004f) dt = 0.004f;
            float inst_vy = (float)dy / dt;
            s_wheel_vel_y = (s_wheel_vel_y == 0.0f) ? inst_vy : (0.35f * s_wheel_vel_y + 0.65f * inst_vy);
            s_wheel_last_x = pt.x;
            s_wheel_last_y = pt.y;
            s_wheel_last_us = now_us;

            anim_off += (float)dy / 36.0f;
            while (anim_off >= 0.5f) {
                if (wrap_list) {
                    anim_off -= 1.0f;
                    cursor = (cursor - 1 + count) % count;
                    s_wheel_items_moved++;
                    play_wheel_tick_haptic();
                } else if (cursor > 0) {
                    anim_off -= 1.0f;
                    cursor--;
                    s_wheel_items_moved++;
                    play_wheel_tick_haptic();
                } else {
                    anim_off = 0.42f;
                    break;
                }
            }
            while (anim_off <= -0.5f) {
                if (wrap_list) {
                    anim_off += 1.0f;
                    cursor = (cursor + 1) % count;
                    s_wheel_items_moved++;
                    play_wheel_tick_haptic();
                } else if (cursor < count - 1) {
                    anim_off += 1.0f;
                    cursor++;
                    s_wheel_items_moved++;
                    play_wheel_tick_haptic();
                } else {
                    anim_off = -0.42f;
                    break;
                }
            }
            s_wheel_anim_last_us = now_us;
            if (is_speakers) update_speaker_wheel(false);
            else             update_station_wheel(false);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s_wheel_touch_armed) return;
        if (dragging && !s_wheel_horiz_locked) {
            int64_t now_us   = esp_timer_get_time();
            int16_t total_dy = s_wheel_last_y - s_wheel_start_y;
            int16_t total_dx = s_wheel_last_x - s_wheel_start_x;
            float total_dt   = (float)(now_us - s_wheel_start_us) * 1e-6f;
            if (total_dt < 0.012f) total_dt = 0.012f;
            float chord_vy   = (float)total_dy / total_dt;

            // Apply flick inertia if finger did not pause before lifting
            if (now_us - s_wheel_last_us <= 100000LL) {
                float vy = (fabsf(s_wheel_vel_y) > fabsf(chord_vy)) ? s_wheel_vel_y : chord_vy;
                int extra = 0;
                if (fabsf(vy) >= 240.0f) {
                    extra = (int)(fabsf(vy) / 260.0f);
                    if (extra > 4) extra = 4;
                } else if (s_wheel_items_moved == 0 && abs(total_dy) >= 12 && abs(total_dy) >= abs(total_dx)) {
                    extra = 1;
                }
                if (extra > 0) {
                    int step = (total_dy < 0) ? extra : -extra;
                    if (wrap_list) {
                        cursor = ((cursor + step) % count + count) % count;
                        anim_off += (float)step;
                    } else {
                        int prev_c = cursor;
                        cursor += step;
                        if (cursor < 0) cursor = 0;
                        if (cursor >= count) cursor = count - 1;
                        anim_off += (float)(cursor - prev_c);
                    }
                    if (anim_off > 4.0f)  anim_off = 4.0f;
                    if (anim_off < -4.0f) anim_off = -4.0f;
                    if (s_wheel_items_moved == 0) {
                        play_wheel_tick_haptic();
                    }
                }
            }
            dragging = false;
            s_wheel_anim_last_us = now_us;
            if (is_speakers) update_speaker_wheel(false);
            else             update_station_wheel(false);
        }
    } else if (code == LV_EVENT_CLICKED) {
        if (!s_wheel_touch_armed || s_wheel_horiz_locked) return;
        if (dragging || bsp_was_touch_dragged()) return;
        lv_indev_t *indev = lv_indev_get_act();
        if (!indev) return;
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);

        static const int HIT_ORDER[WHEEL_SLOTS] = {3, 2, 4, 1, 5, 0, 6};
        lv_obj_t **slots = is_speakers ? s_sp_slots : s_st_slots;
        WheelSlotCache *caches = is_speakers ? s_sp_cache : s_st_cache;
        int hit_idx = -1;
        for (int k = 0; k < WHEEL_SLOTS; k++) {
            int s = HIT_ORDER[k];
            lv_obj_t *row = slots[s];
            if (!row || lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN)) continue;
            if (pt.x >= row->coords.x1 - 8 && pt.x <= row->coords.x2 + 8 &&
                pt.y >= row->coords.y1 - 4 && pt.y <= row->coords.y2 + 4) {
                hit_idx = caches[s].item_idx;
                break;
            }
        }
        if (hit_idx < 0) return;

        cursor = hit_idx;
        anim_off = 0.0f;
        haptic_play_effect(HAPTIC_DOUBLE_CLICK);
        if (is_speakers) {
            app_select_speaker(hit_idx);
        } else {
            app_select_preset(hit_idx, true);
        }
        switch_tab(TAB_NOW_PLAYING);
    }
}

static void station_slot_event_cb(lv_event_t *e) {
    wheel_touch_event_cb(e, false);
}

static void speaker_slot_event_cb(lv_event_t *e) {
    wheel_touch_event_cb(e, true);
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
        lv_obj_set_pos(s_pages[i], 0, 0);
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
    set_arc_state(100, g_app.volume);

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
            app_state_save_playback();
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
    // 1. Track Name (y = 240..268, bold Space Grotesk 22px, single-line ellipsis)
    s_lbl_track = lv_label_create(p0);
    lv_label_set_text(s_lbl_track, "Groove Salad");
    lv_obj_set_style_text_font(s_lbl_track, &ui_font_xl, 0);
    lv_obj_set_style_text_color(s_lbl_track, hex_to_lv(0xFFFFFF), 0);
    lv_label_set_long_mode(s_lbl_track, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_lbl_track, 292, 28);
    lv_obj_set_style_text_align(s_lbl_track, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_track, LV_ALIGN_TOP_MID, 0, 240);
    lv_obj_clear_flag(s_lbl_track, LV_OBJ_FLAG_CLICKABLE);

    // 2. Artist Name (y = 270..292, Space Grotesk Medium 15px, single-line ellipsis)
    s_lbl_artist = lv_label_create(p0);
    lv_label_set_text(s_lbl_artist, "SomaFM Radio");
    lv_obj_set_style_text_font(s_lbl_artist, &ui_font_md, 0);
    lv_obj_set_style_text_color(s_lbl_artist, hex_to_lv(0xE2E8F0), 0);
    lv_label_set_long_mode(s_lbl_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_lbl_artist, 264, 22);
    lv_obj_set_style_text_align(s_lbl_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_artist, LV_ALIGN_TOP_MID, 0, 270);
    lv_obj_clear_flag(s_lbl_artist, LV_OBJ_FLAG_CLICKABLE);

    // 3. Station • AirPlay Speaker status line (y = 295..315, Space Grotesk Bold 13px)
    s_lbl_sub_meta = lv_label_create(p0);
    lv_label_set_text(s_lbl_sub_meta, "Choose a station and speaker");
    lv_obj_set_style_text_font(s_lbl_sub_meta, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_lbl_sub_meta, hex_to_lv(0x10B981), 0);
    lv_label_set_long_mode(s_lbl_sub_meta, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_lbl_sub_meta, 248, 20);
    lv_obj_set_style_text_align(s_lbl_sub_meta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_sub_meta, LV_ALIGN_TOP_MID, 0, 295);
    lv_obj_clear_flag(s_lbl_sub_meta, LV_OBJ_FLAG_CLICKABLE);

    // =========================================================================
    // PAGE 1: 60 FPS VIRTUALIZED SOMAFM STATIONS WHEEL (Swipe Left from Home)
    // Created in 3D cylindrical painter's order (horizon -> outer -> inner -> center)
    // =========================================================================
    lv_obj_t *p1 = s_pages[1];
    lv_obj_add_flag(p1, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p1, station_slot_event_cb, LV_EVENT_ALL, NULL);

    static const int CREATE_ORDER[WHEEL_SLOTS] = {0, 6, 1, 5, 2, 4, 3};

    for (int k = 0; k < WHEEL_SLOTS; k++) {
        int i = CREATE_ORDER[k];
        bool is_center = (i == WHEEL_CENTER_SLOT);

        lv_obj_t *row = lv_obj_create(p1);
        lv_obj_set_size(row, is_center ? 300 : 282, is_center ? 44 : 38);
        lv_obj_set_pos(row, is_center ? 30 : 39, 140);
        lv_obj_set_style_radius(row, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(row, hex_to_lv(is_center ? 0x141B28 : 0x0A0E15), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, is_center ? 2 : 1, 0);
        lv_obj_set_style_border_color(row, hex_to_lv(is_center ? 0x10B981 : 0x151D2A), 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_set_size(dot, 6, is_center ? 22 : 16);
        lv_obj_set_style_radius(dot, 3, 0);
        lv_obj_set_style_bg_color(dot, hex_to_lv(0x10B981), 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(row);
        lv_obj_set_style_text_font(title, is_center ? &ui_font_lg : &ui_font_md, 0);
        lv_obj_set_style_text_color(title, hex_to_lv(0xFFFFFF), 0);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_size(title, 214, is_center ? 24 : 20);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 26, 0);
        lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *tag = lv_label_create(row);
        lv_obj_set_style_text_font(tag, &ui_font_sm, 0);
        lv_obj_set_style_text_color(tag, hex_to_lv(0x64748B), 0);
        lv_label_set_long_mode(tag, LV_LABEL_LONG_CLIP);
        lv_obj_set_size(tag, 64, 18);
        lv_obj_set_style_text_align(tag, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(tag, LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_clear_flag(tag, LV_OBJ_FLAG_CLICKABLE);

        s_st_slots[i] = row;
        s_st_dots[i] = dot;
        s_st_titles[i] = title;
        s_st_tags[i] = tag;
    }

    s_lbl_st_hdr = lv_label_create(p1);
    lv_label_set_text(s_lbl_st_hdr, "STATIONS  •  1 / 30");
    lv_obj_set_style_text_font(s_lbl_st_hdr, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_lbl_st_hdr, hex_to_lv(0x94A3B8), 0);
    lv_obj_align(s_lbl_st_hdr, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_clear_flag(s_lbl_st_hdr, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *st_hint = lv_label_create(p1);
    lv_label_set_text(st_hint, "TAP TO TUNE  •  SWIPE BACK");
    lv_obj_set_style_text_font(st_hint, &ui_font_sm, 0);
    lv_obj_set_style_text_color(st_hint, hex_to_lv(0x475569), 0);
    lv_obj_align(st_hint, LV_ALIGN_TOP_MID, 0, 290);
    lv_obj_clear_flag(st_hint, LV_OBJ_FLAG_CLICKABLE);

    update_station_wheel(true);

    // =========================================================================
    // PAGE 2: 60 FPS VIRTUALIZED AIRPLAY SPEAKERS WHEEL (Swipe Right from Home)
    // Created in 3D cylindrical painter's order (horizon -> outer -> inner -> center)
    // =========================================================================
    lv_obj_t *p2 = s_pages[2];
    lv_obj_add_flag(p2, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p2, speaker_slot_event_cb, LV_EVENT_ALL, NULL);

    for (int k = 0; k < WHEEL_SLOTS; k++) {
        int i = CREATE_ORDER[k];
        bool is_center = (i == WHEEL_CENTER_SLOT);

        lv_obj_t *row = lv_obj_create(p2);
        lv_obj_set_size(row, is_center ? 300 : 282, is_center ? 44 : 38);
        lv_obj_set_pos(row, is_center ? 30 : 39, 140);
        lv_obj_set_style_radius(row, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(row, hex_to_lv(is_center ? 0x141B28 : 0x0A0E15), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, is_center ? 2 : 1, 0);
        lv_obj_set_style_border_color(row, hex_to_lv(is_center ? 0x38BDF8 : 0x151D2A), 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_set_size(dot, 6, is_center ? 22 : 16);
        lv_obj_set_style_radius(dot, 3, 0);
        lv_obj_set_style_bg_color(dot, hex_to_lv(0x38BDF8), 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(row);
        lv_obj_set_style_text_font(title, is_center ? &ui_font_lg : &ui_font_md, 0);
        lv_obj_set_style_text_color(title, hex_to_lv(0xFFFFFF), 0);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_size(title, 214, is_center ? 24 : 20);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 26, 0);
        lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *tag = lv_label_create(row);
        lv_obj_set_style_text_font(tag, &ui_font_sm, 0);
        lv_obj_set_style_text_color(tag, hex_to_lv(0x38BDF8), 0);
        lv_label_set_long_mode(tag, LV_LABEL_LONG_CLIP);
        lv_obj_set_size(tag, 66, 18);
        lv_obj_set_style_text_align(tag, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(tag, LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_clear_flag(tag, LV_OBJ_FLAG_CLICKABLE);

        s_sp_slots[i] = row;
        s_sp_dots[i] = dot;
        s_sp_titles[i] = title;
        s_sp_tags[i] = tag;
    }

    s_lbl_spkr_hdr = lv_label_create(p2);
    lv_label_set_text(s_lbl_spkr_hdr, "AIRPLAY SPEAKERS");
    lv_obj_set_style_text_font(s_lbl_spkr_hdr, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_lbl_spkr_hdr, hex_to_lv(0x94A3B8), 0);
    lv_obj_align(s_lbl_spkr_hdr, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_clear_flag(s_lbl_spkr_hdr, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *sp_hint = lv_label_create(p2);
    lv_label_set_text(sp_hint, "TAP TO CONNECT  •  SWIPE BACK");
    lv_obj_set_style_text_font(sp_hint, &ui_font_sm, 0);
    lv_obj_set_style_text_color(sp_hint, hex_to_lv(0x475569), 0);
    lv_obj_align(sp_hint, LV_ALIGN_TOP_MID, 0, 290);
    lv_obj_clear_flag(sp_hint, LV_OBJ_FLAG_CLICKABLE);

    update_speaker_wheel(true);

    // =========================================================================
    // PAGE 3: SETTINGS & LIVE TELEMETRY DASHBOARD
    // (Swipe Down from Home: Live Buffers, Streaming Status, Tap-for-QR, PIN Pair)
    // =========================================================================
    lv_obj_t *p3 = s_pages[3];
    lv_obj_add_flag(p3, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p3, open_qr_modal, LV_EVENT_CLICKED, NULL);

    s_lbl_wifi_ssid = lv_label_create(p3);
    lv_label_set_text_fmt(s_lbl_wifi_ssid, LV_SYMBOL_WIFI " %s", g_app.wifi_ssid[0] ? g_app.wifi_ssid : "Select network");
    lv_obj_set_style_text_font(s_lbl_wifi_ssid, &ui_font_md, 0);
    lv_obj_set_style_text_color(s_lbl_wifi_ssid, hex_to_lv(0xF8FAFC), 0);
    lv_obj_align(s_lbl_wifi_ssid, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_clear_flag(s_lbl_wifi_ssid, LV_OBJ_FLAG_CLICKABLE);

    s_lbl_wifi_link = lv_label_create(p3);
    lv_label_set_text(s_lbl_wifi_link, "Negotiating Wi-Fi Link...");
    lv_obj_set_style_text_font(s_lbl_wifi_link, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_lbl_wifi_link, hex_to_lv(0x10B981), 0);
    lv_obj_align(s_lbl_wifi_link, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_clear_flag(s_lbl_wifi_link, LV_OBJ_FLAG_CLICKABLE);

    s_lbl_wifi_ip = lv_label_create(p3);
    lv_label_set_text(s_lbl_wifi_ip, "http://airplayknob.local/  •  Tap for QR");
    lv_obj_set_style_text_font(s_lbl_wifi_ip, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_lbl_wifi_ip, hex_to_lv(0x38BDF8), 0);
    lv_obj_align(s_lbl_wifi_ip, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_clear_flag(s_lbl_wifi_ip, LV_OBJ_FLAG_CLICKABLE);

    // Card 1: PCM Ringbuffer Telemetry (2.0 MB PSRAM)
    lv_obj_t *pcm_card = lv_obj_create(p3);
    lv_obj_set_size(pcm_card, 286, 44);
    lv_obj_align(pcm_card, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_set_style_radius(pcm_card, 12, 0);
    lv_obj_set_style_bg_color(pcm_card, hex_to_lv(0x0B0F17), 0);
    lv_obj_set_style_bg_opa(pcm_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pcm_card, 1, 0);
    lv_obj_set_style_border_color(pcm_card, hex_to_lv(0x162030), 0);
    lv_obj_set_style_pad_all(pcm_card, 0, 0);
    lv_obj_clear_flag(pcm_card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *pcm_hdr = lv_label_create(pcm_card);
    lv_label_set_text(pcm_hdr, "PCM BUFFER");
    lv_obj_set_style_text_font(pcm_hdr, &ui_font_sm, 0);
    lv_obj_set_style_text_color(pcm_hdr, hex_to_lv(0x94A3B8), 0);
    lv_obj_align(pcm_hdr, LV_ALIGN_TOP_LEFT, 12, 6);

    s_lbl_buf_pcm = lv_label_create(pcm_card);
    lv_label_set_text_fmt(s_lbl_buf_pcm, "0 KB / %u KB (0.0s)", (unsigned)AudioBuffer::PCM_KIB);
    lv_obj_set_style_text_font(s_lbl_buf_pcm, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_lbl_buf_pcm, hex_to_lv(0x10B981), 0);
    lv_obj_align(s_lbl_buf_pcm, LV_ALIGN_TOP_RIGHT, -12, 6);

    s_bar_buf_pcm = lv_bar_create(pcm_card);
    lv_obj_set_size(s_bar_buf_pcm, 262, 6);
    lv_obj_align(s_bar_buf_pcm, LV_ALIGN_BOTTOM_MID, 0, -7);
    lv_bar_set_range(s_bar_buf_pcm, 0, 100);
    lv_bar_set_value(s_bar_buf_pcm, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar_buf_pcm, hex_to_lv(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_buf_pcm, hex_to_lv(0x10B981), LV_PART_INDICATOR);

    // Card 2: MP3 Stream & Decoder Buffer (64 KB)
    lv_obj_t *mp3_card = lv_obj_create(p3);
    lv_obj_set_size(mp3_card, 304, 44);
    lv_obj_align(mp3_card, LV_ALIGN_TOP_MID, 0, 131);
    lv_obj_set_style_radius(mp3_card, 12, 0);
    lv_obj_set_style_bg_color(mp3_card, hex_to_lv(0x0B0F17), 0);
    lv_obj_set_style_bg_opa(mp3_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mp3_card, 1, 0);
    lv_obj_set_style_border_color(mp3_card, hex_to_lv(0x162030), 0);
    lv_obj_set_style_pad_all(mp3_card, 0, 0);
    lv_obj_clear_flag(mp3_card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *mp3_hdr = lv_label_create(mp3_card);
    lv_label_set_text(mp3_hdr, "MP3 STREAM");
    lv_obj_set_style_text_font(mp3_hdr, &ui_font_sm, 0);
    lv_obj_set_style_text_color(mp3_hdr, hex_to_lv(0x94A3B8), 0);
    lv_obj_align(mp3_hdr, LV_ALIGN_TOP_LEFT, 12, 6);

    s_lbl_buf_mp3 = lv_label_create(mp3_card);
    lv_label_set_text(s_lbl_buf_mp3, "0 / 64 KB  •  128 kbps");
    lv_obj_set_style_text_font(s_lbl_buf_mp3, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_lbl_buf_mp3, hex_to_lv(0x38BDF8), 0);
    lv_obj_align(s_lbl_buf_mp3, LV_ALIGN_TOP_RIGHT, -12, 6);

    s_bar_buf_mp3 = lv_bar_create(mp3_card);
    lv_obj_set_size(s_bar_buf_mp3, 280, 6);
    lv_obj_align(s_bar_buf_mp3, LV_ALIGN_BOTTOM_MID, 0, -7);
    lv_bar_set_range(s_bar_buf_mp3, 0, 100);
    lv_bar_set_value(s_bar_buf_mp3, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar_buf_mp3, hex_to_lv(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_buf_mp3, hex_to_lv(0x38BDF8), LV_PART_INDICATOR);

    // Card 3: AirPlay & Streaming Engine Status
    lv_obj_t *str_card = lv_obj_create(p3);
    lv_obj_set_size(str_card, 304, 46);
    lv_obj_align(str_card, LV_ALIGN_TOP_MID, 0, 180);
    lv_obj_set_style_radius(str_card, 12, 0);
    lv_obj_set_style_bg_color(str_card, hex_to_lv(0x0B0F17), 0);
    lv_obj_set_style_bg_opa(str_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(str_card, 1, 0);
    lv_obj_set_style_border_color(str_card, hex_to_lv(0x162030), 0);
    lv_obj_set_style_pad_all(str_card, 0, 0);
    lv_obj_clear_flag(str_card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_lbl_stream_state = lv_label_create(str_card);
    lv_label_set_text(s_lbl_stream_state, "STREAMING  •  READY");
    lv_obj_set_style_text_font(s_lbl_stream_state, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_lbl_stream_state, hex_to_lv(0xF8FAFC), 0);
    lv_label_set_long_mode(s_lbl_stream_state, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_lbl_stream_state, 280, 18);
    lv_obj_set_style_text_align(s_lbl_stream_state, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_stream_state, LV_ALIGN_TOP_MID, 0, 5);

    s_lbl_stream_pkts = lv_label_create(str_card);
    lv_label_set_text(s_lbl_stream_pkts, "Queued: 0  •  Reject: 0  •  Repair: 0");
    lv_obj_set_style_text_font(s_lbl_stream_pkts, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_lbl_stream_pkts, hex_to_lv(0x64748B), 0);
    lv_label_set_long_mode(s_lbl_stream_pkts, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_lbl_stream_pkts, 284, 18);
    lv_obj_set_style_text_align(s_lbl_stream_pkts, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_stream_pkts, LV_ALIGN_TOP_MID, 0, 24);

    // Bottom Action Buttons: Wi-Fi | QR Code | Pair PIN
    lv_obj_t *w_scan_btn = create_pill_btn(p3, 80, 32, 0x0F141D, 0x1E293B, "Wi-Fi", 0xCBD5E1);
    lv_obj_align(w_scan_btn, LV_ALIGN_TOP_MID, -86, 236);
    lv_obj_add_event_cb(w_scan_btn, open_wifi_scan_modal, LV_EVENT_CLICKED, NULL);

    lv_obj_t *w_qr_btn = create_pill_btn(p3, 84, 32, 0x0F141D, 0x38BDF8, "QR Code", 0x38BDF8);
    lv_obj_align(w_qr_btn, LV_ALIGN_TOP_MID, 0, 236);
    lv_obj_add_event_cb(w_qr_btn, open_qr_modal, LV_EVENT_CLICKED, NULL);

    lv_obj_t *w_pin_btn = create_pill_btn(p3, 80, 32, 0x0F141D, 0xF59E0B, "Pair PIN", 0xF59E0B);
    lv_obj_align(w_pin_btn, LV_ALIGN_TOP_MID, 86, 236);
    lv_obj_add_event_cb(w_pin_btn, [](lv_event_t *e) {
        (void)e;
        if (bsp_was_touch_dragged()) return;
        haptic_play_effect(HAPTIC_SHARP_CLICK);
        bool is_local = true;
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (g_app.current_speaker >= 0 && g_app.current_speaker < g_app.speaker_count) {
                is_local = g_app.speakers[g_app.current_speaker].is_local_dac;
            }
            xSemaphoreGive(g_app.mutex);
        }
        if (is_local) {
            show_hud("AIRPLAY PIN", "Select AirPlay Speaker", 0xF59E0B, 1800);
            return;
        }
        app_request_airplay_pin_pair();
        show_hud("AIRPLAY PIN", "Requesting PIN...", 0xF59E0B, 2200);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *set_hint = lv_label_create(p3);
    lv_label_set_text(set_hint, "TAP FOR QR  •  SWIPE UP HOME");
    lv_obj_set_style_text_font(set_hint, &ui_font_sm, 0);
    lv_obj_set_style_text_color(set_hint, hex_to_lv(0x475569), 0);
    lv_obj_align(set_hint, LV_ALIGN_TOP_MID, 0, 286);
    lv_obj_clear_flag(set_hint, LV_OBJ_FLAG_CLICKABLE);

    // =========================================================================
    // FULL-SCREEN WHITECAP VISUALIZER LAYER (360x360, Swipe Up from Home)
    // Renders directly into internal DMA SRAM strips via LV_EVENT_DRAW_MAIN!
    // =========================================================================
    s_wc_layer = lv_obj_create(s_scr);
    lv_obj_set_size(s_wc_layer, WC_W, WC_H);
    lv_obj_set_pos(s_wc_layer, 0, 0);
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
            app_state_save_playback();
            haptic_play_effect(HAPTIC_LIGHT_TICK);
            show_whitecap_caption(lv_label_get_text(s_lbl_track), WHITECAP_PRESET_NAMES[next_p], 2800);
        } else {
            set_whitecap_visible(false);
        }
    }, LV_EVENT_CLICKED, NULL);

    s_wc_caption_box = lv_obj_create(s_wc_layer);
    lv_obj_set_size(s_wc_caption_box, 256, 54);
    lv_obj_align(s_wc_caption_box, LV_ALIGN_TOP_MID, 0, 272);
    lv_obj_set_style_radius(s_wc_caption_box, 27, 0);
    lv_obj_set_style_bg_color(s_wc_caption_box, hex_to_lv(0x050608), 0);
    lv_obj_set_style_bg_opa(s_wc_caption_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wc_caption_box, 1, 0);
    lv_obj_set_style_border_color(s_wc_caption_box, hex_to_lv(0x1E293B), 0);
    lv_obj_set_style_pad_all(s_wc_caption_box, 0, 0);
    lv_obj_clear_flag(s_wc_caption_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_wc_lbl_track = lv_label_create(s_wc_caption_box);
    lv_label_set_text(s_wc_lbl_track, "Groove Salad");
    lv_obj_set_style_text_font(s_wc_lbl_track, &ui_font_md, 0);
    lv_obj_set_style_text_color(s_wc_lbl_track, hex_to_lv(0xF8FAFC), 0);
    lv_label_set_long_mode(s_wc_lbl_track, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_wc_lbl_track, 228, 20);
    lv_obj_set_style_text_align(s_wc_lbl_track, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wc_lbl_track, LV_ALIGN_TOP_MID, 0, 8);

    s_wc_lbl_sub = lv_label_create(s_wc_caption_box);
    lv_label_set_text(s_wc_lbl_sub, WHITECAP_PRESET_NAMES[0]);
    lv_obj_set_style_text_font(s_wc_lbl_sub, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_wc_lbl_sub, hex_to_lv(0x94A3B8), 0);
    lv_label_set_long_mode(s_wc_lbl_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_wc_lbl_sub, 228, 18);
    lv_obj_set_style_text_align(s_wc_lbl_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wc_lbl_sub, LV_ALIGN_TOP_MID, 0, 29);

    // Register 60+ FPS WhiteCap and Wheel Scroll render timers inside LVGL
    lv_timer_create(whitecap_lv_timer_cb, 1, NULL);
    lv_timer_create(wheel_anim_lv_timer_cb, 1, NULL);

    // =========================================================================
    // MINIMALIST FLOATING HUD PILL (Opaque cover when turning physical knob)
    // =========================================================================
    s_hud_pill = lv_obj_create(s_scr);
    lv_obj_set_size(s_hud_pill, 236, 58);
    lv_obj_align(s_hud_pill, LV_ALIGN_TOP_MID, 0, 256);
    lv_obj_set_style_radius(s_hud_pill, 29, 0);
    lv_obj_set_style_bg_color(s_hud_pill, hex_to_lv(0x0B0F19), 0);
    lv_obj_set_style_bg_opa(s_hud_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_hud_pill, 1, 0);
    lv_obj_set_style_border_color(s_hud_pill, hex_to_lv(0x10B981), 0);
    lv_obj_set_style_pad_all(s_hud_pill, 0, 0);
    lv_obj_clear_flag(s_hud_pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_hud_title = lv_label_create(s_hud_pill);
    lv_label_set_text(s_lbl_hud_title, "VOLUME");
    lv_obj_set_style_text_font(s_lbl_hud_title, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_lbl_hud_title, hex_to_lv(0x10B981), 0);
    lv_label_set_long_mode(s_lbl_hud_title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_lbl_hud_title, 196, 18);
    lv_obj_set_style_text_align(s_lbl_hud_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_hud_title, LV_ALIGN_TOP_MID, 0, 8);

    s_lbl_hud_val = lv_label_create(s_hud_pill);
    lv_label_set_text(s_lbl_hud_val, "18%");
    lv_obj_set_style_text_font(s_lbl_hud_val, &ui_font_lg, 0);
    lv_obj_set_style_text_color(s_lbl_hud_val, hex_to_lv(0xFFFFFF), 0);
    lv_label_set_long_mode(s_lbl_hud_val, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_lbl_hud_val, 204, 24);
    lv_obj_set_style_text_align(s_lbl_hud_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_hud_val, LV_ALIGN_TOP_MID, 0, 27);

    lv_obj_add_flag(s_hud_pill, LV_OBJ_FLAG_HIDDEN);

    // =========================================================================
    // ANIMATED STARTUP & WI-FI CONNECTING SCREEN (Pure Black Background)
    // Shows animated sweep ring, target SSID, IP & negotiated Wi-Fi speed
    // =========================================================================
    s_conn_layer = lv_obj_create(s_scr);
    lv_obj_set_size(s_conn_layer, 360, 360);
    lv_obj_center(s_conn_layer);
    lv_obj_set_style_radius(s_conn_layer, 0, 0);
    lv_obj_set_style_bg_color(s_conn_layer, hex_to_lv(0x000000), 0);
    lv_obj_set_style_bg_opa(s_conn_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_conn_layer, 0, 0);
    lv_obj_set_style_pad_all(s_conn_layer, 0, 0);
    lv_obj_clear_flag(s_conn_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_conn_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_conn_layer, [](lv_event_t *e) {
        (void)e;
        if (bsp_was_touch_dragged()) return;
        s_conn_dismissed = true;
        lv_obj_add_flag(s_conn_layer, LV_OBJ_FLAG_HIDDEN);
        haptic_play_effect(HAPTIC_SHARP_CLICK);
    }, LV_EVENT_CLICKED, NULL);

    s_conn_lbl_title = lv_label_create(s_conn_layer);
    lv_label_set_text(s_conn_lbl_title, "AIRPLAY KNOB");
    lv_obj_set_style_text_font(s_conn_lbl_title, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_conn_lbl_title, hex_to_lv(0x64748B), 0);
    lv_obj_align(s_conn_lbl_title, LV_ALIGN_TOP_MID, 0, 26);

    s_conn_pulse_ring = lv_obj_create(s_conn_layer);
    lv_obj_set_size(s_conn_pulse_ring, 76, 76);
    lv_obj_align(s_conn_pulse_ring, LV_ALIGN_TOP_MID, 0, 68);
    lv_obj_set_style_radius(s_conn_pulse_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_conn_pulse_ring, hex_to_lv(0x09131C), 0);
    lv_obj_set_style_bg_opa(s_conn_pulse_ring, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_conn_pulse_ring, 1, 0);
    lv_obj_set_style_border_color(s_conn_pulse_ring, hex_to_lv(0x1E293B), 0);
    lv_obj_set_style_pad_all(s_conn_pulse_ring, 0, 0);
    lv_obj_clear_flag(s_conn_pulse_ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_conn_lbl_icon = lv_label_create(s_conn_pulse_ring);
    lv_label_set_text(s_conn_lbl_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_conn_lbl_icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_conn_lbl_icon, hex_to_lv(0x38BDF8), 0);
    lv_obj_center(s_conn_lbl_icon);

    s_conn_spinner_arc = lv_arc_create(s_conn_layer);
    lv_obj_set_size(s_conn_spinner_arc, 98, 98);
    lv_obj_align(s_conn_spinner_arc, LV_ALIGN_TOP_MID, 0, 57);
    lv_arc_set_bg_angles(s_conn_spinner_arc, 0, 360);
    lv_arc_set_angles(s_conn_spinner_arc, 0, 95);
    lv_obj_remove_style(s_conn_spinner_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_conn_spinner_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_conn_spinner_arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_conn_spinner_arc, hex_to_lv(0x111827), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_conn_spinner_arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_conn_spinner_arc, hex_to_lv(0x38BDF8), LV_PART_INDICATOR);

    s_conn_lbl_ssid = lv_label_create(s_conn_layer);
    lv_label_set_text(s_conn_lbl_ssid, g_app.wifi_ssid[0] ? g_app.wifi_ssid : "Select network");
    lv_obj_set_style_text_font(s_conn_lbl_ssid, &ui_font_xl, 0);
    lv_obj_set_style_text_color(s_conn_lbl_ssid, hex_to_lv(0xFFFFFF), 0);
    lv_label_set_long_mode(s_conn_lbl_ssid, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_conn_lbl_ssid, 280, 28);
    lv_obj_set_style_text_align(s_conn_lbl_ssid, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_conn_lbl_ssid, LV_ALIGN_TOP_MID, 0, 170);

    s_conn_lbl_status = lv_label_create(s_conn_layer);
    lv_label_set_text(s_conn_lbl_status, "Joining Wi-Fi Network...");
    lv_obj_set_style_text_font(s_conn_lbl_status, &ui_font_md, 0);
    lv_obj_set_style_text_color(s_conn_lbl_status, hex_to_lv(0x38BDF8), 0);
    lv_obj_set_size(s_conn_lbl_status, 290, 22);
    lv_obj_set_style_text_align(s_conn_lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_conn_lbl_status, LV_ALIGN_TOP_MID, 0, 202);

    s_conn_lbl_speed = lv_label_create(s_conn_layer);
    lv_label_set_text(s_conn_lbl_speed, "Negotiating PHY Rate & DHCP...");
    lv_obj_set_style_text_font(s_conn_lbl_speed, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_conn_lbl_speed, hex_to_lv(0x94A3B8), 0);
    lv_obj_set_size(s_conn_lbl_speed, 296, 20);
    lv_obj_set_style_text_align(s_conn_lbl_speed, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_conn_lbl_speed, LV_ALIGN_TOP_MID, 0, 230);

    s_conn_lbl_ip = lv_label_create(s_conn_layer);
    lv_label_set_text(s_conn_lbl_ip, "Waiting for IP address...");
    lv_obj_set_style_text_font(s_conn_lbl_ip, &ui_font_sm, 0);
    lv_obj_set_style_text_color(s_conn_lbl_ip, hex_to_lv(0x64748B), 0);
    lv_obj_set_size(s_conn_lbl_ip, 280, 20);
    lv_obj_set_style_text_align(s_conn_lbl_ip, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_conn_lbl_ip, LV_ALIGN_TOP_MID, 0, 254);

    s_conn_wifi_btn = create_pill_btn(s_conn_layer, 118, 30, 0x0F141D, 0x1E293B, "Change Wi-Fi", 0xCBD5E1);
    lv_obj_align(s_conn_wifi_btn, LV_ALIGN_TOP_MID, 0, 292);
    lv_obj_add_event_cb(s_conn_wifi_btn, open_wifi_scan_modal, LV_EVENT_CLICKED, NULL);

    lv_obj_move_foreground(s_conn_layer);

    bsp_lvgl_unlock();
}

void knob_ui_on_rotate(int delta) {
    if (delta == 0) return;
    if (!bsp_lvgl_lock(30)) return;

    // If 4-Digit AirPlay PIN Modal is open, turning the physical knob dials the active digit (0..9)!
    if (s_pin_modal) {
        int cur_d = s_pin_digits[s_pin_cursor] - '0';
        cur_d = (cur_d + (delta > 0 ? 1 : -1) + 100) % 10;
        s_pin_digits[s_pin_cursor] = (char)('0' + cur_d);
        if (s_pin_entered_len <= s_pin_cursor) {
            s_pin_entered_len = s_pin_cursor + 1;
        }
        haptic_play_effect(HAPTIC_LIGHT_TICK);
        refresh_pin_boxes();
        bsp_lvgl_unlock();
        return;
    }

    // If Wi-Fi Scan list modal is open, turning the physical knob scrolls the network list!
    if (s_wifi_modal && s_wifi_list) {
        lv_obj_scroll_by_bounded(s_wifi_list, 0, -delta * 42, LV_ANIM_OFF);
        haptic_play_effect(HAPTIC_LIGHT_TICK);
        bsp_lvgl_unlock();
        return;
    }

    // 60 FPS O(1) Virtualized Station Wheel Scroll with Smooth Momentum Animation
    if (!g_app.whitecap_active && s_active_tab == TAB_STATIONS) {
        int count = g_app.preset_count > 0 ? g_app.preset_count : 1;
        int steps = delta;
        if (steps > 4) steps = 4;
        if (steps < -4) steps = -4;
        s_station_cursor = ((s_station_cursor + steps) % count + count) % count;
        s_st_anim_offset += (float)steps * 0.95f;
        if (s_st_anim_offset > 3.2f) s_st_anim_offset = 3.2f;
        if (s_st_anim_offset < -3.2f) s_st_anim_offset = -3.2f;
        s_wheel_anim_last_us = esp_timer_get_time();
        update_station_wheel(false);
        bsp_lvgl_unlock();
        haptic_play_effect(HAPTIC_LIGHT_TICK);
        return;
    }

    // 60 FPS O(1) Virtualized AirPlay Speaker Wheel Scroll with Smooth Momentum Animation
    if (!g_app.whitecap_active && s_active_tab == TAB_SPEAKERS) {
        int count = g_app.speaker_count > 0 ? g_app.speaker_count : 1;
        int steps = delta;
        if (steps > 4) steps = 4;
        if (steps < -4) steps = -4;
        if (count >= 5) {
            s_speaker_cursor = ((s_speaker_cursor + steps) % count + count) % count;
            s_sp_anim_offset += (float)steps * 0.95f;
        } else {
            int prev_cur = s_speaker_cursor;
            s_speaker_cursor = ((s_speaker_cursor + steps) % count + count) % count;
            int actual_step = s_speaker_cursor - prev_cur;
            s_sp_anim_offset += (float)actual_step * 0.95f;
        }
        if (s_sp_anim_offset > 3.2f) s_sp_anim_offset = 3.2f;
        if (s_sp_anim_offset < -3.2f) s_sp_anim_offset = -3.2f;
        s_wheel_anim_last_us = esp_timer_get_time();
        update_speaker_wheel(false);
        bsp_lvgl_unlock();
        haptic_play_effect(HAPTIC_LIGHT_TICK);
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
        app_state_save_playback();
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

    static uint32_t s_last_slow_tick_ms = 0;
    if (now_ms - s_last_slow_tick_ms < 35) {
        return;
    }
    s_last_slow_tick_ms = now_ms;

    int commit_preset = -1;
    int commit_speaker = -1;

    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (g_app.preview_timestamp_ms > 0 && (now_ms - g_app.preview_timestamp_ms >= 450)) {
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

    bsp_swipe_dir_t swipe = bsp_pop_swipe_gesture();
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
    int32_t wifi_rssi = 0;
    uint16_t wifi_speed = 0;
    char wifi_phy[24] = {0};
    uint8_t wifi_ch = 0;
    uint32_t wifi_start_ms = 0;
    uint16_t pcm_ms = 0;
    uint16_t pcm_kb = 0;
    uint32_t mp3_bytes = 0;
    uint16_t bitrate = 128;
    uint32_t underruns = 0;
    bool raop_conn = false;
    bool raop_ap2 = true;
    bool sonos_native = false;
    bool local_monitor = false;
    uint32_t raop_pkts = 0;
    uint32_t raop_rejects = 0;
    uint32_t raop_ntp = 0;
    uint32_t raop_retx = 0;
    bool pin_prompt = false;
    bool pin_halted = false;
    char status_txt[64] = {0};
    char pin_spkr[48] = {0};
    bool pin_toast = false;
    char pin_toast_t[32] = {0};
    char pin_toast_m[48] = {0};
    uint32_t pin_toast_c = 0xF59E0B;
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
        wifi_rssi = g_app.wifi_rssi;
        wifi_speed = g_app.wifi_speed_mbps;
        strncpy(wifi_phy, g_app.wifi_phy_mode, sizeof(wifi_phy) - 1);
        wifi_ch = g_app.wifi_channel;
        wifi_start_ms = g_app.wifi_connect_start_ms;
        pcm_ms = g_app.pcm_buf_ms;
        pcm_kb = g_app.pcm_buf_kb;
        mp3_bytes = g_app.mp3_buf_bytes;
        bitrate = g_app.bitrate_kbps;
        underruns = g_app.stream_underruns;
        raop_conn = g_app.raop_connected;
        raop_ap2 = g_app.raop_is_ap2;
        sonos_native = g_app.sonos_native;
        local_monitor = g_app.also_local_dac || g_app.whitecap_active;
        raop_pkts = g_app.raop_tx_accepted;
        raop_rejects = g_app.raop_tx_rejected;
        raop_ntp = g_app.raop_ntp_replies;
        raop_retx = g_app.raop_retransmits;
        pin_prompt = g_app.pin_prompt_active;
        pin_halted = (!cur_sp.is_local_dac && cur_sp.ip[0] != '\0' && g_app.pin_halted_ip[0] != '\0' &&
                      strcmp(cur_sp.ip, g_app.pin_halted_ip) == 0);
        strncpy(status_txt, g_app.status_text, sizeof(status_txt) - 1);
        strncpy(pin_spkr, g_app.pin_speaker_name, sizeof(pin_spkr) - 1);
        if (g_app.pin_toast_pending) {
            pin_toast = true;
            g_app.pin_toast_pending = false;
            strncpy(pin_toast_t, g_app.pin_toast_title, sizeof(pin_toast_t) - 1);
            strncpy(pin_toast_m, g_app.pin_toast_msg, sizeof(pin_toast_m) - 1);
            pin_toast_c = g_app.pin_toast_color;
        }
        wc_active = g_app.whitecap_active;
        ota_active = g_app.ota_in_progress;
        ota_pct = g_app.ota_progress_pct;
        spk_count = g_app.speaker_count;
        spk_scanning = g_app.speaker_scanning;
        xSemaphoreGive(g_app.mutex);
    } else {
        bsp_lvgl_unlock();
        return;
    }

    // Automatically pop up or close 4-Digit AirPlay PIN Modal when requested by RAOP client
    if (pin_prompt && !s_pin_modal) {
        open_pin_modal(pin_spkr[0] ? pin_spkr : cur_sp.name);
    } else if (!pin_prompt && s_pin_modal) {
        close_pin_modal(false);
    }
    if (pin_toast) {
        show_hud(pin_toast_t, pin_toast_m, pin_toast_c, 2600);
    }

    // Animate Startup / Wi-Fi Connecting Screen (shows while connecting + 2.2s after link-up to display negotiated speed)
    if (s_conn_layer && !s_wifi_modal) {
        if (!wifi_ok && s_conn_ready_ms == 0) {
            if (!s_conn_dismissed) {
                lv_obj_clear_flag(s_conn_layer, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(s_conn_layer);

                s_conn_anim_angle = (s_conn_anim_angle + 14) % 360;
                lv_arc_set_angles(s_conn_spinner_arc, s_conn_anim_angle, (s_conn_anim_angle + 100) % 360);
                lv_obj_set_style_arc_color(s_conn_spinner_arc, hex_to_lv(0x38BDF8), LV_PART_INDICATOR);
                lv_obj_set_style_text_color(s_conn_lbl_icon, hex_to_lv(0x38BDF8), 0);

                // Subtle pulse on inner circle border
                bool pulse_hi = ((now_ms / 400) & 1) != 0;
                lv_obj_set_style_border_color(s_conn_pulse_ring, hex_to_lv(pulse_hi ? 0x38BDF8 : 0x1E293B), 0);

                lv_label_set_text(s_conn_lbl_ssid, ssid[0] ? ssid : "Wi-Fi");
                uint32_t elapsed_s = (now_ms > wifi_start_ms) ? ((now_ms - wifi_start_ms) / 1000) : 0;
                const char *phase_str = "Joining Wi-Fi Network...";
                if ((elapsed_s % 3) == 1) phase_str = "Negotiating WPA2 & PHY Link...";
                else if ((elapsed_s % 3) == 2) phase_str = "Requesting DHCP IP Address...";
                lv_label_set_text(s_conn_lbl_status, phase_str);
                lv_obj_set_style_text_color(s_conn_lbl_status, hex_to_lv(0x38BDF8), 0);
                lv_label_set_text_fmt(s_conn_lbl_speed, "Connecting (%lus)  •  2.4 GHz 802.11n", (unsigned long)elapsed_s);
                lv_obj_set_style_text_color(s_conn_lbl_speed, hex_to_lv(0x94A3B8), 0);
                lv_label_set_text(s_conn_lbl_ip, "Waiting for IP...");
            }
        } else if (!s_conn_dismissed) {
            if (s_conn_ready_ms == 0) {
                s_conn_ready_ms = now_ms;
                lv_arc_set_angles(s_conn_spinner_arc, 0, 360);
                lv_obj_set_style_arc_color(s_conn_spinner_arc, hex_to_lv(0x10B981), LV_PART_INDICATOR);
                lv_obj_set_style_border_color(s_conn_pulse_ring, hex_to_lv(0x10B981), 0);
                lv_obj_set_style_text_color(s_conn_lbl_icon, hex_to_lv(0x10B981), 0);
                lv_label_set_text(s_conn_lbl_ssid, ssid);
                lv_label_set_text(s_conn_lbl_status, "Wi-Fi Connected!");
                lv_obj_set_style_text_color(s_conn_lbl_status, hex_to_lv(0x10B981), 0);
                lv_label_set_text_fmt(s_conn_lbl_speed, "%u Mbps  •  %s  •  Ch %u",
                                      (unsigned)wifi_speed,
                                      wifi_phy[0] ? wifi_phy : "802.11n",
                                      (unsigned)wifi_ch);
                lv_obj_set_style_text_color(s_conn_lbl_speed, hex_to_lv(0xFFFFFF), 0);
                lv_label_set_text_fmt(s_conn_lbl_ip, "IP %s  •  %ld dBm", ip, (long)wifi_rssi);
                lv_obj_set_style_text_color(s_conn_lbl_ip, hex_to_lv(0x38BDF8), 0);
                haptic_play_effect(HAPTIC_DOUBLE_CLICK);
            } else if (now_ms - s_conn_ready_ms >= 2200) {
                s_conn_dismissed = true;
                lv_obj_add_flag(s_conn_layer, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (s_conn_dismissed && wc_active != s_prev_wc_active) {
        s_prev_wc_active = wc_active;
        set_whitecap_visible(wc_active);
    }

    if (ota_active) {
        char pstr[32];
        snprintf(pstr, sizeof(pstr), "%d%% Complete", ota_pct);
        show_hud("OTA FIRMWARE UPDATE", pstr, 0x38BDF8, 1000);
        set_arc_state(100, ota_pct);
        bsp_lvgl_unlock();
        return;
    }

    // Refresh Speakers wheel automatically when background mDNS scan updates speakers
    static bool prev_spk_scanning = false;
    static int prev_spk_count = 1;
    if ((prev_spk_scanning != spk_scanning) || (spk_count != prev_spk_count)) {
        prev_spk_scanning = spk_scanning;
        prev_spk_count = spk_count;
        for (int i = 0; i < WHEEL_SLOTS; i++) s_sp_cache[i].item_idx = -1;
        if (s_active_tab == TAB_SPEAKERS) {
            update_speaker_wheel(false);
        }
    }

    // Check if new Album Art or Stream Art arrived from artwork_fetcher
    static uint32_t last_art_seq = 0;
    uint32_t cur_art_seq = artwork_get_seq();
    if (cur_art_seq != last_art_seq) {
        last_art_seq = cur_art_seq;
        if (s_art_img) {
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

    // Update Settings Tab live telemetry (Wi-Fi link speed, PCM & MP3 buffers, RAOP packets)
    static uint32_t s_last_settings_upd_ms = 0;
    if (s_active_tab == TAB_WIFI && (now_ms - s_last_settings_upd_ms >= 200)) {
        s_last_settings_upd_ms = now_ms;
        if (wifi_ok) {
            lv_label_set_text_fmt(s_lbl_wifi_ssid, LV_SYMBOL_WIFI " %s  •  %ld dBm", ssid, (long)wifi_rssi);
            lv_label_set_text_fmt(s_lbl_wifi_link, "%u Mbps  •  %s  •  Ch %u",
                                  (unsigned)wifi_speed,
                                  wifi_phy[0] ? wifi_phy : "802.11n",
                                  (unsigned)wifi_ch);
            lv_label_set_text_fmt(s_lbl_wifi_ip, "http://%s/  •  Tap for QR", ip);
        } else {
            lv_label_set_text_fmt(s_lbl_wifi_ssid, LV_SYMBOL_WIFI " %s (Connecting...)", ssid[0] ? ssid : "Wi-Fi");
            lv_label_set_text(s_lbl_wifi_link, "Negotiating Wi-Fi Link...");
            lv_label_set_text(s_lbl_wifi_ip, "Waiting for DHCP IP...");
        }

        int pcm_pct = ((int)pcm_kb * 100) / AudioBuffer::PCM_KIB;
        if (pcm_pct > 100) pcm_pct = 100;
        lv_bar_set_value(s_bar_buf_pcm, pcm_pct, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_lbl_buf_pcm, "%u / %u KB (%u.%us)",
                              (unsigned)pcm_kb, (unsigned)AudioBuffer::PCM_KIB, (unsigned)(pcm_ms / 1000), (unsigned)((pcm_ms % 1000) / 100));

        int mp3_kb_x10 = (int)((mp3_bytes * 10) / 1024);
        int mp3_pct = (int)((mp3_bytes * 100) / 262144);
        if (mp3_pct > 100) mp3_pct = 100;
        lv_bar_set_value(s_bar_buf_mp3, mp3_pct, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_lbl_buf_mp3, "%d.%d / 256 KB  •  %u kbps",
                              mp3_kb_x10 / 10, mp3_kb_x10 % 10, (unsigned)(bitrate ? bitrate : 128));
        if (sonos_native && !local_monitor) {
            lv_label_set_text(s_lbl_buf_pcm, "Audio buffered by Sonos");
            lv_label_set_text(s_lbl_buf_mp3, "Local monitoring off");
        }

        if (!playing) {
            lv_label_set_text_fmt(s_lbl_stream_state, "PAUSED  •  %s", cur_sp.name[0] ? cur_sp.name : "Speaker");
            lv_obj_set_style_text_color(s_lbl_stream_state, hex_to_lv(0x94A3B8), 0);
        } else if (cur_sp.is_local_dac) {
            lv_label_set_text(s_lbl_stream_state, "LOCAL 3.5MM DAC  •  I2S STREAMING");
            lv_obj_set_style_text_color(s_lbl_stream_state, hex_to_lv(0x10B981), 0);
        } else if (sonos_native) {
            lv_label_set_text_fmt(s_lbl_stream_state, "SONOS RADIO  •  %s", cur_sp.name);
            lv_obj_set_style_text_color(s_lbl_stream_state, hex_to_lv(0x10B981), 0);
        } else if (raop_conn) {
            lv_label_set_text_fmt(s_lbl_stream_state, "%s  •  %s",
                                  raop_ap2 ? "AIRPLAY 2 (ChaCha20)" : "AIRPLAY 1 (AES)",
                                  cur_sp.name);
            lv_obj_set_style_text_color(s_lbl_stream_state, hex_to_lv(0x10B981), 0);
        } else if (pin_halted) {
            lv_label_set_text(s_lbl_stream_state, status_txt[0] ? status_txt : "PIN Cancelled / Locked");
            lv_obj_set_style_text_color(s_lbl_stream_state, hex_to_lv(0xEF4444), 0);
        } else {
            lv_label_set_text_fmt(s_lbl_stream_state, "CONNECTING  •  %s", cur_sp.name);
            lv_obj_set_style_text_color(s_lbl_stream_state, hex_to_lv(0xF59E0B), 0);
        }

        if (sonos_native) lv_label_set_text(s_lbl_stream_pkts, "Playing directly on speaker");
        else lv_label_set_text_fmt(s_lbl_stream_pkts, "Queued %lu • Reject %lu • Repair %lu",
                              (unsigned long)raop_pkts,
                              (unsigned long)raop_rejects,
                              (unsigned long)raop_retx);
    }

    // Update QR modal if open and IP changed
    static char prev_ip[20] = {0};
    if (wifi_ok && strcmp(ip, prev_ip) != 0 && ip[0] != '\0') {
        strncpy(prev_ip, ip, sizeof(prev_ip) - 1);
        char url[64];
        snprintf(url, sizeof(url), "http://%s/", ip);
        if (s_qr_code) {
            lv_qrcode_update(s_qr_code, url, strlen(url));
        }
        if (s_qr_lbl_url) {
            lv_label_set_text(s_qr_lbl_url, url);
        }
    }

    // Populate Wi-Fi scan modal if open and scan finished (using monotonic sequence counter so fast scans are never missed!)
    if (s_wifi_modal && s_wifi_list) {
        bool cur_scanning = false;
        uint32_t done_seq = 0;
        int scan_cnt = 0;
        WifiScanItem items[MAX_WIFI_SCAN];
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            cur_scanning = g_app.wifi_scanning;
            done_seq = g_app.wifi_scan_done_seq;
            scan_cnt = g_app.wifi_scan_count;
            memcpy(items, g_app.wifi_scan, sizeof(items));
            xSemaphoreGive(g_app.mutex);
        }
        if (!cur_scanning && done_seq != s_wifi_rendered_scan_seq) {
            s_wifi_rendered_scan_seq = done_seq;
            lv_obj_clean(s_wifi_list);
            if (scan_cnt <= 0) {
                lv_obj_t *empty_lbl = lv_label_create(s_wifi_list);
                lv_label_set_text(empty_lbl, "No networks found.\nTap Rescan below.");
                lv_obj_set_style_text_font(empty_lbl, &ui_font_sm, 0);
                lv_obj_set_style_text_color(empty_lbl, hex_to_lv(0x94A3B8), 0);
            } else {
                for (int i = 0; i < scan_cnt; i++) {
                    bool is_cur = (ssid[0] != '\0' && strcasecmp(items[i].ssid, ssid) == 0);
                    lv_obj_t *b = lv_btn_create(s_wifi_list);
                    lv_obj_set_size(b, 260, 38);
                    lv_obj_set_style_radius(b, 10, 0);
                    lv_obj_set_style_bg_color(b, hex_to_lv(is_cur ? 0x112220 : 0x141B28), 0);
                    lv_obj_set_style_border_width(b, 1, 0);
                    lv_obj_set_style_border_color(b, hex_to_lv(is_cur ? 0x10B981 : 0x1E293B), 0);
                    lv_obj_set_style_pad_all(b, 0, 0);
                    lv_obj_add_event_cb(b, wifi_ssid_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

                    lv_obj_t *l = lv_label_create(b);
                    lv_label_set_text_fmt(l, LV_SYMBOL_WIFI "  %s", items[i].ssid);
                    lv_obj_set_style_text_font(l, &ui_font_sm, 0);
                    lv_obj_set_style_text_color(l, hex_to_lv(0xF8FAFC), 0);
                    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
                    lv_obj_set_width(l, 178);
                    lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);

                    lv_obj_t *r = lv_label_create(b);
                    lv_label_set_text_fmt(r, "%ld dBm", (long)items[i].rssi);
                    lv_obj_set_style_text_font(r, &ui_font_sm, 0);
                    lv_obj_set_style_text_color(r, hex_to_lv(items[i].rssi >= -68 ? 0x10B981 : 0x94A3B8), 0);
                    lv_obj_align(r, LV_ALIGN_RIGHT_MID, -10, 0);
                }
            }
        }
    }

    bsp_lvgl_unlock();
}

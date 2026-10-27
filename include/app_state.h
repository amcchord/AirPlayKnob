#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "somafm_presets.h"

#define MAX_PRESETS        48
#define MAX_SPEAKERS       32
#define MAX_WIFI_SCAN      20

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID  ""
#endif
#ifndef DEFAULT_WIFI_PASS
#define DEFAULT_WIFI_PASS  ""
#endif

enum KnobMode {
    KNOB_MODE_VOLUME = 0,
    KNOB_MODE_STATION = 1,
    KNOB_MODE_SPEAKER = 2,
};

enum StreamStatus {
    STREAM_STOPPED = 0,
    STREAM_CONNECTING_RADIO = 1,
    STREAM_CONNECTING_AIRPLAY = 2,
    STREAM_PLAYING = 3,
    STREAM_ERROR = 4,
};

struct AirPlaySpeaker {
    char name[48];
    char host[64];
    char ip[20];
    uint16_t port;
    char et[24];
    char cn[24];
    char md[24];
    char am[32];
    bool is_local_dac;
};

struct WifiScanItem {
    char ssid[36];
    int32_t rssi;
    bool secure;
};

struct AppState {
    SemaphoreHandle_t mutex;

    // Presets
    StationPreset presets[MAX_PRESETS];
    int preset_count;
    int current_preset;
    int preview_preset;
    uint32_t preview_timestamp_ms;

    // Speakers
    AirPlaySpeaker speakers[MAX_SPEAKERS];
    int speaker_count;
    int current_speaker;
    int preview_speaker;
    bool also_local_dac;
    bool speaker_scan_requested;
    bool speaker_scanning;

    // Playback & Volume
    bool playing;
    int volume; // 0..100
    bool volume_dirty;
    bool stream_reconfig_needed;
    StreamStatus status;
    char status_text[64];
    char track_title[128];
    char icy_station[64];
    int bitrate_kbps;
    uint8_t vu_bars[12]; // 0..100 for 12 visualizer bars
    uint8_t vu_peak;     // 0..100 overall level
    int8_t wave_l[128];  // Real-time stereo waveform samples (-127..127) for WhiteCap visualizer
    int8_t wave_r[128];
    char artwork_url[256]; // Current live Album Art or SomaFM Stream Art URL

    // WhiteCap Visualizer & OTA status
    bool whitecap_active;
    int whitecap_preset;
    bool ota_in_progress;
    int ota_progress_pct;

    // Knob mode
    KnobMode knob_mode;

    // Wi-Fi
    char wifi_ssid[36];
    char wifi_pass[64];
    bool wifi_connected;
    char wifi_ip[20];
    int32_t wifi_rssi;
    bool wifi_reconnect_requested;
    bool wifi_scan_requested;
    bool wifi_scanning;
    WifiScanItem wifi_scan[MAX_WIFI_SCAN];
    int wifi_scan_count;
};

extern AppState g_app;

void app_state_init(void);
void app_state_save_presets(void);
void app_state_reset_default_presets(void);
void app_state_save_playback(void);
void app_state_save_wifi(const char *ssid, const char *pass);
void app_select_preset(int index, bool auto_play);
void app_select_speaker(int index);
void app_set_volume(int vol);
void app_toggle_play(void);

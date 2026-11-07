#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "somafm_presets.h"

#define MAX_PRESETS        48
#define MAX_SPEAKERS       48
#define MAX_WIFI_SCAN      20
#define AIRPLAYKNOB_BUILD_ID "art-network-20261107-r6"

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
    bool is_ap2;
    bool requires_pin;
    uint32_t last_seen_ms;
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
    bool prefer_sonos_native; // Optional radio optimization; false always uses AirPlay.
    bool speaker_scan_requested;
    bool speaker_scanning;
    bool speaker_scan_deferred;
    bool wifi_scan_deferred;
    uint32_t speaker_scan_count;
    uint32_t speaker_scan_last_ms;

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

    // Live Buffers & Streaming Telemetry
    uint32_t pcm_buf_ms;
    uint32_t pcm_buf_kb;
    uint32_t mp3_buf_bytes;
    uint32_t stream_underruns;
    bool raop_connected;
    bool raop_is_ap2;
    bool sonos_native;
    bool sonos_radio_selected; // Includes native connection/recovery, before PLAYING.
    uint32_t raop_pkts_sent;
    uint32_t raop_ntp_replies;
    uint32_t raop_retransmits;
    uint32_t raop_tx_accepted;
    uint32_t raop_tx_rejected;
    uint32_t raop_tx_retries;
    uint32_t raop_repair_rejected;
    uint32_t raop_repair_pending;
    int raop_tx_last_error;

    // AirPlay 4-Digit PIN Pairing State
    bool pin_prompt_active;
    bool pin_submitted;
    bool pin_cancelled;
    bool pin_manual_request;
    char pin_code[8];
    char pin_speaker_name[48];
    char pin_speaker_ip[20];
    char pin_halted_ip[20];
    bool pin_toast_pending;
    char pin_toast_title[32];
    char pin_toast_msg[48];
    uint32_t pin_toast_color;

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
    int wifi_speed_mbps;
    char wifi_phy_mode[24];
    uint8_t wifi_channel;
    uint32_t wifi_connect_start_ms;
    uint32_t wifi_connected_since_ms;
    bool wifi_reconnect_requested;
    bool wifi_scan_requested;
    bool wifi_scanning;
    bool wifi_modal_open;
    uint32_t wifi_scan_done_seq;
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
void app_submit_airplay_pin(const char *pin);
void app_cancel_airplay_pin(void);
void app_request_airplay_pin_pair(void);
void app_notify_pin_toast(const char *title, const char *msg, uint32_t color);
void app_get_ap2_client_identity(char id_out[37], uint8_t ltpk_out[32], uint8_t ltsk_out[64]);
bool app_is_ap2_speaker_paired(const char *ip);
void app_mark_ap2_speaker_paired(const char *ip);

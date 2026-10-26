#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "app_state.h"

#define RAOP_CHUNK_FRAMES 352
#define RAOP_SAMPLE_RATE  44100

class RaopClient {
public:
    RaopClient();
    ~RaopClient();

    bool connect(const AirPlaySpeaker &spk, int initial_volume_pct);
    void disconnect();
    bool isConnected() const { return m_connected; }
    bool isAirPlay2() const { return m_ap2; }

    // Returns true when the NTP-locked RTP clock is ready to accept the next 352-frame chunk
    bool acceptFrames() const;

    // Sends 352 stereo 16-bit PCM frames (1408 bytes)
    bool sendChunk(const int16_t *stereo_pcm_352);

    bool setVolume(int volume_pct);
    bool setMetadata(const char *title, const char *artist, const char *album);
    void pollControlAndKeepalive();

private:
    bool execRtsp(const char *cmd, const char *uri_override,
                  const char *content_type, const uint8_t *content, int content_len,
                  const char *extra_headers, char *resp_headers_out, int resp_headers_max,
                  uint8_t **resp_body_out, int *resp_body_len_out);

    int rtspSend(const uint8_t *data, int len);
    bool rtspFillPlain(int timeout_ms);

    bool doGetInfo(bool *supports_transient_out);
    bool doAp2PairTransient();
    bool doAp2SetupSession(uint16_t *event_port_out);
    bool doAp2SetupStream();
    void connectEventChannel(uint16_t event_port);
    void pollEventChannel();

    bool doAuthSetup();
    bool doAnnounce(bool use_rsa_aes);
    bool doSetup();
    bool doRecord();
    void sendSyncPacket(bool first);
    void handleControlSocket();

    static void timingTaskTrampoline(void *arg);
    void timingTaskLoop();

    int m_rtsp_sock;
    int m_event_sock;
    int m_audio_sock;
    int m_ctrl_sock;
    int m_time_sock;

    uint16_t m_local_audio_port;
    uint16_t m_local_ctrl_port;
    uint16_t m_local_time_port;

    uint16_t m_remote_audio_port;
    uint16_t m_remote_ctrl_port;
    uint16_t m_remote_time_port;

    uint32_t m_peer_ip_addr;
    uint32_t m_local_ip_addr;
    uint16_t m_rtsp_port;

    char m_rtsp_url[128];
    char m_session[64];
    char m_user_agent[48];
    char m_sci[20];
    char m_dacp_id[20];
    char m_active_remote[16];
    char m_sid[16];
    uint32_t m_sid_int;
    AirPlaySpeaker m_speaker;

    int m_cseq;
    bool m_connected;
    bool m_first_pkt;
    bool m_encrypt_aes;
    uint8_t m_aes_key[16];
    uint8_t m_aes_iv[16];

    // AirPlay 2 state (HomeKit transient pairing + ChaCha20-Poly1305)
    bool m_ap2;
    bool m_rtsp_encrypted;
    uint8_t m_ctrl_write_key[32];
    uint8_t m_ctrl_read_key[32];
    uint64_t m_ctrl_write_counter;
    uint64_t m_ctrl_read_counter;

    uint8_t m_ev_write_key[32];
    uint8_t m_ev_read_key[32];
    uint64_t m_ev_write_counter;
    uint64_t m_ev_read_counter;

    uint8_t m_shk[32]; // Stream audio ChaCha20-Poly1305 key

    uint8_t *m_rx_enc_buf;
    size_t m_rx_enc_len;
    uint8_t *m_rx_plain_buf;
    size_t m_rx_plain_len;

    uint8_t *m_ev_enc_buf;
    size_t m_ev_enc_len;
    uint8_t *m_ev_plain_buf;
    size_t m_ev_plain_len;

    uint16_t m_seq_num;
    uint32_t m_ssrc;
    uint64_t m_head_ts;
    uint32_t m_latency_frames;
    uint64_t m_last_sync_us;
    uint64_t m_last_keepalive_us;
    uint32_t m_pkts_sent;
    uint32_t m_ntp_replies;
    uint32_t m_retransmits;

    volatile bool m_timing_running;
    TaskHandle_t m_timing_task;

    struct BacklogSlot {
        uint16_t seq;
        uint16_t len;
        uint8_t data[1480];
    };
    BacklogSlot *m_backlog; // allocated in PSRAM
};

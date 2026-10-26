#include "raop_AnySpeaker.h"
#include "crypto/curve25519_donna.h"
#include <Arduino.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <esp_timer.h>
#include <esp_random.h>
#include <esp_heap_caps.h>
#include <mbedtls/aes.h>
#include <mbedtls/pk.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/base64.h>

#define BACKLOG_SIZE 256

static const char APPLE_RSA_PUBKEY_PEM[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA59dE8qLieItsH1WgjrcF\n"
    "RKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUtwC5vOYvfDmFI6oSFXi5ELabW\n"
    "JmT2dKHzBJKa3k9ok+8t9ucRqMd6DZHJ2YCCLlDRKSKv6kDqnw4UwPdpOMXziC/A\n"
    "Mj3Z/lUVX1G7WSHCAWKf1zNS1eLvqr+boEjXuBOitnZ/bDzPHrTOZz0Dew0uowxf\n"
    "/+sG+NCK3eQJVxqcaJ/vEHKIVd2M+5qL71yJQ+87X6oV3eaYvt3zWZYD6z5vYTcr\n"
    "tij2VZ9Zmni/UAaHqn9JdsBWLUEpVviYnhimNVvYFZeCXg/IdTQ+x4IRdiXNv5hE\n"
    "ewIDAQAB\n"
    "-----END PUBLIC KEY-----\n";

static uint64_t get_ntp_now(uint32_t *sec_out, uint32_t *frac_out) {
    uint64_t us = (uint64_t)esp_timer_get_time() + 3900000000ULL * 1000000ULL;
    uint32_t sec = (uint32_t)(us / 1000000ULL);
    uint32_t frac = (uint32_t)(((us % 1000000ULL) << 32) / 1000000ULL);
    if (sec_out) *sec_out = sec;
    if (frac_out) *frac_out = frac;
    return ((uint64_t)sec << 32) | frac;
}

static inline uint64_t ntp_to_ts(uint64_t ntp, uint32_t rate) {
    return (((ntp >> 16) * rate) >> 16);
}

static inline uint64_t ts_to_ntp(uint64_t ts, uint32_t rate) {
    return (((ts << 16) / rate) << 16);
}

// Encodes 352 stereo 16-bit little-endian frames into raw uncompressed ALAC packet
static int encode_pcm_to_alac_raw(const int16_t *stereo_pcm, int frames, uint8_t *out) {
    uint8_t *p = out;
    const uint32_t *in = (const uint32_t *)stereo_pcm;
    int bsize = RAOP_CHUNK_FRAMES;
    if (frames > bsize) frames = bsize;

    *p++ = (1 << 5);
    *p++ = 0;
    *p++ = (1 << 4) | (1 << 1) | ((bsize & 0x80000000) >> 31);
    *p++ = ((bsize & 0x7f800000) << 1) >> 24;
    *p++ = ((bsize & 0x007f8000) << 1) >> 16;
    *p++ = ((bsize & 0x00007f80) << 1) >> 8;
    *p   = ((bsize & 0x0000007f) << 1);
    *p++ |= (*in & 0x00008000) >> 15;

    int count = frames - 1;
    while (count-- > 0) {
        *p++ = ((*in & 0x00007f80) >> 7);
        *p++ = ((*in & 0x0000007f) << 1) | ((*in & 0x80000000) >> 31);
        *p++ = ((*in & 0x7f800000) >> 23);
        *p++ = ((*in & 0x007f0000) >> 15) | ((*(in + 1) & 0x00008000) >> 15);
        in++;
    }
    *p++ = ((*in & 0x00007f80) >> 7);
    *p++ = ((*in & 0x0000007f) << 1) | ((*in & 0x80000000) >> 31);
    *p++ = ((*in & 0x7f800000) >> 23);
    *p++ = ((*in & 0x007f0000) >> 15);

    count = (bsize - frames) * 4;
    while (count-- > 0) *p++ = 0;

    *(p - 1) |= 1;
    *p = (7 >> 1) << 6;
    return (int)(p - out + 1);
}

RaopClient::RaopClient()
    : m_rtsp_sock(-1),
      m_audio_sock(-1),
      m_ctrl_sock(-1),
      m_time_sock(-1),
      m_local_audio_port(0),
      m_local_ctrl_port(0),
      m_local_time_port(0),
      m_remote_audio_port(0),
      m_remote_ctrl_port(0),
      m_remote_time_port(0),
      m_peer_ip_addr(0),
      m_local_ip_addr(0),
      m_rtsp_port(7000),
      m_cseq(0),
      m_connected(false),
      m_first_pkt(true),
      m_encrypt_aes(false),
      m_seq_num(0),
      m_ssrc(0),
      m_head_ts(0),
      m_latency_frames(44100),
      m_last_sync_us(0),
      m_last_keepalive_us(0),
      m_timing_running(false),
      m_timing_task(NULL),
      m_backlog(NULL) {
    memset(m_session, 0, sizeof(m_session));
    m_backlog = (BacklogSlot *)heap_caps_calloc(BACKLOG_SIZE, sizeof(BacklogSlot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

RaopClient::~RaopClient() {
    disconnect();
    if (m_backlog) {
        heap_caps_free(m_backlog);
        m_backlog = NULL;
    }
}

static int bind_udp_ephemeral(uint16_t *port_out) {
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return -1;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in sin = {};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = 0;
    if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        close(s);
        return -1;
    }
    socklen_t slen = sizeof(sin);
    getsockname(s, (struct sockaddr *)&sin, &slen);
    *port_out = ntohs(sin.sin_port);
    return s;
}

bool RaopClient::connect(const AirPlaySpeaker &spk, int initial_volume_pct) {
    disconnect();
    m_speaker = spk;
    m_cseq = 0;
    m_session[0] = '\0';
    m_rtsp_port = spk.port ? spk.port : 7000;

    m_peer_ip_addr = inet_addr(spk.ip);
    if (m_peer_ip_addr == INADDR_NONE || m_peer_ip_addr == 0) {
        struct hostent *he = gethostbyname(spk.host);
        if (!he || !he->h_addr_list[0]) {
            Serial.printf("[RAOP] Failed to resolve host %s\n", spk.host);
            return false;
        }
        memcpy(&m_peer_ip_addr, he->h_addr_list[0], 4);
    }

    m_rtsp_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_rtsp_sock < 0) return false;

    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(m_rtsp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(m_rtsp_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in peer_in = {};
    peer_in.sin_family = AF_INET;
    peer_in.sin_addr.s_addr = m_peer_ip_addr;
    peer_in.sin_port = htons(m_rtsp_port);

    Serial.printf("[RAOP] Connecting RTSP to %s (%s:%u) am=%s et=%s\n",
                  spk.name, inet_ntoa(peer_in.sin_addr), m_rtsp_port, spk.am, spk.et);

    if (::connect(m_rtsp_sock, (struct sockaddr *)&peer_in, sizeof(peer_in)) < 0) {
        Serial.printf("[RAOP] TCP connect failed to %s:%u\n", spk.ip, m_rtsp_port);
        close(m_rtsp_sock);
        m_rtsp_sock = -1;
        return false;
    }

    struct sockaddr_in local_in = {};
    socklen_t local_len = sizeof(local_in);
    getsockname(m_rtsp_sock, (struct sockaddr *)&local_in, &local_len);
    m_local_ip_addr = local_in.sin_addr.s_addr;

    uint32_t r1 = esp_random(), r2 = esp_random(), r3 = esp_random();
    m_ssrc = esp_random();
    m_seq_num = (uint16_t)(esp_random() & 0x7FFF);
    snprintf(m_sid, sizeof(m_sid), "%010lu", (unsigned long)(r1 % 1000000000UL + 100000000UL));
    snprintf(m_sci, sizeof(m_sci), "%08lX%08lX", (unsigned long)r2, (unsigned long)r3);
    snprintf(m_dacp_id, sizeof(m_dacp_id), "%08lX%08lX", (unsigned long)r3, (unsigned long)r2);
    snprintf(m_active_remote, sizeof(m_active_remote), "%010lu", (unsigned long)(r2 % 1000000000UL + 100000000UL));

    struct in_addr laddr = {.s_addr = m_local_ip_addr};
    snprintf(m_rtsp_url, sizeof(m_rtsp_url), "rtsp://%s/%s", inet_ntoa(laddr), m_sid);

    if (strcasestr(spk.am, "AudioAccessory") || strcasestr(spk.am, "AppleTV") || strcasestr(spk.am, "HomePod")) {
        strncpy(m_user_agent, "AirPlay/999.0.0", sizeof(m_user_agent));
    } else {
        strncpy(m_user_agent, "iTunes/7.6.2 (Windows; N;)", sizeof(m_user_agent));
    }

    // Open UDP sockets for timing, control, and audio before ANNOUNCE/SETUP
    m_time_sock = bind_udp_ephemeral(&m_local_time_port);
    m_ctrl_sock = bind_udp_ephemeral(&m_local_ctrl_port);
    m_audio_sock = bind_udp_ephemeral(&m_local_audio_port);
    if (m_time_sock < 0 || m_ctrl_sock < 0 || m_audio_sock < 0) {
        Serial.println("[RAOP] Failed to bind local UDP ports");
        disconnect();
        return false;
    }

    int sndbuf = 65536;
    setsockopt(m_audio_sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    m_remote_time_port = 0;
    m_timing_running = true;
    xTaskCreatePinnedToCore(timingTaskTrampoline, "raop_ntp", 4096, this, 5, &m_timing_task, 0);

    // MFi / Sonos auth-setup if et contains '4'
    if (strchr(spk.et, '4')) {
        doAuthSetup();
    }

    // Determine whether speaker needs RSA+AES (et=1 without et=0)
    bool prefer_rsa = (strchr(spk.et, '1') != NULL && strchr(spk.et, '0') == NULL);
    if (!doAnnounce(prefer_rsa)) {
        if (!prefer_rsa && strchr(spk.et, '1')) {
            Serial.println("[RAOP] Retrying ANNOUNCE with RSA-AES encryption...");
            if (!doAnnounce(true)) {
                disconnect();
                return false;
            }
        } else {
            disconnect();
            return false;
        }
    }

    if (!doSetup()) {
        disconnect();
        return false;
    }

    if (!doRecord()) {
        disconnect();
        return false;
    }

    m_head_ts = ntp_to_ts(get_ntp_now(NULL, NULL), RAOP_SAMPLE_RATE);
    m_first_pkt = true;
    m_connected = true;
    m_last_sync_us = esp_timer_get_time();
    m_last_keepalive_us = esp_timer_get_time();

    setVolume(initial_volume_pct);
    Serial.printf("[RAOP] Connected and streaming to %s (audio=%u ctrl=%u time=%u)\n",
                  spk.name, m_remote_audio_port, m_remote_ctrl_port, m_remote_time_port);
    return true;
}

void RaopClient::disconnect() {
    m_connected = false;
    if (m_timing_running) {
        m_timing_running = false;
        vTaskDelay(pdMS_TO_TICKS(150));
        m_timing_task = NULL;
    }
    if (m_rtsp_sock >= 0) {
        if (m_session[0]) {
            char hdr[64];
            snprintf(hdr, sizeof(hdr), "RTP-Info: seq=%u;rtptime=%u\r\n",
                     (unsigned)(m_seq_num + 1), (unsigned)(m_head_ts + 1));
            execRtsp("FLUSH", NULL, NULL, NULL, 0, hdr, NULL, 0, NULL, NULL);
            execRtsp("TEARDOWN", NULL, NULL, NULL, 0, NULL, NULL, 0, NULL, NULL);
        }
        close(m_rtsp_sock);
        m_rtsp_sock = -1;
    }
    if (m_audio_sock >= 0) { close(m_audio_sock); m_audio_sock = -1; }
    if (m_ctrl_sock >= 0)  { close(m_ctrl_sock);  m_ctrl_sock = -1; }
    if (m_time_sock >= 0)  { close(m_time_sock);  m_time_sock = -1; }
    m_session[0] = '\0';
}

bool RaopClient::execRtsp(const char *cmd, const char *uri_override,
                          const char *content_type, const uint8_t *content, int content_len,
                          const char *extra_headers, char *resp_headers_out, int resp_headers_max,
                          uint8_t *resp_body_out, int *resp_body_len_inout) {
    if (m_rtsp_sock < 0) return false;

    int req_cap = 1024 + content_len;
    uint8_t *req = (uint8_t *)malloc(req_cap);
    if (!req) return false;

    int hdr_len = snprintf((char *)req, 1024,
        "%s %s RTSP/1.0\r\n"
        "CSeq: %d\r\n"
        "User-Agent: %s\r\n"
        "Client-Instance: %s\r\n"
        "DACP-ID: %s\r\n"
        "Active-Remote: %s\r\n",
        cmd, uri_override ? uri_override : m_rtsp_url,
        ++m_cseq, m_user_agent, m_sci, m_dacp_id, m_active_remote);

    if (m_session[0]) {
        hdr_len += snprintf((char *)req + hdr_len, 1024 - hdr_len, "Session: %s\r\n", m_session);
    }
    if (extra_headers && extra_headers[0]) {
        hdr_len += snprintf((char *)req + hdr_len, 1024 - hdr_len, "%s", extra_headers);
    }
    if (content_type && content && content_len > 0) {
        hdr_len += snprintf((char *)req + hdr_len, 1024 - hdr_len,
            "Content-Type: %s\r\nContent-Length: %d\r\n", content_type, content_len);
    }
    hdr_len += snprintf((char *)req + hdr_len, 1024 - hdr_len, "\r\n");

    if (content && content_len > 0) {
        memcpy(req + hdr_len, content, content_len);
    }

    int total_send = hdr_len + content_len;
    int sent = send(m_rtsp_sock, req, total_send, 0);
    free(req);
    if (sent != total_send) {
        Serial.printf("[RAOP] RTSP send failed (%s)\n", cmd);
        return false;
    }

    char rbuf[2048];
    int rlen = 0;
    int hdr_end = -1;
    while (rlen < (int)sizeof(rbuf) - 1) {
        int n = recv(m_rtsp_sock, rbuf + rlen, sizeof(rbuf) - 1 - rlen, 0);
        if (n <= 0) break;
        rlen += n;
        rbuf[rlen] = '\0';
        char *p = strstr(rbuf, "\r\n\r\n");
        if (p) {
            hdr_end = (int)(p - rbuf) + 4;
            break;
        }
    }
    if (hdr_end < 0) {
        Serial.printf("[RAOP] RTSP %s: no header terminator (rlen=%d)\n", cmd, rlen);
        return false;
    }

    rbuf[hdr_end - 2] = '\0';
    int status_code = 0;
    sscanf(rbuf, "RTSP/%*s %d", &status_code);
    if (status_code != 200) {
        Serial.printf("[RAOP] RTSP %s returned status %d:\n%s\n", cmd, status_code, rbuf);
    }

    if (resp_headers_out && resp_headers_max > 0) {
        strncpy(resp_headers_out, rbuf, resp_headers_max - 1);
        resp_headers_out[resp_headers_max - 1] = '\0';
    }

    int clen = 0;
    char *cl_ptr = strcasestr(rbuf, "Content-Length:");
    if (cl_ptr) {
        sscanf(cl_ptr + 15, "%d", &clen);
    }
    int already_body = rlen - hdr_end;
    if (clen > 0) {
        uint8_t *b = (uint8_t *)malloc(clen + 1);
        if (b) {
            int copied = (already_body > clen) ? clen : already_body;
            if (copied > 0) memcpy(b, rbuf + hdr_end, copied);
            while (copied < clen) {
                int n = recv(m_rtsp_sock, b + copied, clen - copied, 0);
                if (n <= 0) break;
                copied += n;
            }
            if (resp_body_out && resp_body_len_inout && *resp_body_len_inout >= clen) {
                memcpy(resp_body_out, b, clen);
                *resp_body_len_inout = clen;
            }
            free(b);
        }
    }
    return status_code == 200;
}

bool RaopClient::doAuthSetup() {
    uint8_t secret[32];
    esp_fill_random(secret, sizeof(secret));
    secret[0] &= 248;
    secret[31] &= 127;
    secret[31] |= 64;

    uint8_t basepoint[32] = {9};
    uint8_t payload[33];
    payload[0] = 0x01;
    curve25519_donna(payload + 1, secret, basepoint);

    bool ok = execRtsp("POST", "/auth-setup", "application/octet-stream", payload, 33, NULL, NULL, 0, NULL, NULL);
    Serial.printf("[RAOP] /auth-setup: %s\n", ok ? "OK" : "FAILED (continuing)");
    return ok;
}

bool RaopClient::doAnnounce(bool use_rsa_aes) {
    struct in_addr laddr = {.s_addr = m_local_ip_addr};
    struct in_addr paddr = {.s_addr = m_peer_ip_addr};
    char local_str[20], peer_str[20];
    strncpy(local_str, inet_ntoa(laddr), sizeof(local_str));
    strncpy(peer_str, inet_ntoa(paddr), sizeof(peer_str));

    char sdp[1024];
    int len = snprintf(sdp, sizeof(sdp),
        "v=0\r\n"
        "o=iTunes %s 0 IN IP4 %s\r\n"
        "s=iTunes\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "m=audio 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 AppleLossless\r\n"
        "a=fmtp:96 %d 0 16 40 10 14 2 255 0 0 %d\r\n",
        m_sid, local_str, peer_str, RAOP_CHUNK_FRAMES, RAOP_SAMPLE_RATE);

    m_encrypt_aes = false;
    if (use_rsa_aes) {
        esp_fill_random(m_aes_key, 16);
        esp_fill_random(m_aes_iv, 16);

        mbedtls_pk_context pk;
        mbedtls_entropy_context entropy;
        mbedtls_ctr_drbg_context ctr_drbg;
        mbedtls_pk_init(&pk);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&ctr_drbg);

        const char *pers = "raop_rsa";
        mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const uint8_t *)pers, strlen(pers));
        if (mbedtls_pk_parse_public_key(&pk, (const uint8_t *)APPLE_RSA_PUBKEY_PEM, sizeof(APPLE_RSA_PUBKEY_PEM)) == 0) {
            mbedtls_rsa_context *rsa = mbedtls_pk_rsa(pk);
            mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
            uint8_t enc_key[256];
            size_t olen = 0;
            if (mbedtls_rsa_rsaes_oaep_encrypt(rsa, mbedtls_ctr_drbg_random, &ctr_drbg,
                                               NULL, 0, 16, m_aes_key, enc_key) == 0) {
                olen = 256;
                char b64_key[400] = {0}, b64_iv[64] = {0};
                size_t klen = 0, ivlen = 0;
                mbedtls_base64_encode((uint8_t *)b64_key, sizeof(b64_key), &klen, enc_key, olen);
                mbedtls_base64_encode((uint8_t *)b64_iv, sizeof(b64_iv), &ivlen, m_aes_iv, 16);
                while (klen > 0 && b64_key[klen - 1] == '=') b64_key[--klen] = '\0';
                while (ivlen > 0 && b64_iv[ivlen - 1] == '=') b64_iv[--ivlen] = '\0';
                len += snprintf(sdp + len, sizeof(sdp) - len,
                    "a=rsaaeskey:%s\r\na=aesiv:%s\r\n", b64_key, b64_iv);
                m_encrypt_aes = true;
            }
        }
        mbedtls_pk_free(&pk);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
    }

    return execRtsp("ANNOUNCE", NULL, "application/sdp", (const uint8_t *)sdp, len, NULL, NULL, 0, NULL, NULL);
}

bool RaopClient::doSetup() {
    char transport_hdr[160];
    snprintf(transport_hdr, sizeof(transport_hdr),
        "Transport: RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;control_port=%u;timing_port=%u\r\n",
        m_local_ctrl_port, m_local_time_port);

    char resp_hdrs[1024] = {0};
    if (!execRtsp("SETUP", NULL, NULL, NULL, 0, transport_hdr, resp_hdrs, sizeof(resp_hdrs), NULL, NULL)) {
        return false;
    }

    // Parse Session
    char *sess = strcasestr(resp_hdrs, "Session:");
    if (sess) {
        sess += 8;
        while (*sess == ' ') sess++;
        int i = 0;
        while (*sess && *sess != '\r' && *sess != '\n' && *sess != ';' && i < (int)sizeof(m_session) - 1) {
            m_session[i++] = *sess++;
        }
        m_session[i] = '\0';
    }

    // Parse Transport ports
    m_remote_audio_port = 0;
    m_remote_ctrl_port = 0;
    m_remote_time_port = 0;
    char *tp = strcasestr(resp_hdrs, "Transport:");
    if (tp) {
        char *sp = strcasestr(tp, "server_port=");
        if (sp) m_remote_audio_port = (uint16_t)atoi(sp + 12);
        char *cp = strcasestr(tp, "control_port=");
        if (cp) m_remote_ctrl_port = (uint16_t)atoi(cp + 13);
        char *tmp = strcasestr(tp, "timing_port=");
        if (tmp) m_remote_time_port = (uint16_t)atoi(tmp + 12);
    }

    return (m_session[0] != '\0' && m_remote_audio_port != 0);
}

bool RaopClient::doRecord() {
    uint64_t now_ts = ntp_to_ts(get_ntp_now(NULL, NULL), RAOP_SAMPLE_RATE);
    char hdr[160];
    snprintf(hdr, sizeof(hdr),
        "Range: npt=0-\r\n"
        "RTP-Info: seq=%u;rtptime=%u\r\n",
        (unsigned)(m_seq_num + 1), (unsigned)now_ts);

    char resp_hdrs[1024] = {0};
    if (!execRtsp("RECORD", NULL, NULL, NULL, 0, hdr, resp_hdrs, sizeof(resp_hdrs), NULL, NULL)) {
        return false;
    }
    char *lat = strcasestr(resp_hdrs, "Audio-Latency:");
    if (lat) {
        int l = atoi(lat + 14);
        if (l > 11025 && l < 220500) m_latency_frames = (uint32_t)l;
    }
    return true;
}

bool RaopClient::setVolume(int volume_pct) {
    if (!m_connected || m_rtsp_sock < 0) return false;
    if (volume_pct < 0) volume_pct = 0;
    if (volume_pct > 100) volume_pct = 100;
    float db = (volume_pct == 0) ? -144.0f : (-30.0f + (30.0f * (float)volume_pct) / 100.0f);
    char body[64];
    int blen = snprintf(body, sizeof(body), "volume: %.6f\r\n", db);
    return execRtsp("SET_PARAMETER", NULL, "text/parameters", (const uint8_t *)body, blen, NULL, NULL, 0, NULL, NULL);
}

static int write_dmap_str(uint8_t *dst, const char *tag, const char *str) {
    int slen = str ? (int)strlen(str) : 0;
    memcpy(dst, tag, 4);
    dst[4] = (slen >> 24) & 0xFF;
    dst[5] = (slen >> 16) & 0xFF;
    dst[6] = (slen >> 8) & 0xFF;
    dst[7] = slen & 0xFF;
    if (slen > 0) memcpy(dst + 8, str, slen);
    return 8 + slen;
}

bool RaopClient::setMetadata(const char *title, const char *artist, const char *album) {
    if (!m_connected || m_rtsp_sock < 0 || !strchr(m_speaker.md, '0')) return false;
    uint8_t dmap[512];
    int pos = 8; // mlit header
    memcpy(dmap + pos, "mikd\x00\x00\x00\x01\x02", 9);
    pos += 9;
    pos += write_dmap_str(dmap + pos, "minm", title ? title : "");
    pos += write_dmap_str(dmap + pos, "asar", artist ? artist : "");
    pos += write_dmap_str(dmap + pos, "asal", album ? album : "SomaFM");
    int inner_len = pos - 8;
    memcpy(dmap, "mlit", 4);
    dmap[4] = (inner_len >> 24) & 0xFF;
    dmap[5] = (inner_len >> 16) & 0xFF;
    dmap[6] = (inner_len >> 8) & 0xFF;
    dmap[7] = inner_len & 0xFF;

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "RTP-Info: rtptime=%u\r\n", (unsigned)(m_head_ts + m_latency_frames));
    return execRtsp("SET_PARAMETER", NULL, "application/x-dmap-tagged", dmap, pos, hdr, NULL, 0, NULL, NULL);
}

void RaopClient::sendSyncPacket(bool first) {
    if (!m_connected || m_ctrl_sock < 0 || m_remote_ctrl_port == 0) return;

    struct __attribute__((packed)) {
        uint8_t proto;
        uint8_t type;
        uint16_t seq;
        uint32_t rtp_ts_latency;
        uint32_t ntp_sec;
        uint32_t ntp_frac;
        uint32_t rtp_ts;
    } pkt;

    uint64_t now_ntp = ts_to_ntp(m_head_ts, RAOP_SAMPLE_RATE);
    pkt.proto = 0x80 | (first ? 0x10 : 0x00);
    pkt.type = 0x54 | 0x80;
    pkt.seq = htons(7);
    pkt.rtp_ts_latency = htonl((uint32_t)(m_head_ts - m_latency_frames));
    pkt.ntp_sec = htonl((uint32_t)(now_ntp >> 32));
    pkt.ntp_frac = htonl((uint32_t)(now_ntp & 0xFFFFFFFFULL));
    pkt.rtp_ts = htonl((uint32_t)m_head_ts);

    struct sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = m_peer_ip_addr;
    dst.sin_port = htons(m_remote_ctrl_port);
    sendto(m_ctrl_sock, &pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst));
}

bool RaopClient::sendChunk(const int16_t *stereo_pcm_352) {
    if (!m_connected || m_audio_sock < 0) return false;

    if (m_first_pkt) {
        m_head_ts = ntp_to_ts(get_ntp_now(NULL, NULL), RAOP_SAMPLE_RATE);
        sendSyncPacket(true);
        m_last_sync_us = esp_timer_get_time();
    }

    uint8_t alac_buf[1424];
    int alac_len = encode_pcm_to_alac_raw(stereo_pcm_352, RAOP_CHUNK_FRAMES, alac_buf);

    if (m_encrypt_aes) {
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        mbedtls_aes_setkey_enc(&aes, m_aes_key, 128);
        uint8_t iv[16];
        memcpy(iv, m_aes_iv, 16);
        int blocks_len = (alac_len / 16) * 16;
        if (blocks_len > 0) {
            mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, blocks_len, iv, alac_buf, alac_buf);
        }
        mbedtls_aes_free(&aes);
    }

    m_seq_num++;
    uint8_t rtp_pkt[1440];
    rtp_pkt[0] = 0x80;
    rtp_pkt[1] = 0x60 | (m_first_pkt ? 0x80 : 0x00);
    m_first_pkt = false;
    rtp_pkt[2] = (m_seq_num >> 8) & 0xFF;
    rtp_pkt[3] = m_seq_num & 0xFF;
    uint32_t ts_net = htonl((uint32_t)m_head_ts);
    uint32_t ssrc_net = htonl(m_ssrc);
    memcpy(rtp_pkt + 4, &ts_net, 4);
    memcpy(rtp_pkt + 8, &ssrc_net, 4);
    memcpy(rtp_pkt + 12, alac_buf, alac_len);
    int total_len = 12 + alac_len;

    if (m_backlog) {
        BacklogSlot &slot = m_backlog[m_seq_num % BACKLOG_SIZE];
        slot.seq = m_seq_num;
        slot.len = (uint16_t)total_len;
        memcpy(slot.data, rtp_pkt, total_len);
    }

    struct sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = m_peer_ip_addr;
    dst.sin_port = htons(m_remote_audio_port);
    sendto(m_audio_sock, rtp_pkt, total_len, 0, (struct sockaddr *)&dst, sizeof(dst));

    m_head_ts += RAOP_CHUNK_FRAMES;

    uint64_t now_us = esp_timer_get_time();
    if (now_us - m_last_sync_us >= 1000000ULL) {
        sendSyncPacket(false);
        m_last_sync_us = now_us;
    }
    return true;
}

void RaopClient::handleControlSocket() {
    if (!m_connected || m_ctrl_sock < 0) return;

    struct __attribute__((packed)) {
        uint8_t proto;
        uint8_t type;
        uint16_t seq;
        uint16_t first_seq;
        uint16_t count;
    } req;

    while (true) {
        int n = recv(m_ctrl_sock, &req, sizeof(req), MSG_DONTWAIT);
        if (n < (int)sizeof(req)) break;
        if ((req.type & 0x7F) == 0x55 && m_backlog) {
            uint16_t first = ntohs(req.first_seq);
            uint16_t cnt = ntohs(req.count);
            if (cnt > 32) cnt = 32;
            struct sockaddr_in dst = {};
            dst.sin_family = AF_INET;
            dst.sin_addr.s_addr = m_peer_ip_addr;
            dst.sin_port = htons(m_remote_ctrl_port);

            for (uint16_t i = 0; i < cnt; i++) {
                uint16_t target = first + i;
                BacklogSlot &slot = m_backlog[target % BACKLOG_SIZE];
                if (slot.seq == target && slot.len > 0) {
                    uint8_t retr[1444];
                    retr[0] = 0x80;
                    retr[1] = 0x56 | 0x80;
                    retr[2] = 0x00;
                    retr[3] = 0x01;
                    memcpy(retr + 4, slot.data, slot.len);
                    sendto(m_ctrl_sock, retr, 4 + slot.len, 0, (struct sockaddr *)&dst, sizeof(dst));
                }
            }
        }
    }
}

void RaopClient::pollControlAndKeepalive() {
    if (!m_connected) return;
    handleControlSocket();
    uint64_t now_us = esp_timer_get_time();
    if (now_us - m_last_keepalive_us >= 22000000ULL) {
        m_last_keepalive_us = now_us;
        execRtsp("OPTIONS", "*", NULL, NULL, 0, NULL, NULL, 0, NULL, NULL);
    }
}

void RaopClient::timingTaskTrampoline(void *arg) {
    ((RaopClient *)arg)->timingTaskLoop();
    vTaskDelete(NULL);
}

void RaopClient::timingTaskLoop() {
    struct __attribute__((packed)) {
        uint8_t proto;
        uint8_t type;
        uint16_t seq;
        uint32_t dummy;
        uint32_t ref_sec;
        uint32_t ref_frac;
        uint32_t recv_sec;
        uint32_t recv_frac;
        uint32_t send_sec;
        uint32_t send_frac;
    } req, rsp;

    while (m_timing_running && m_time_sock >= 0) {
        struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(m_time_sock, &rfds);
        int r = select(m_time_sock + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0 || !FD_ISSET(m_time_sock, &rfds)) continue;

        struct sockaddr_in client_in = {};
        socklen_t clen = sizeof(client_in);
        int n = recvfrom(m_time_sock, &req, sizeof(req), 0, (struct sockaddr *)&client_in, &clen);
        if (n >= (int)sizeof(req)) {
            uint32_t sec = 0, frac = 0;
            get_ntp_now(&sec, &frac);
            rsp = req;
            rsp.type = 0x53 | 0x80;
            rsp.ref_sec = req.send_sec;
            rsp.ref_frac = req.send_frac;
            rsp.recv_sec = htonl(sec);
            rsp.recv_frac = htonl(frac);
            rsp.send_sec = htonl(sec);
            rsp.send_frac = htonl(frac);
            sendto(m_time_sock, &rsp, sizeof(rsp), 0, (struct sockaddr *)&client_in, clen);
        }
    }
}

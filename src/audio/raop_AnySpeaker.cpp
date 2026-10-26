#include "raop_AnySpeaker.h"
#include "plist_bin.h"
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
#include <mbedtls/bignum.h>
#include <mbedtls/sha512.h>
#include <mbedtls/md.h>
#include <mbedtls/hkdf.h>
#include <sodium.h>

#define BACKLOG_SIZE 256
#define BPLIST_CONTENT_TYPE "application/x-apple-binary-plist"
#define AP2_BLOCK_MAX 0x400
#define AP2_TAG_SIZE  16
#define AP2_NONCE_SIZE 8

// RFC 5054 3072-bit SRP-6a prime N and generator g=5
static const char SRP_N_3072_HEX[] =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B"
    "139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485"
    "B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1F"
    "E649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F83655D23"
    "DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32"
    "905E462E36CE3BE39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF69558"
    "17183995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33A85521"
    "ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7ABF5AE8CDB0933D7"
    "1E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D87602733EC86A64521F2B1817"
    "7B200CBBE117577A615D6C770988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82"
    "D120A93AD2CAFFFFFFFFFFFFFFFF";

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
    // Standard NTP epoch offset (0x83aa7e80 = 2208988800s) + monotonic clock
    uint64_t us = (uint64_t)esp_timer_get_time() + 3910000000ULL * 1000000ULL;
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

// --- SRP-6a + HKDF + ChaCha20-Poly1305 Helpers for AirPlay 2 ---

static void sha512_mpi_update(mbedtls_sha512_context *ctx, const mbedtls_mpi *n) {
    size_t len = mbedtls_mpi_size(n);
    if (len == 0) return;
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) return;
    mbedtls_mpi_write_binary(n, buf, len);
    mbedtls_sha512_update(ctx, buf, len);
    free(buf);
}

static void sha512_mpi_hash(const mbedtls_mpi *n, uint8_t out64[64]) {
    size_t len = mbedtls_mpi_size(n);
    uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
    if (len) mbedtls_mpi_write_binary(n, buf, len);
    mbedtls_sha512(buf, len, out64, 0);
    free(buf);
}

static void srp_h_nn_pad(mbedtls_mpi *out, const mbedtls_mpi *n1, const mbedtls_mpi *n2, size_t pad_len) {
    uint8_t *bin = (uint8_t *)calloc(2 * pad_len, 1);
    size_t l1 = mbedtls_mpi_size(n1);
    size_t l2 = mbedtls_mpi_size(n2);
    if (l1 <= pad_len && l1 > 0) mbedtls_mpi_write_binary(n1, bin + (pad_len - l1), l1);
    if (l2 <= pad_len && l2 > 0) mbedtls_mpi_write_binary(n2, bin + (2 * pad_len - l2), l2);
    uint8_t digest[64];
    mbedtls_sha512(bin, 2 * pad_len, digest, 0);
    free(bin);
    mbedtls_mpi_read_binary(out, digest, 64);
}

static void srp_calc_x(mbedtls_mpi *x_out, const mbedtls_mpi *salt, const char *user, const char *pin) {
    uint8_t ucp[64];
    mbedtls_sha512_context ctx;
    mbedtls_sha512_init(&ctx);
    mbedtls_sha512_starts(&ctx, 0);
    mbedtls_sha512_update(&ctx, (const uint8_t *)user, strlen(user));
    mbedtls_sha512_update(&ctx, (const uint8_t *)":", 1);
    mbedtls_sha512_update(&ctx, (const uint8_t *)pin, strlen(pin));
    mbedtls_sha512_finish(&ctx, ucp);

    uint8_t x_hash[64];
    mbedtls_sha512_starts(&ctx, 0);
    sha512_mpi_update(&ctx, salt);
    mbedtls_sha512_update(&ctx, ucp, 64);
    mbedtls_sha512_finish(&ctx, x_hash);
    mbedtls_sha512_free(&ctx);

    mbedtls_mpi_read_binary(x_out, x_hash, 64);
}

static bool hkdf_sha512(uint8_t *okm, size_t okm_len, const uint8_t *ikm, size_t ikm_len,
                        const char *salt, const char *info) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    if (!md) return false;
    return mbedtls_hkdf(md,
                        (const uint8_t *)salt, strlen(salt),
                        ikm, ikm_len,
                        (const uint8_t *)info, strlen(info),
                        okm, okm_len) == 0;
}

static bool hap_chacha_encrypt(uint8_t **out_buf, size_t *out_len,
                               const uint8_t *plain, size_t plain_len,
                               const uint8_t key[32], uint64_t *counter) {
    if (!plain || plain_len == 0) return false;
    size_t nblocks = 1 + ((plain_len - 1) / AP2_BLOCK_MAX);
    *out_len = nblocks * (2 + AP2_TAG_SIZE) + plain_len;
    *out_buf = (uint8_t *)malloc(*out_len);
    if (!*out_buf) return false;

    const uint8_t *p = plain;
    uint8_t *c = *out_buf;
    for (size_t i = 0; i < nblocks; i++) {
        uint16_t blen = (i + 1 == nblocks) ? (uint16_t)(plain + plain_len - p) : AP2_BLOCK_MAX;
        uint8_t aad[2] = {(uint8_t)(blen & 0xFF), (uint8_t)((blen >> 8) & 0xFF)};
        uint8_t nonce[12] = {0};
        uint64_t cnt = (*counter)++;
        for (int b = 0; b < 8; b++) nonce[4 + b] = (uint8_t)(cnt >> (8 * b));

        c[0] = aad[0];
        c[1] = aad[1];
        uint8_t tag[AP2_TAG_SIZE];
        unsigned long long maclen = 0;
        if (crypto_aead_chacha20poly1305_ietf_encrypt_detached(
                c + 2, tag, &maclen, p, blen, aad, 2, NULL, nonce, key) != 0) {
            free(*out_buf);
            *out_buf = NULL;
            return false;
        }
        memcpy(c + 2 + blen, tag, AP2_TAG_SIZE);
        p += blen;
        c += 2 + blen + AP2_TAG_SIZE;
    }

    return true;
}

static ssize_t hap_chacha_decrypt(uint8_t **plain_out, size_t *plain_len_out,
                                  const uint8_t *cipher, size_t cipher_len,
                                  const uint8_t key[32], uint64_t *counter) {
    *plain_out = NULL;
    *plain_len_out = 0;
    if (!cipher || cipher_len < 2) return 0;

    uint8_t *plain = (uint8_t *)malloc(cipher_len + 1);
    if (!plain) return -1;

    const uint8_t *c = cipher;
    uint8_t *p = plain;
    uint64_t saved_counter = *counter;

    while ((size_t)(c - cipher) + 2 <= cipher_len) {
        uint16_t blen = (uint16_t)c[0] | ((uint16_t)c[1] << 8);
        if ((size_t)(c - cipher) + 2 + blen + AP2_TAG_SIZE > cipher_len) {
            break; // wait for rest of block
        }
        uint8_t aad[2] = {c[0], c[1]};
        const uint8_t *tag = c + 2 + blen;
        uint8_t nonce[12] = {0};
        uint64_t cnt = (*counter)++;
        for (int b = 0; b < 8; b++) nonce[4 + b] = (uint8_t)(cnt >> (8 * b));

        if (crypto_aead_chacha20poly1305_ietf_decrypt_detached(
                p, NULL, c + 2, blen, tag, aad, 2, nonce, key) != 0) {
            *counter = saved_counter;
            free(plain);
            return -1;
        }
        p += blen;
        c += 2 + blen + AP2_TAG_SIZE;
    }

    *plain_len_out = (size_t)(p - plain);
    plain[*plain_len_out] = '\0';
    *plain_out = plain;
    return (ssize_t)(c - cipher);
}

// Extract a potentially multi-chunk TLV value from a HomeKit TLV8 buffer
static bool tlv8_extract(const uint8_t *buf, int len, uint8_t target_type, uint8_t *out, int max_out, int *out_len) {
    *out_len = 0;
    int i = 0;
    bool found = false;
    while (i + 2 <= len) {
        uint8_t t = buf[i];
        uint8_t l = buf[i + 1];
        if (i + 2 + l > len) break;
        if (t == target_type) {
            found = true;
            if (*out_len + l <= max_out) {
                memcpy(out + *out_len, buf + i + 2, l);
                *out_len += l;
            }
        } else if (found) {
            break;
        }
        i += 2 + l;
    }
    return found;
}

static int tlv8_append(uint8_t *dst, uint8_t type, const uint8_t *val, int len) {
    int pos = 0;
    int rem = len;
    const uint8_t *p = val;
    if (rem == 0) {
        dst[0] = type;
        dst[1] = 0;
        return 2;
    }
    while (rem > 0) {
        int chunk = (rem > 255) ? 255 : rem;
        dst[pos++] = type;
        dst[pos++] = (uint8_t)chunk;
        memcpy(dst + pos, p, chunk);
        pos += chunk;
        p += chunk;
        rem -= chunk;
    }
    return pos;
}

static bool buf_append(uint8_t **buf, size_t *len, const uint8_t *data, size_t n) {
    uint8_t *tmp = (uint8_t *)realloc(*buf, *len + n + 1);
    if (!tmp) return false;
    memcpy(tmp + *len, data, n);
    *len += n;
    tmp[*len] = '\0';
    *buf = tmp;
    return true;
}

RaopClient::RaopClient()
    : m_rtsp_sock(-1),
      m_event_sock(-1),
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
      m_sid_int(0),
      m_cseq(0),
      m_connected(false),
      m_first_pkt(true),
      m_encrypt_aes(false),
      m_ap2(false),
      m_rtsp_encrypted(false),
      m_ctrl_write_counter(0),
      m_ctrl_read_counter(0),
      m_ev_write_counter(0),
      m_ev_read_counter(0),
      m_rx_enc_buf(NULL),
      m_rx_enc_len(0),
      m_rx_plain_buf(NULL),
      m_rx_plain_len(0),
      m_ev_enc_buf(NULL),
      m_ev_enc_len(0),
      m_ev_plain_buf(NULL),
      m_ev_plain_len(0),
      m_seq_num(0),
      m_ssrc(0),
      m_head_ts(0),
      m_latency_frames(44100),
      m_last_sync_us(0),
      m_last_keepalive_us(0),
      m_pkts_sent(0),
      m_ntp_replies(0),
      m_retransmits(0),
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
    m_ap2 = false;
    m_rtsp_encrypted = false;
    m_pkts_sent = 0;
    m_ntp_replies = 0;
    m_retransmits = 0;
    m_latency_frames = 44100;

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
    m_sid_int = (r1 % 1000000000UL) + 100000000UL;
    snprintf(m_sid, sizeof(m_sid), "%010lu", (unsigned long)m_sid_int);
    snprintf(m_sci, sizeof(m_sci), "%08lX%08lX", (unsigned long)r2, (unsigned long)r3);
    snprintf(m_dacp_id, sizeof(m_dacp_id), "%08lX%08lX", (unsigned long)r3, (unsigned long)r2);
    snprintf(m_active_remote, sizeof(m_active_remote), "%010lu", (unsigned long)(r2 % 1000000000UL + 100000000UL));

    struct in_addr paddr = {.s_addr = m_peer_ip_addr};
    snprintf(m_rtsp_url, sizeof(m_rtsp_url), "rtsp://%s/%s", inet_ntoa(paddr), m_sid);

    bool is_apple_ap2_candidate =
        (strcasestr(spk.am, "AudioAccessory") != NULL ||
         strcasestr(spk.am, "AppleTV") != NULL ||
         strcasestr(spk.am, "HomePod") != NULL ||
         strchr(spk.et, '5') != NULL);

    if (is_apple_ap2_candidate) {
        strncpy(m_user_agent, "AirPlay/999.0.0", sizeof(m_user_agent));
    } else {
        strncpy(m_user_agent, "iTunes/7.6.2 (Windows; N;)", sizeof(m_user_agent));
    }

    // Open UDP sockets for timing, control, and audio BEFORE SETUP (required by HomePod/AppleTV)
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

    if (is_apple_ap2_candidate) {
        bool supports_transient = false;
        if (doGetInfo(&supports_transient) && supports_transient) {
            m_ap2 = true;
        } else if (strcasestr(spk.am, "AudioAccessory") || strcasestr(spk.am, "AppleTV")) {
            m_ap2 = true;
        }
    }

    if (m_ap2) {
        Serial.println("[RAOP] Using AirPlay 2 (HomeKit transient pairing + ChaCha20-Poly1305)");
        struct in_addr laddr = {.s_addr = m_local_ip_addr};
        snprintf(m_rtsp_url, sizeof(m_rtsp_url), "rtsp://%s/%s", inet_ntoa(laddr), m_sid);
        m_encrypt_aes = false;
        m_latency_frames = 44100;

        if (!doAp2PairTransient()) {
            Serial.println("[RAOP] AirPlay 2 transient pairing failed");
            disconnect();
            return false;
        }

        uint16_t event_port = 0;
        if (!doAp2SetupSession(&event_port)) {
            Serial.println("[RAOP] AirPlay 2 session SETUP failed");
            disconnect();
            return false;
        }

        if (event_port > 0) {
            connectEventChannel(event_port);
        }

        if (!doAp2SetupStream()) {
            Serial.println("[RAOP] AirPlay 2 stream SETUP failed");
            disconnect();
            return false;
        }
    } else {
        Serial.println("[RAOP] Using AirPlay 1 (RAOP)");
        if (strchr(spk.et, '4')) {
            doAuthSetup();
        }

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
    Serial.printf("[RAOP] Connected (%s) -> %s (audio=%u ctrl=%u time=%u latency=%lu)\n",
                  m_ap2 ? "AirPlay 2" : "AirPlay 1",
                  spk.name, m_remote_audio_port, m_remote_ctrl_port, m_remote_time_port,
                  (unsigned long)m_latency_frames);
    return true;
}

void RaopClient::disconnect() {
    m_connected = false;
    if (m_timing_running) {
        m_timing_running = false;
        vTaskDelay(pdMS_TO_TICKS(150));
        m_timing_task = NULL;
    }
    if (m_event_sock >= 0) {
        close(m_event_sock);
        m_event_sock = -1;
    }
    if (m_rtsp_sock >= 0) {
        if (m_ap2 && m_rtsp_encrypted) {
            char hdr[64];
            snprintf(hdr, sizeof(hdr), "RTP-Info: seq=%u;rtptime=%u\r\n",
                     (unsigned)(m_seq_num + 1), (unsigned)(m_head_ts + 1));
            execRtsp("FLUSH", NULL, NULL, NULL, 0, hdr, NULL, 0, NULL, NULL);

            plb_t *root = plb_dict();
            size_t bplen = 0;
            uint8_t *bp = plb_encode(root, &bplen);
            plb_free(root);
            if (bp) {
                execRtsp("TEARDOWN", NULL, BPLIST_CONTENT_TYPE, bp, (int)bplen, NULL, NULL, 0, NULL, NULL);
                free(bp);
            }
        } else if (m_session[0]) {
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

    free(m_rx_enc_buf);   m_rx_enc_buf = NULL;   m_rx_enc_len = 0;
    free(m_rx_plain_buf); m_rx_plain_buf = NULL; m_rx_plain_len = 0;
    free(m_ev_enc_buf);   m_ev_enc_buf = NULL;   m_ev_enc_len = 0;
    free(m_ev_plain_buf); m_ev_plain_buf = NULL; m_ev_plain_len = 0;

    m_rtsp_encrypted = false;
    m_ap2 = false;
    m_session[0] = '\0';
}

int RaopClient::rtspSend(const uint8_t *data, int len) {
    if (m_rtsp_sock < 0) return -1;
    if (!m_rtsp_encrypted) {
        return send(m_rtsp_sock, data, len, 0);
    }

    uint8_t *enc = NULL;
    size_t enc_len = 0;
    if (!hap_chacha_encrypt(&enc, &enc_len, data, (size_t)len, m_ctrl_write_key, &m_ctrl_write_counter)) {
        Serial.println("[RAOP] Control channel ChaCha20 encryption failed");
        return -1;
    }

    size_t sent = 0;
    while (sent < enc_len) {
        int n = send(m_rtsp_sock, enc + sent, enc_len - sent, 0);
        if (n <= 0) break;
        sent += (size_t)n;
    }
    free(enc);
    return (sent == enc_len) ? len : -1;
}

bool RaopClient::rtspFillPlain(int timeout_ms) {
    if (m_rtsp_sock < 0) return false;
    struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(m_rtsp_sock, &rfds);
    if (select(m_rtsp_sock + 1, &rfds, NULL, NULL, &tv) <= 0) return false;

    uint8_t raw[1536];
    int n = recv(m_rtsp_sock, raw, sizeof(raw), 0);
    if (n <= 0) return false;

    if (!m_rtsp_encrypted) {
        return buf_append(&m_rx_plain_buf, &m_rx_plain_len, raw, (size_t)n);
    }

    if (!buf_append(&m_rx_enc_buf, &m_rx_enc_len, raw, (size_t)n)) return false;
    if (m_rx_enc_len < 2) return true;

    uint8_t *plain = NULL;
    size_t plain_len = 0;
    ssize_t used = hap_chacha_decrypt(&plain, &plain_len, m_rx_enc_buf, m_rx_enc_len,
                                      m_ctrl_read_key, &m_ctrl_read_counter);
    if (used < 0) {
        Serial.println("[RAOP] Control channel ChaCha20 decryption failed");
        return false;
    }
    if (used > 0) {
        memmove(m_rx_enc_buf, m_rx_enc_buf + used, m_rx_enc_len - (size_t)used);
        m_rx_enc_len -= (size_t)used;
        buf_append(&m_rx_plain_buf, &m_rx_plain_len, plain, plain_len);
    }
    free(plain);
    return true;
}

bool RaopClient::execRtsp(const char *cmd, const char *uri_override,
                          const char *content_type, const uint8_t *content, int content_len,
                          const char *extra_headers, char *resp_headers_out, int resp_headers_max,
                          uint8_t **resp_body_out, int *resp_body_len_out) {
    if (resp_body_out) *resp_body_out = NULL;
    if (resp_body_len_out) *resp_body_len_out = 0;
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

    if (m_ap2) {
        hdr_len += snprintf((char *)req + hdr_len, 1024 - hdr_len, "X-Apple-Client-Name: AirPlay\r\n");
    }
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
    int sent = rtspSend(req, total_send);
    free(req);
    if (sent != total_send) {
        Serial.printf("[RAOP] RTSP send failed (%s)\n", cmd);
        return false;
    }

    uint32_t start_ms = millis();
    char *hdr_end_ptr = NULL;
    int clen = 0;

    while (true) {
        if (m_rx_plain_buf && (hdr_end_ptr = strstr((char *)m_rx_plain_buf, "\r\n\r\n")) != NULL) {
            char *cl_ptr = strcasestr((char *)m_rx_plain_buf, "\r\nContent-Length:");
            if (cl_ptr && cl_ptr < hdr_end_ptr) {
                clen = atoi(cl_ptr + 17);
            }
            size_t needed = (size_t)(hdr_end_ptr + 4 - (char *)m_rx_plain_buf) + (size_t)clen;
            if (m_rx_plain_len >= needed) break;
        }
        uint32_t elapsed = millis() - start_ms;
        if (elapsed >= 6000 || !rtspFillPlain(6000 - (int)elapsed)) {
            Serial.printf("[RAOP] RTSP %s timeout waiting for response (rx=%u)\n", cmd, (unsigned)m_rx_plain_len);
            return false;
        }
    }

    size_t hdr_bytes = (size_t)(hdr_end_ptr + 4 - (char *)m_rx_plain_buf);
    int status_code = 0;
    sscanf((char *)m_rx_plain_buf, "RTSP/%*s %d", &status_code);
    if (status_code != 200) {
        Serial.printf("[RAOP] RTSP %s (%s) returned status %d\n",
                      cmd, uri_override ? uri_override : "", status_code);
    }

    if (resp_headers_out && resp_headers_max > 0) {
        size_t copy_h = (hdr_bytes < (size_t)(resp_headers_max - 1)) ? hdr_bytes : (size_t)(resp_headers_max - 1);
        memcpy(resp_headers_out, m_rx_plain_buf, copy_h);
        resp_headers_out[copy_h] = '\0';
    }

    if (resp_body_out && clen > 0) {
        uint8_t *b = (uint8_t *)malloc(clen + 1);
        if (b) {
            memcpy(b, m_rx_plain_buf + hdr_bytes, clen);
            b[clen] = '\0';
            *resp_body_out = b;
            if (resp_body_len_out) *resp_body_len_out = clen;
        }
    }

    size_t consumed = hdr_bytes + (size_t)clen;
    memmove(m_rx_plain_buf, m_rx_plain_buf + consumed, m_rx_plain_len - consumed + 1);
    m_rx_plain_len -= consumed;

    return status_code == 200;
}

bool RaopClient::doGetInfo(bool *supports_transient_out) {
    *supports_transient_out = false;
    uint8_t *body = NULL;
    int body_len = 0;
    if (!execRtsp("GET", "/info", NULL, NULL, 0, NULL, NULL, 0, &body, &body_len) || !body || body_len <= 0) {
        free(body);
        return false;
    }

    plb_t *root = plb_decode(body, (size_t)body_len);
    free(body);
    if (!root) {
        Serial.println("[RAOP] Failed to decode /info bplist");
        return false;
    }

    uint64_t features = 0;
    int64_t status_flags = 0;
    plb_t *fnode = plb_get(root, "features");
    if (fnode) {
        if (fnode->type == PLB_INT) {
            features = (uint64_t)fnode->i;
        } else if (fnode->type == PLB_STRING && fnode->s) {
            unsigned long long low = 0, high = 0;
            if (sscanf(fnode->s, "%llx,%llx", &low, &high) >= 1) {
                features = (uint64_t)low | ((uint64_t)high << 32);
            }
        }
    }
    plb_get_int(root, "statusFlags", &status_flags);
    const char *model = plb_get_string(root, "model");
    const char *src_ver = plb_get_string(root, "sourceVersion");

    // Bit 52 = SupportsUnifiedPairSetupAndMFi (transient pairing)
    bool transient = ((features & (1ULL << 52)) != 0);
    *supports_transient_out = transient;

    Serial.printf("[RAOP] /info: model=%s ver=%s features=0x%llx statusFlags=0x%llx transient=%d\n",
                  model ? model : "?", src_ver ? src_ver : "?",
                  (unsigned long long)features, (unsigned long long)status_flags, transient ? 1 : 0);

    plb_free(root);
    return true;
}

bool RaopClient::doAp2PairTransient() {
    // M1: State=1, Method=0 (PairSetup), Flags=0x10 (Transient)
    const uint8_t m1_tlv[] = {
        0x06, 0x01, 0x01, // TLVType_State = 1
        0x00, 0x01, 0x00, // TLVType_Method = 0
        0x13, 0x01, 0x10  // TLVType_Flags = 0x10 (Transient)
    };

    uint8_t *m2_body = NULL;
    int m2_len = 0;
    if (!execRtsp("POST", "/pair-setup", "application/octet-stream", m1_tlv, sizeof(m1_tlv),
                  "X-Apple-HKP: 4\r\n", NULL, 0, &m2_body, &m2_len) || !m2_body || m2_len <= 0) {
        Serial.println("[RAOP] pair-setup M1->M2 HTTP failed");
        free(m2_body);
        return false;
    }

    uint8_t err_code = 0;
    int err_len = 0;
    if (tlv8_extract(m2_body, m2_len, 0x07, &err_code, 1, &err_len) && err_len > 0) {
        Serial.printf("[RAOP] pair-setup M2 returned TLV error %u\n", err_code);
        free(m2_body);
        return false;
    }

    uint8_t *pkB_bytes = (uint8_t *)malloc(384);
    uint8_t salt_bytes[32] = {0};
    int pkB_len = 0, salt_len = 0;
    bool ok_m2 = tlv8_extract(m2_body, m2_len, 0x03, pkB_bytes, 384, &pkB_len) &&
                 tlv8_extract(m2_body, m2_len, 0x02, salt_bytes, sizeof(salt_bytes), &salt_len);
    free(m2_body);

    if (!ok_m2 || pkB_len == 0 || salt_len != 16) {
        Serial.printf("[RAOP] pair-setup M2 missing pkB (%d) or salt (%d)\n", pkB_len, salt_len);
        free(pkB_bytes);
        return false;
    }

    // Compute SRP-6a M3 (3072-bit group, SHA-512, I="Pair-Setup", P="3939")
    mbedtls_mpi N, g, s, B, a, A, k, u, x, v, S, tmp1, tmp2, tmp3, RR;
    mbedtls_mpi_init(&N); mbedtls_mpi_init(&g); mbedtls_mpi_init(&s); mbedtls_mpi_init(&B);
    mbedtls_mpi_init(&a); mbedtls_mpi_init(&A); mbedtls_mpi_init(&k); mbedtls_mpi_init(&u);
    mbedtls_mpi_init(&x); mbedtls_mpi_init(&v); mbedtls_mpi_init(&S);
    mbedtls_mpi_init(&tmp1); mbedtls_mpi_init(&tmp2); mbedtls_mpi_init(&tmp3); mbedtls_mpi_init(&RR);

    mbedtls_mpi_read_string(&N, 16, SRP_N_3072_HEX);
    mbedtls_mpi_lset(&g, 5);
    mbedtls_mpi_read_binary(&s, salt_bytes, (size_t)salt_len);
    mbedtls_mpi_read_binary(&B, pkB_bytes, (size_t)pkB_len);
    free(pkB_bytes);

    uint8_t rand_a[32];
    esp_fill_random(rand_a, sizeof(rand_a));
    mbedtls_mpi_read_binary(&a, rand_a, sizeof(rand_a));

    // A = g^a mod N
    mbedtls_mpi_exp_mod(&A, &g, &a, &N, &RR);

    // k = H(PAD(N) || PAD(g)), u = H(PAD(A) || PAD(B))
    srp_h_nn_pad(&k, &N, &g, 384);
    srp_h_nn_pad(&u, &A, &B, 384);

    // x = H(s || H("Pair-Setup:3939"))
    srp_calc_x(&x, &s, "Pair-Setup", "3939");

    // v = g^x mod N
    mbedtls_mpi_exp_mod(&v, &g, &x, &N, &RR);

    // tmp2 = a + u * x
    mbedtls_mpi_mul_mpi(&tmp1, &u, &x);
    mbedtls_mpi_add_mpi(&tmp2, &a, &tmp1);

    // tmp1 = (B - (k * v mod N) + N) mod N
    mbedtls_mpi_mul_mpi(&tmp3, &k, &v);
    mbedtls_mpi_mod_mpi(&tmp3, &tmp3, &N);
    mbedtls_mpi_mod_mpi(&tmp1, &B, &N);
    if (mbedtls_mpi_cmp_mpi(&tmp1, &tmp3) < 0) {
        mbedtls_mpi_add_mpi(&tmp1, &tmp1, &N);
    }
    mbedtls_mpi_sub_mpi(&tmp1, &tmp1, &tmp3);

    // S = tmp1 ^ tmp2 mod N
    mbedtls_mpi_exp_mod(&S, &tmp1, &tmp2, &N, &RR);

    // session_key K = SHA512(S)
    uint8_t session_key[64];
    sha512_mpi_hash(&S, session_key);

    // M1 = H((H(N) ^ H(g)) || H(I) || s || A || B || K)
    uint8_t H_N[64], H_g[64], H_I[64], H_xor[64], M1_proof[64], H_AMK[64];
    sha512_mpi_hash(&N, H_N);
    sha512_mpi_hash(&g, H_g);
    mbedtls_sha512((const uint8_t *)"Pair-Setup", 10, H_I, 0);
    for (int i = 0; i < 64; i++) H_xor[i] = H_N[i] ^ H_g[i];

    mbedtls_sha512_context sha_ctx;
    mbedtls_sha512_init(&sha_ctx);
    mbedtls_sha512_starts(&sha_ctx, 0);
    mbedtls_sha512_update(&sha_ctx, H_xor, 64);
    mbedtls_sha512_update(&sha_ctx, H_I, 64);
    sha512_mpi_update(&sha_ctx, &s);
    sha512_mpi_update(&sha_ctx, &A);
    sha512_mpi_update(&sha_ctx, &B);
    mbedtls_sha512_update(&sha_ctx, session_key, 64);
    mbedtls_sha512_finish(&sha_ctx, M1_proof);

    // H_AMK = H(A || M1 || K)
    mbedtls_sha512_starts(&sha_ctx, 0);
    sha512_mpi_update(&sha_ctx, &A);
    mbedtls_sha512_update(&sha_ctx, M1_proof, 64);
    mbedtls_sha512_update(&sha_ctx, session_key, 64);
    mbedtls_sha512_finish(&sha_ctx, H_AMK);
    mbedtls_sha512_free(&sha_ctx);

    size_t pkA_len = mbedtls_mpi_size(&A);
    uint8_t *pkA_bytes = (uint8_t *)malloc(pkA_len ? pkA_len : 1);
    mbedtls_mpi_write_binary(&A, pkA_bytes, pkA_len);

    mbedtls_mpi_free(&N); mbedtls_mpi_free(&g); mbedtls_mpi_free(&s); mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&a); mbedtls_mpi_free(&A); mbedtls_mpi_free(&k); mbedtls_mpi_free(&u);
    mbedtls_mpi_free(&x); mbedtls_mpi_free(&v); mbedtls_mpi_free(&S);
    mbedtls_mpi_free(&tmp1); mbedtls_mpi_free(&tmp2); mbedtls_mpi_free(&tmp3); mbedtls_mpi_free(&RR);

    // Format M3 TLV: State=3, PublicKey=pkA, Proof=M1_proof
    uint8_t *m3_tlv = (uint8_t *)malloc(512);
    int m3_pos = 0;
    uint8_t state3 = 0x03;
    m3_pos += tlv8_append(m3_tlv + m3_pos, 0x06, &state3, 1);
    m3_pos += tlv8_append(m3_tlv + m3_pos, 0x03, pkA_bytes, (int)pkA_len);
    m3_pos += tlv8_append(m3_tlv + m3_pos, 0x04, M1_proof, 64);
    free(pkA_bytes);

    uint8_t *m4_body = NULL;
    int m4_len = 0;
    bool ok_m4 = execRtsp("POST", "/pair-setup", "application/octet-stream", m3_tlv, m3_pos,
                          "X-Apple-HKP: 4\r\n", NULL, 0, &m4_body, &m4_len);
    free(m3_tlv);

    if (!ok_m4 || !m4_body || m4_len <= 0) {
        Serial.println("[RAOP] pair-setup M3->M4 HTTP failed");
        free(m4_body);
        return false;
    }

    if (tlv8_extract(m4_body, m4_len, 0x07, &err_code, 1, &err_len) && err_len > 0) {
        Serial.printf("[RAOP] pair-setup M4 returned TLV error %u\n", err_code);
        free(m4_body);
        return false;
    }

    uint8_t M2_proof[64] = {0};
    int m2_proof_len = 0;
    bool has_proof = tlv8_extract(m4_body, m4_len, 0x04, M2_proof, 64, &m2_proof_len);
    free(m4_body);

    if (!has_proof || m2_proof_len != 64 || memcmp(M2_proof, H_AMK, 64) != 0) {
        Serial.println("[RAOP] pair-setup M4 server proof verification failed");
        return false;
    }

    // Derive Control and Event channel keys via HKDF-SHA-512
    hkdf_sha512(m_ctrl_write_key, 32, session_key, 64, "Control-Salt", "Control-Write-Encryption-Key");
    hkdf_sha512(m_ctrl_read_key,  32, session_key, 64, "Control-Salt", "Control-Read-Encryption-Key");
    hkdf_sha512(m_ev_write_key,   32, session_key, 64, "Events-Salt",  "Events-Read-Encryption-Key");
    hkdf_sha512(m_ev_read_key,    32, session_key, 64, "Events-Salt",  "Events-Write-Encryption-Key");

    m_ctrl_write_counter = 0;
    m_ctrl_read_counter = 0;
    m_ev_write_counter = 0;
    m_ev_read_counter = 0;

    free(m_rx_enc_buf);   m_rx_enc_buf = NULL;   m_rx_enc_len = 0;
    free(m_rx_plain_buf); m_rx_plain_buf = NULL; m_rx_plain_len = 0;
    m_rtsp_encrypted = true;

    Serial.println("[RAOP] AirPlay 2 transient pairing succeeded! Control channel encrypted.");
    return true;
}

bool RaopClient::doAp2SetupSession(uint16_t *event_port_out) {
    *event_port_out = 0;
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));

    char device_id[18], uuid[37];
    snprintf(device_id, sizeof(device_id), "%02X:%02X:%02X:%02X:%02X:%02X",
             rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5]);
    snprintf(uuid, sizeof(uuid), "%02X%02X%02X%02X-%02X%02X-4%01X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6] & 0x0f, rnd[7],
             (rnd[8] & 0x3f) | 0x80, rnd[9], rnd[10], rnd[11], rnd[12], rnd[13], rnd[14], rnd[15]);

    plb_t *root = plb_dict();
    plb_dict_set(root, "deviceID", plb_string(device_id));
    plb_dict_set(root, "sessionUUID", plb_string(uuid));
    plb_dict_set(root, "timingPort", plb_int(m_local_time_port));
    plb_dict_set(root, "timingProtocol", plb_string("NTP"));

    size_t body_len = 0;
    uint8_t *body = plb_encode(root, &body_len);
    plb_free(root);
    if (!body) return false;

    char resp_hdrs[1024] = {0};
    uint8_t *rsp_body = NULL;
    int rsp_len = 0;
    bool ok = execRtsp("SETUP", NULL, BPLIST_CONTENT_TYPE, body, (int)body_len,
                       NULL, resp_hdrs, sizeof(resp_hdrs), &rsp_body, &rsp_len);
    free(body);
    if (!ok) {
        free(rsp_body);
        return false;
    }

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

    if (rsp_body && rsp_len > 0) {
        plb_t *rroot = plb_decode(rsp_body, (size_t)rsp_len);
        if (rroot) {
            int64_t v = 0;
            if (plb_get_int(rroot, "eventPort", &v)) *event_port_out = (uint16_t)v;
            if (plb_get_int(rroot, "timingPort", &v)) m_remote_time_port = (uint16_t)v;
            plb_free(rroot);
        }
    }
    free(rsp_body);

    Serial.printf("[RAOP] AirPlay 2 session SETUP OK (eventPort=%u timingPort=%u)\n",
                  *event_port_out, m_remote_time_port);
    return true;
}

void RaopClient::connectEventChannel(uint16_t event_port) {
    m_event_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_event_sock < 0) return;

    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(m_event_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(m_event_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in peer_in = {};
    peer_in.sin_family = AF_INET;
    peer_in.sin_addr.s_addr = m_peer_ip_addr;
    peer_in.sin_port = htons(event_port);

    if (::connect(m_event_sock, (struct sockaddr *)&peer_in, sizeof(peer_in)) < 0) {
        Serial.printf("[RAOP] Event channel connect to port %u failed (non-fatal)\n", event_port);
        close(m_event_sock);
        m_event_sock = -1;
        return;
    }
    Serial.printf("[RAOP] Connected to AirPlay 2 reverse event channel port %u\n", event_port);
}

void RaopClient::pollEventChannel() {
    if (m_event_sock < 0) return;
    uint8_t raw[1024];
    int n = recv(m_event_sock, raw, sizeof(raw), MSG_DONTWAIT);
    if (n <= 0) return;

    if (!buf_append(&m_ev_enc_buf, &m_ev_enc_len, raw, (size_t)n)) return;
    if (m_ev_enc_len < 2) return;

    uint8_t *plain = NULL;
    size_t plain_len = 0;
    ssize_t used = hap_chacha_decrypt(&plain, &plain_len, m_ev_enc_buf, m_ev_enc_len,
                                      m_ev_read_key, &m_ev_read_counter);
    if (used < 0) {
        close(m_event_sock);
        m_event_sock = -1;
        return;
    }
    if (used > 0) {
        memmove(m_ev_enc_buf, m_ev_enc_buf + used, m_ev_enc_len - (size_t)used);
        m_ev_enc_len -= (size_t)used;
        buf_append(&m_ev_plain_buf, &m_ev_plain_len, plain, plain_len);
    }
    free(plain);

    while (m_ev_plain_len > 0 && m_ev_plain_buf) {
        char *end = strstr((char *)m_ev_plain_buf, "\r\n\r\n");
        if (!end) break;
        size_t hlen = (size_t)(end + 4 - (char *)m_ev_plain_buf);
        size_t clen = 0;
        char *cl_ptr = strcasestr((char *)m_ev_plain_buf, "Content-Length:");
        if (cl_ptr && cl_ptr < end) clen = (size_t)strtoul(cl_ptr + 15, NULL, 10);
        if (m_ev_plain_len < hlen + clen) break;

        char cseq[16] = "";
        char *cs_ptr = strcasestr((char *)m_ev_plain_buf, "CSeq:");
        if (cs_ptr && cs_ptr < end) sscanf(cs_ptr + 5, " %15[0-9]", cseq);

        char rsp[192];
        int rlen = snprintf(rsp, sizeof(rsp),
                            "RTSP/1.0 200 OK\r\nServer: AirTunes/366.0\r\n%s%s%sContent-Length: 0\r\n\r\n",
                            cseq[0] ? "CSeq: " : "", cseq, cseq[0] ? "\r\n" : "");
        uint8_t *enc_rsp = NULL;
        size_t enc_rsp_len = 0;
        if (hap_chacha_encrypt(&enc_rsp, &enc_rsp_len, (const uint8_t *)rsp, (size_t)rlen,
                               m_ev_write_key, &m_ev_write_counter)) {
            send(m_event_sock, enc_rsp, enc_rsp_len, 0);
            free(enc_rsp);
        }

        size_t total = hlen + clen;
        memmove(m_ev_plain_buf, m_ev_plain_buf + total, m_ev_plain_len - total + 1);
        m_ev_plain_len -= total;
    }
}

bool RaopClient::doAp2SetupStream() {
    esp_fill_random(m_shk, sizeof(m_shk));

    plb_t *stream = plb_dict();
    // 0x40000 = ALAC 44100Hz 16-bit stereo
    plb_dict_set(stream, "audioFormat", plb_int(0x40000));
    plb_dict_set(stream, "audioMode", plb_string("default"));
    plb_dict_set(stream, "controlPort", plb_int(m_local_ctrl_port));
    plb_dict_set(stream, "ct", plb_int(2));
    plb_dict_set(stream, "isMedia", plb_bool(true));
    plb_dict_set(stream, "latencyMax", plb_int(88200));
    plb_dict_set(stream, "latencyMin", plb_int(11025));
    plb_dict_set(stream, "shk", plb_data(m_shk, sizeof(m_shk)));
    plb_dict_set(stream, "spf", plb_int(RAOP_CHUNK_FRAMES));
    plb_dict_set(stream, "sr", plb_int(RAOP_SAMPLE_RATE));
    plb_dict_set(stream, "type", plb_int(0x60));
    plb_dict_set(stream, "supportsDynamicStreamID", plb_bool(false));
    plb_dict_set(stream, "streamConnectionID", plb_int(m_sid_int));

    plb_t *streams = plb_array();
    plb_array_add(streams, stream);
    plb_t *root = plb_dict();
    plb_dict_set(root, "streams", streams);

    size_t body_len = 0;
    uint8_t *body = plb_encode(root, &body_len);
    plb_free(root);
    if (!body) return false;

    uint8_t *rsp_body = NULL;
    int rsp_len = 0;
    bool ok = execRtsp("SETUP", NULL, BPLIST_CONTENT_TYPE, body, (int)body_len,
                       NULL, NULL, 0, &rsp_body, &rsp_len);
    free(body);
    if (!ok) {
        free(rsp_body);
        return false;
    }

    m_remote_audio_port = 0;
    m_remote_ctrl_port = 0;
    if (rsp_body && rsp_len > 0) {
        plb_t *rroot = plb_decode(rsp_body, (size_t)rsp_len);
        if (rroot) {
            plb_t *s0 = plb_at(plb_get(rroot, "streams"), 0);
            if (s0) {
                int64_t v = 0;
                if (plb_get_int(s0, "dataPort", &v)) m_remote_audio_port = (uint16_t)v;
                if (plb_get_int(s0, "controlPort", &v)) m_remote_ctrl_port = (uint16_t)v;
            }
            plb_free(rroot);
        }
    }
    free(rsp_body);

    Serial.printf("[RAOP] AirPlay 2 stream SETUP: dataPort=%u controlPort=%u\n",
                  m_remote_audio_port, m_remote_ctrl_port);
    return (m_remote_audio_port != 0 && m_remote_ctrl_port != 0);
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
    float db = (volume_pct == 0) ? -144.0f : (-28.0f + (28.0f * (float)volume_pct) / 100.0f);
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

bool RaopClient::acceptFrames() const {
    if (!m_connected) return false;
    if (m_first_pkt) return true;
    uint64_t now_ts = ntp_to_ts(get_ntp_now(NULL, NULL), RAOP_SAMPLE_RATE);
    return (now_ts >= m_head_ts + RAOP_CHUNK_FRAMES);
}

bool RaopClient::sendChunk(const int16_t *stereo_pcm_352) {
    if (!m_connected || m_audio_sock < 0) return false;

    uint64_t now_ts = ntp_to_ts(get_ntp_now(NULL, NULL), RAOP_SAMPLE_RATE);
    if (m_first_pkt || now_ts > m_head_ts + 22050ULL) {
        m_head_ts = now_ts;
        sendSyncPacket(true);
        m_last_sync_us = esp_timer_get_time();
    }

    uint8_t alac_buf[1424];
    int alac_len = encode_pcm_to_alac_raw(stereo_pcm_352, RAOP_CHUNK_FRAMES, alac_buf);

    if (!m_ap2 && m_encrypt_aes) {
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
    uint8_t rtp_pkt[1480];
    rtp_pkt[0] = 0x80;
    rtp_pkt[1] = 0x60 | (m_first_pkt ? 0x80 : 0x00);
    m_first_pkt = false;
    rtp_pkt[2] = (m_seq_num >> 8) & 0xFF;
    rtp_pkt[3] = m_seq_num & 0xFF;
    uint32_t ts_net = htonl((uint32_t)m_head_ts);
    uint32_t ssrc_net = htonl(m_ssrc);
    memcpy(rtp_pkt + 4, &ts_net, 4);
    memcpy(rtp_pkt + 8, &ssrc_net, 4);

    int total_len = 0;
    if (m_ap2) {
        // AirPlay 2: RTP header (12B) in clear, ALAC payload encrypted with ChaCha20-Poly1305
        // using shk, AAD = rtp_pkt[4..11] (timestamp + ssrc), nonce[4..5] = seq (LE),
        // followed by 16B tag + 8B nonce suffix
        uint8_t nonce[12] = {0};
        nonce[4] = (uint8_t)(m_seq_num & 0xFF);
        nonce[5] = (uint8_t)((m_seq_num >> 8) & 0xFF);
        uint8_t tag[AP2_TAG_SIZE];
        unsigned long long maclen = 0;
        int rc = crypto_aead_chacha20poly1305_ietf_encrypt_detached(
            rtp_pkt + 12, tag, &maclen,
            alac_buf, (unsigned long long)alac_len,
            rtp_pkt + 4, 8,
            NULL, nonce, m_shk);
        if (rc != 0) return false;

        memcpy(rtp_pkt + 12 + alac_len, tag, AP2_TAG_SIZE);
        memcpy(rtp_pkt + 12 + alac_len + AP2_TAG_SIZE, nonce + 4, AP2_NONCE_SIZE);
        total_len = 12 + alac_len + AP2_TAG_SIZE + AP2_NONCE_SIZE;
    } else {
        memcpy(rtp_pkt + 12, alac_buf, alac_len);
        total_len = 12 + alac_len;
    }

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
    m_pkts_sent++;

    uint64_t now_us = esp_timer_get_time();
    if (now_us - m_last_sync_us >= 1000000ULL) {
        sendSyncPacket(false);
        m_last_sync_us = now_us;
        if ((m_pkts_sent % 500) < 5) {
            Serial.printf("[RAOP] Stream stats: mode=%s pkts=%lu ntp=%lu retr=%lu seq=%u\n",
                          m_ap2 ? "AP2" : "AP1",
                          (unsigned long)m_pkts_sent,
                          (unsigned long)m_ntp_replies,
                          (unsigned long)m_retransmits,
                          (unsigned)m_seq_num);
        }
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
                    uint8_t retr[1484];
                    retr[0] = 0x80;
                    retr[1] = 0x56 | 0x80;
                    retr[2] = 0x00;
                    retr[3] = 0x01;
                    memcpy(retr + 4, slot.data, slot.len);
                    sendto(m_ctrl_sock, retr, 4 + slot.len, 0, (struct sockaddr *)&dst, sizeof(dst));
                    m_retransmits++;
                }
            }
        }
    }
}

void RaopClient::pollControlAndKeepalive() {
    if (!m_connected) return;
    handleControlSocket();
    if (m_ap2) {
        pollEventChannel();
    }
    uint64_t now_us = esp_timer_get_time();
    if (now_us - m_last_keepalive_us >= 20000000ULL) {
        m_last_keepalive_us = now_us;
        if (m_ap2) {
            execRtsp("POST", "/feedback", NULL, NULL, 0, NULL, NULL, 0, NULL, NULL);
        } else {
            execRtsp("OPTIONS", "*", NULL, NULL, 0, NULL, NULL, 0, NULL, NULL);
        }
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
            m_ntp_replies++;
        }
    }
}

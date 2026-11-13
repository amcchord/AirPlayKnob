#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

#include "radio_streamer.h"
#include "audio_buffer_config.h"
#include "artwork_fetcher.h"
#include "app_state.h"
#include "raop_AnySpeaker.h"
#include "sonos_radio.h"
#include "bsp/local_dac.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <lwip/sockets.h>
#include <lwip/tcp.h>
#include <lwip/priv/tcp_priv.h>
#include <lwip/tcpip.h>
#include <lwip/ip_addr.h>
#include <lwip/pbuf.h>
#include <fcntl.h>
#include <errno.h>
#include <new>
#include <esp_heap_caps.h>
#include <esp_timer.h>

// 512 KiB stereo PCM ring in PSRAM: 131072 frames, ~2.97 seconds.
constexpr uint32_t PCM_RING_FRAMES = AudioBuffer::PCM_FRAMES;
// 256KB MP3 buffer in PSRAM (~16.4 seconds at 128kbps) absorbs the entire 260KB Icecast initial burst
// so lwIP's TCP receive window never advertises ZeroWindow!
constexpr uint32_t MP3_BUF_SIZE = AudioBuffer::MP3_BYTES;
static int16_t *s_pcm_ring = NULL;
static volatile uint32_t s_ring_write = 0;
static volatile uint32_t s_ring_read = 0;
static volatile uint32_t s_ring_seq = 0;
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t ring_available_frames() {
    portENTER_CRITICAL(&s_ring_mux);
    uint32_t avail = (s_ring_write - s_ring_read) & (PCM_RING_FRAMES - 1);
    portEXIT_CRITICAL(&s_ring_mux);
    return avail;
}

uint32_t radio_get_buffered_ms(void) {
    return (uint32_t)(((uint64_t)ring_available_frames() * 1000ULL) / 44100ULL);
}

static uint32_t ring_free_frames() {
    return (PCM_RING_FRAMES - 1) - ring_available_frames();
}

static void ring_clear() {
    portENTER_CRITICAL(&s_ring_mux);
    s_ring_write = 0;
    s_ring_read = 0;
    s_ring_seq = s_ring_seq + 1;
    portEXIT_CRITICAL(&s_ring_mux);
}

static void ring_push_frames(const int16_t *stereo_frames, uint32_t count) {
    if (!s_pcm_ring || count == 0) return;
    portENTER_CRITICAL(&s_ring_mux);
    uint32_t avail = (s_ring_write - s_ring_read) & (PCM_RING_FRAMES - 1);
    uint32_t free_f = (PCM_RING_FRAMES - 1) - avail;
    uint32_t w = s_ring_write;
    uint32_t seq = s_ring_seq;
    portEXIT_CRITICAL(&s_ring_mux);

    if (count > free_f) count = free_f;
    if (count == 0) return;

    uint32_t first = PCM_RING_FRAMES - w;
    if (first >= count) {
        memcpy(&s_pcm_ring[w * 2], stereo_frames, count * 2 * sizeof(int16_t));
    } else {
        memcpy(&s_pcm_ring[w * 2], stereo_frames, first * 2 * sizeof(int16_t));
        memcpy(&s_pcm_ring[0], &stereo_frames[first * 2], (count - first) * 2 * sizeof(int16_t));
    }

    portENTER_CRITICAL(&s_ring_mux);
    if (s_ring_seq == seq) {
        s_ring_write = (w + count) & (PCM_RING_FRAMES - 1);
    }
    portEXIT_CRITICAL(&s_ring_mux);
}

static bool ring_pop_352(int16_t *out_stereo_352) {
    if (!s_pcm_ring) return false;
    portENTER_CRITICAL(&s_ring_mux);
    uint32_t avail = (s_ring_write - s_ring_read) & (PCM_RING_FRAMES - 1);
    uint32_t r = s_ring_read;
    uint32_t seq = s_ring_seq;
    portEXIT_CRITICAL(&s_ring_mux);

    if (avail < RAOP_CHUNK_FRAMES) {
        return false;
    }

    uint32_t first = PCM_RING_FRAMES - r;
    if (first >= RAOP_CHUNK_FRAMES) {
        memcpy(out_stereo_352, &s_pcm_ring[r * 2], RAOP_CHUNK_FRAMES * 2 * sizeof(int16_t));
    } else {
        memcpy(out_stereo_352, &s_pcm_ring[r * 2], first * 2 * sizeof(int16_t));
        memcpy(&out_stereo_352[first * 2], &s_pcm_ring[0], (RAOP_CHUNK_FRAMES - first) * 2 * sizeof(int16_t));
    }

    portENTER_CRITICAL(&s_ring_mux);
    if (s_ring_seq == seq) {
        s_ring_read = (r + RAOP_CHUNK_FRAMES) & (PCM_RING_FRAMES - 1);
    }
    portEXIT_CRITICAL(&s_ring_mux);
    return true;
}

static void update_vu_meters(const int16_t *stereo_352) {
    uint8_t bars[12] = {0};
    int32_t max_abs = 0;
    int samples_per_bar = RAOP_CHUNK_FRAMES / 12;

    for (int b = 0; b < 12; b++) {
        int32_t sum = 0;
        int start = b * samples_per_bar;
        for (int i = 0; i < samples_per_bar; i++) {
            int32_t l = abs((int32_t)stereo_352[(start + i) * 2]);
            int32_t r = abs((int32_t)stereo_352[(start + i) * 2 + 1]);
            int32_t m = (l + r) >> 1;
            sum += m;
            if (m > max_abs) max_abs = m;
        }
        int32_t avg = sum / samples_per_bar;
        int pct = (avg * 100) / 14000;
        if (pct > 100) pct = 100;
        bars[b] = (uint8_t)pct;
    }
    int peak_pct = (max_abs * 100) / 24000;
    if (peak_pct > 100) peak_pct = 100;

    int8_t wl[128], wr[128];
    for (int i = 0; i < 128; i++) {
        int idx = (i * RAOP_CHUNK_FRAMES) / 128;
        int32_t sl = stereo_352[idx * 2] / 220;
        int32_t sr = stereo_352[idx * 2 + 1] / 220;
        if (sl < -127) sl = -127;
        if (sl > 127)  sl = 127;
        if (sr < -127) sr = -127;
        if (sr > 127)  sr = 127;
        wl[i] = (int8_t)sl;
        wr[i] = (int8_t)sr;
    }

    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        for (int b = 0; b < 12; b++) {
            // Smooth decay
            if (bars[b] >= g_app.vu_bars[b]) {
                g_app.vu_bars[b] = bars[b];
            } else {
                g_app.vu_bars[b] = (uint8_t)((g_app.vu_bars[b] * 3 + bars[b]) / 4);
            }
        }
        g_app.vu_peak = (uint8_t)peak_pct;
        memcpy(g_app.wave_l, wl, 128);
        memcpy(g_app.wave_r, wr, 128);
        xSemaphoreGive(g_app.mutex);
    }
}

static void parse_icy_metadata(const char *meta_buf, int meta_len) {
    const char *p = strstr(meta_buf, "StreamTitle='");
    if (!p) return;
    p += 13;
    const char *end = strstr(p, "';");
    if (!end) end = strchr(p, '\'');
    if (!end) return;

    int len = (int)(end - p);
    if (len <= 0) return;
    if (len > 120) len = 120;

    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        memcpy(g_app.track_title, p, len);
        g_app.track_title[len] = '\0';
        xSemaphoreGive(g_app.mutex);
        Serial.printf("[ICY] Now Playing: %s\n", g_app.track_title);
    }
}

static bool radio_should_abort_stream_open(void) {
    bool abort_now = false;
    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (!g_app.playing || !g_app.wifi_connected || g_app.stream_reconfig_needed || g_app.ota_in_progress ||
            ((g_app.sonos_native || g_app.sonos_radio_selected) &&
             !g_app.also_local_dac && !g_app.whitecap_active)) {
            abort_now = true;
        }
        xSemaphoreGive(g_app.mutex);
    }
    return abort_now;
}

// Raw lwIP TCP reader for HTTP Icecast streams.
// Precompiled ESP32-S3 liblwip.a hardcodes CONFIG_LWIP_TCP_WND_DEFAULT=5760 (4 MSS),
// CONFIG_LWIP_TCP_RECVMBOX_SIZE=6, TCP_WND_UPDATE_THRESHOLD=1440 (> 1436 MSS),
// CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS=4, and TCP_TMR_INTERVAL=250ms. Using a raw lwIP PCB
// with a 16-MSS (22,976 B) receive window (~276 KB/s BDP at 83ms RTT), TF_SACK cleared
// (preventing >4-pbuf OOSEQ SACK reneging), and immediate ACK absorbs SomaFM's initial
// burst in ~1.2 seconds and eliminates the 250ms Delayed-ACK stall (4,308 B/s) completely.
struct IcyRawTcp {
    static constexpr uint32_t RING_SIZE   = 131072U; // 128 KiB power-of-two PSRAM SPSC ring
    static constexpr uint32_t RING_MASK   = RING_SIZE - 1U;
    static constexpr uint32_t ICY_TCP_WND = 22976U;  // 16 * 1436 MSS (<= 65535 without WND_SCALE)

    uint8_t *ring = NULL;
    SemaphoreHandle_t rx_sem = NULL;
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    volatile uint32_t write_idx = 0;
    volatile uint32_t read_idx = 0;
    struct tcp_pcb *pcb = NULL;
    volatile bool connected = false;
    volatile bool connect_failed = false;
    volatile bool peer_closed = false;
    volatile bool wnd_throttled = false;
    volatile err_t last_err = ERR_OK;
    char pending_req[384] = {0};
    uint16_t pending_req_len = 0;

    bool ensure_init() {
        if (!ring) {
            ring = (uint8_t *)heap_caps_malloc(RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!ring) {
                ring = (uint8_t *)heap_caps_malloc(RING_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            }
        }
        if (!rx_sem) {
            rx_sem = xSemaphoreCreateBinary();
        }
        return (ring != NULL) && (rx_sem != NULL);
    }

    uint32_t avail_bytes() {
        portENTER_CRITICAL(&mux);
        uint32_t av = (write_idx - read_idx) & RING_MASK;
        portEXIT_CRITICAL(&mux);
        return av;
    }

    uint32_t free_bytes() {
        return (RING_SIZE - 1U) - avail_bytes();
    }

    static err_t on_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err) {
        IcyRawTcp *ctx = static_cast<IcyRawTcp *>(arg);
        if (!ctx || ctx->pcb != tpcb) return ERR_OK;
        if (err != ERR_OK) {
            portENTER_CRITICAL(&ctx->mux);
            ctx->connect_failed = true;
            ctx->last_err = err;
            portEXIT_CRITICAL(&ctx->mux);
            if (ctx->rx_sem) xSemaphoreGive(ctx->rx_sem);
            return ERR_OK;
        }

        // Clear TF_SACK before advertising 16-MSS (22,976 B) receive window: precompiled
        // liblwip.a limits CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS=4, so >4 out-of-order segments
        // with SACK active triggers tcp_remove_sacks_gt() SACK reneging and Linux sender RTOs.
#ifdef TF_SACK
        tcp_clear_flags(tpcb, TF_SACK);
#endif
        tpcb->rcv_wnd = (tcpwnd_size_t)ICY_TCP_WND;
        tcp_update_rcv_ann_wnd(tpcb);
        tcp_nagle_disable(tpcb);

        bool write_ok = true;
        if (ctx->pending_req_len > 0) {
            err_t we = tcp_write(tpcb, ctx->pending_req, (u16_t)ctx->pending_req_len, TCP_WRITE_FLAG_COPY);
            if (we == ERR_OK) {
                ctx->pending_req_len = 0;
            } else {
                write_ok = false;
                ctx->last_err = we;
            }
        }

        portENTER_CRITICAL(&ctx->mux);
        if (write_ok) {
            ctx->connected = true;
        } else {
            ctx->connect_failed = true;
        }
        portEXIT_CRITICAL(&ctx->mux);
        if (ctx->rx_sem) xSemaphoreGive(ctx->rx_sem);
        return ERR_OK;
    }

    static err_t on_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
        (void)err;
        IcyRawTcp *ctx = static_cast<IcyRawTcp *>(arg);
        if (!ctx || ctx->pcb != tpcb) {
            if (p) pbuf_free(p);
            return ERR_OK;
        }
        if (!p) {
            portENTER_CRITICAL(&ctx->mux);
            ctx->peer_closed = true;
            portEXIT_CRITICAL(&ctx->mux);
            if (ctx->rx_sem) xSemaphoreGive(ctx->rx_sem);
            return ERR_OK;
        }

        portENTER_CRITICAL(&ctx->mux);
        uint32_t avail = (ctx->write_idx - ctx->read_idx) & RING_MASK;
        uint32_t free_sp = (RING_SIZE - 1U) - avail;
        uint32_t w = ctx->write_idx;
        portEXIT_CRITICAL(&ctx->mux);

        uint32_t n = (uint32_t)p->tot_len;
        if (n > free_sp) {
            portENTER_CRITICAL(&ctx->mux);
            ctx->wnd_throttled = true;
            portEXIT_CRITICAL(&ctx->mux);
            if (ctx->rx_sem) xSemaphoreGive(ctx->rx_sem);
            return ERR_MEM; // Keep p in tpcb->refused_data without dropping bytes
        }

        if (n > 0) {
            uint32_t first = RING_SIZE - w;
            if (first >= n) {
                pbuf_copy_partial(p, ctx->ring + w, (u16_t)n, 0);
            } else {
                pbuf_copy_partial(p, ctx->ring + w, (u16_t)first, 0);
                pbuf_copy_partial(p, ctx->ring, (u16_t)(n - first), (u16_t)first);
            }
        }
        pbuf_free(p);

        uint32_t rem_space = 0;
        portENTER_CRITICAL(&ctx->mux);
        ctx->write_idx = (w + n) & RING_MASK;
        uint32_t new_avail = (ctx->write_idx - ctx->read_idx) & RING_MASK;
        rem_space = (RING_SIZE - 1U) - new_avail;
        ctx->wnd_throttled = (rem_space < ICY_TCP_WND);
        portEXIT_CRITICAL(&ctx->mux);

        tcpwnd_size_t target_wnd = (rem_space >= ICY_TCP_WND) ? (tcpwnd_size_t)ICY_TCP_WND : (tcpwnd_size_t)rem_space;
        if (target_wnd > tpcb->rcv_wnd) {
            tpcb->rcv_wnd = target_wnd;
        }
        tcp_update_rcv_ann_wnd(tpcb);
        tcp_clear_flags(tpcb, TF_ACK_DELAY);
        tcp_ack_now(tpcb);

        if (ctx->rx_sem) xSemaphoreGive(ctx->rx_sem);
        return ERR_OK;
    }

    static void on_err_cb(void *arg, err_t err) {
        IcyRawTcp *ctx = static_cast<IcyRawTcp *>(arg);
        if (!ctx) return;
        ctx->pcb = NULL; // Already freed by lwIP before invoking err_cb
        portENTER_CRITICAL(&ctx->mux);
        ctx->peer_closed = true;
        ctx->connect_failed = true;
        ctx->last_err = err;
        portEXIT_CRITICAL(&ctx->mux);
        if (ctx->rx_sem) xSemaphoreGive(ctx->rx_sem);
    }

    void close_raw() {
        LOCK_TCPIP_CORE();
        if (pcb) {
            struct tcp_pcb *old = pcb;
            pcb = NULL;
            tcp_arg(old, NULL);
            tcp_recv(old, NULL);
            tcp_err(old, NULL);
            tcp_poll(old, NULL, 0);
            tcp_sent(old, NULL);
            if (tcp_close(old) != ERR_OK) {
                tcp_abort(old);
            }
        }
        UNLOCK_TCPIP_CORE();

        portENTER_CRITICAL(&mux);
        write_idx = 0;
        read_idx = 0;
        connected = false;
        connect_failed = false;
        peer_closed = true;
        wnd_throttled = false;
        pending_req_len = 0;
        portEXIT_CRITICAL(&mux);
    }

    int connect_and_send(const IPAddress &remote_ip, uint16_t port,
                         const char *req, int req_len, uint32_t timeout_ms) {
        close_raw();
        if (!ensure_init() || req_len <= 0 || req_len >= (int)sizeof(pending_req)) {
            return -1;
        }

        xSemaphoreTake(rx_sem, 0);
        memcpy(pending_req, req, (size_t)req_len);
        pending_req_len = (uint16_t)req_len;

        portENTER_CRITICAL(&mux);
        write_idx = 0;
        read_idx = 0;
        connected = false;
        connect_failed = false;
        peer_closed = false;
        wnd_throttled = false;
        last_err = ERR_OK;
        portEXIT_CRITICAL(&mux);

        ip_addr_t dst_ip;
        IP_ADDR4(&dst_ip, remote_ip[0], remote_ip[1], remote_ip[2], remote_ip[3]);

        LOCK_TCPIP_CORE();
        struct tcp_pcb *npcb = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (!npcb) {
            UNLOCK_TCPIP_CORE();
            return -1;
        }
        tcp_arg(npcb, this);
        tcp_recv(npcb, on_recv_cb);
        tcp_err(npcb, on_err_cb);
        tcp_nagle_disable(npcb);
        ip_set_option(npcb, SOF_KEEPALIVE);
        npcb->keep_idle = 5000;
        npcb->keep_intvl = 2000;
        npcb->keep_cnt = 4;
        pcb = npcb;
        err_t ce = tcp_connect(npcb, &dst_ip, port, on_connected_cb);
        if (ce != ERR_OK) {
            pcb = NULL;
            tcp_arg(npcb, NULL);
            tcp_recv(npcb, NULL);
            tcp_err(npcb, NULL);
            tcp_abort(npcb);
            UNLOCK_TCPIP_CORE();
            return -1;
        }
        UNLOCK_TCPIP_CORE();

        uint32_t t0 = millis();
        while (millis() - t0 < timeout_ms) {
            if (radio_should_abort_stream_open()) {
                close_raw();
                return -2;
            }
            bool is_conn = false;
            bool is_fail = false;
            portENTER_CRITICAL(&mux);
            is_conn = connected;
            is_fail = connect_failed;
            portEXIT_CRITICAL(&mux);
            if (is_conn) return 0;
            if (is_fail) {
                close_raw();
                return -1;
            }
            uint32_t rem = timeout_ms - (millis() - t0);
            if (rem > 60U) rem = 60U;
            TickType_t ticks = pdMS_TO_TICKS(rem);
            if (ticks == 0) ticks = 1;
            xSemaphoreTake(rx_sem, ticks);
        }
        close_raw();
        return -1;
    }

    bool is_open() {
        portENTER_CRITICAL(&mux);
        uint32_t av = (write_idx - read_idx) & RING_MASK;
        bool open_now = (av > 0) || (!peer_closed && connected);
        portEXIT_CRITICAL(&mux);
        return open_now;
    }

    bool wait_readable(uint32_t timeout_ms) {
        if (avail_bytes() > 0 || peer_closed) return true;
        if (timeout_ms == 0) return false;
        uint32_t t0 = millis();
        while (avail_bytes() == 0 && !peer_closed) {
            uint32_t elapsed = millis() - t0;
            if (elapsed >= timeout_ms) break;
            uint32_t rem = timeout_ms - elapsed;
            TickType_t ticks = pdMS_TO_TICKS(rem);
            if (ticks == 0) ticks = 1;
            xSemaphoreTake(rx_sem, ticks);
        }
        return (avail_bytes() > 0) || peer_closed;
    }

    int read_some(uint8_t *dst, size_t max_len) {
        if (!dst || max_len == 0) return 0;
        portENTER_CRITICAL(&mux);
        uint32_t avail = (write_idx - read_idx) & RING_MASK;
        uint32_t r = read_idx;
        bool closed_now = peer_closed;
        portEXIT_CRITICAL(&mux);

        if (avail == 0) {
            return closed_now ? -1 : 0;
        }

        uint32_t to_copy = ((uint32_t)max_len < avail) ? (uint32_t)max_len : avail;
        uint32_t first = RING_SIZE - r;
        if (first >= to_copy) {
            memcpy(dst, ring + r, to_copy);
        } else {
            memcpy(dst, ring + r, first);
            memcpy(dst + first, ring, to_copy - first);
        }

        bool need_wnd_update = false;
        uint32_t rem_space = 0;
        portENTER_CRITICAL(&mux);
        read_idx = (r + to_copy) & RING_MASK;
        uint32_t new_avail = (write_idx - read_idx) & RING_MASK;
        rem_space = (RING_SIZE - 1U) - new_avail;
        need_wnd_update = wnd_throttled && !peer_closed && (rem_space >= 1436U);
        portEXIT_CRITICAL(&mux);

        if (need_wnd_update) {
            LOCK_TCPIP_CORE();
            if (pcb) {
                if (pcb->refused_data != NULL) {
                    tcp_process_refused_data(pcb);
                }
                if (pcb) {
                    uint32_t sp = free_bytes();
                    tcpwnd_size_t target_wnd = (sp >= ICY_TCP_WND) ? (tcpwnd_size_t)ICY_TCP_WND : (tcpwnd_size_t)sp;
                    if (target_wnd > pcb->rcv_wnd) {
                        pcb->rcv_wnd = target_wnd;
                    }
                    u32_t inf = tcp_update_rcv_ann_wnd(pcb);
                    if (inf >= 1436U) {
                        tcp_clear_flags(pcb, TF_ACK_DELAY);
                        tcp_ack_now(pcb);
                        tcp_output(pcb);
                    }
                    portENTER_CRITICAL(&mux);
                    wnd_throttled = (target_wnd < ICY_TCP_WND);
                    portEXIT_CRITICAL(&mux);
                }
            }
            UNLOCK_TCPIP_CORE();
        }

        return (int)to_copy;
    }
};

static IcyRawTcp s_raw_tcp;

struct IcyStreamHandle {
    bool use_raw_tcp = false;
    WiFiClientSecure *tls_client = NULL;
    HTTPClient *tls_http = NULL;
    uint8_t preread[4096];
    int preread_pos = 0;
    int preread_len = 0;
    bool eof = false;

    void close() {
        if (tls_http) {
            tls_http->end();
            delete tls_http;
            tls_http = NULL;
        }
        if (tls_client) {
            delete tls_client;
            tls_client = NULL;
        }
        if (use_raw_tcp) {
            s_raw_tcp.close_raw();
            use_raw_tcp = false;
        }
        preread_pos = 0;
        preread_len = 0;
        eof = true;
    }

    bool is_open() const {
        if (eof) return false;
        if (preread_pos < preread_len) return true;
        if (tls_http) return tls_http->connected();
        if (use_raw_tcp) return s_raw_tcp.is_open();
        return false;
    }

    int queued_bytes() const {
        int pre = (preread_pos < preread_len) ? (preread_len - preread_pos) : 0;
        if (use_raw_tcp) {
            return pre + (int)s_raw_tcp.avail_bytes();
        }
        if (tls_http && tls_http->getStreamPtr()) {
            return pre + tls_http->getStreamPtr()->available();
        }
        return pre;
    }

    bool wait_readable(uint32_t timeout_ms) const {
        if (preread_pos < preread_len) return true;
        if (tls_http) {
            WiFiClient *st = tls_http->getStreamPtr();
            if (st && st->available() > 0) return true;
            int tfd = st ? st->fd() : -1;
            if (tfd < 0) {
                if (timeout_ms > 0) vTaskDelay(pdMS_TO_TICKS(timeout_ms));
                return false;
            }
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(tfd, &rfds);
            struct timeval tv = {
                .tv_sec = (time_t)(timeout_ms / 1000U),
                .tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U),
            };
            return lwip_select(tfd + 1, &rfds, NULL, NULL, &tv) > 0;
        }
        if (use_raw_tcp) {
            return s_raw_tcp.wait_readable(timeout_ms);
        }
        return false;
    }

    // Non-blocking read: returns >0 bytes, 0 if EWOULDBLOCK, or -1 on EOF/error
    int read_some(uint8_t *dst, size_t max_len) {
        if (!dst || max_len == 0) return 0;
        if (preread_pos < preread_len) {
            size_t avail = (size_t)(preread_len - preread_pos);
            size_t n = (avail < max_len) ? avail : max_len;
            memcpy(dst, preread + preread_pos, n);
            preread_pos += (int)n;
            if (preread_pos >= preread_len) {
                preread_pos = 0;
                preread_len = 0;
            }
            return (int)n;
        }
        if (tls_http) {
            WiFiClient *st = tls_http->getStreamPtr();
            if (!st) {
                eof = true;
                return -1;
            }
            int av = st->available();
            if (av <= 0) {
                if (!tls_http->connected()) {
                    eof = true;
                    return -1;
                }
                return 0;
            }
            size_t to_r = ((size_t)av < max_len) ? (size_t)av : max_len;
            int n = st->read(dst, to_r);
            return (n > 0) ? n : 0;
        }
        if (!use_raw_tcp) {
            eof = true;
            return -1;
        }
        int n = s_raw_tcp.read_some(dst, max_len);
        if (n < 0) {
            eof = true;
            return -1;
        }
        return n;
    }

    int read_exact(uint8_t *dst, int want_len, uint32_t timeout_ms) {
        int got = 0;
        uint32_t t0 = millis();
        while (got < want_len) {
            int n = read_some(dst + got, (size_t)(want_len - got));
            if (n > 0) {
                got += n;
                continue;
            }
            if (n < 0) return got > 0 ? got : -1;
            uint32_t elapsed = millis() - t0;
            if (elapsed >= timeout_ms || radio_should_abort_stream_open()) break;
            uint32_t wait_slice = timeout_ms - elapsed;
            if (wait_slice > 40U) wait_slice = 40U;
            wait_readable(wait_slice);
        }
        return got;
    }
};

struct IcyDnsCacheEntry {
    char host[96];
    uint32_t ip_u32;
    uint32_t resolved_ms;
};
static IcyDnsCacheEntry s_icy_dns_cache[8] = {};
static int s_icy_dns_next_slot = 0;

static bool resolve_icy_host_cached(const char *host, IPAddress *out_ip) {
    uint32_t now = millis();
    for (int i = 0; i < 8; i++) {
        if (s_icy_dns_cache[i].ip_u32 != 0 &&
            strcasecmp(s_icy_dns_cache[i].host, host) == 0 &&
            (now - s_icy_dns_cache[i].resolved_ms) < 600000U) {
            *out_ip = IPAddress(s_icy_dns_cache[i].ip_u32);
            return true;
        }
    }
    if (!WiFi.hostByName(host, *out_ip)) {
        return false;
    }
    int slot = -1;
    for (int i = 0; i < 8; i++) {
        if (s_icy_dns_cache[i].ip_u32 == 0 || strcasecmp(s_icy_dns_cache[i].host, host) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = s_icy_dns_next_slot;
        s_icy_dns_next_slot = (s_icy_dns_next_slot + 1) & 7;
    }
    strncpy(s_icy_dns_cache[slot].host, host, sizeof(s_icy_dns_cache[slot].host) - 1);
    s_icy_dns_cache[slot].host[sizeof(s_icy_dns_cache[slot].host) - 1] = '\0';
    s_icy_dns_cache[slot].ip_u32 = (uint32_t)(*out_ip);
    s_icy_dns_cache[slot].resolved_ms = millis();
    return true;
}

static void invalidate_icy_host_cache(const char *host) {
    for (int i = 0; i < 8; i++) {
        if (s_icy_dns_cache[i].ip_u32 != 0 && strcasecmp(s_icy_dns_cache[i].host, host) == 0) {
            s_icy_dns_cache[i].ip_u32 = 0;
        }
    }
}

static int open_icy_stream(const char *initial_url, IcyStreamHandle *out,
                           int *out_metaint, int *out_br, char *out_icy_name, size_t icy_name_sz) {
    out->close();
    out->eof = false;
    *out_metaint = 0;
    *out_br = 128;
    if (out_icy_name && icy_name_sz > 0) out_icy_name[0] = '\0';

    char cur_url[200] = {0};
    strncpy(cur_url, initial_url, sizeof(cur_url) - 1);

    for (int hop = 0; hop < 4; hop++) {
        if (radio_should_abort_stream_open()) return -2;

        if (strncmp(cur_url, "https://", 8) == 0) {
            out->tls_client = new WiFiClientSecure();
            out->tls_client->setInsecure();
            out->tls_http = new HTTPClient();
            out->tls_http->setUserAgent("VLC/3.0.20");
            out->tls_http->setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
            out->tls_http->setConnectTimeout(4500);
            out->tls_http->setTimeout(4500);
            const char *header_keys[] = {"icy-metaint", "icy-name", "icy-br"};
            out->tls_http->collectHeaders(header_keys, 3);
            if (!out->tls_http->begin(*out->tls_client, cur_url)) {
                out->close();
                return -1;
            }
            out->tls_http->addHeader("Icy-MetaData", "1");
            int code = out->tls_http->GET();
            if (code != HTTP_CODE_OK) {
                out->close();
                return code;
            }
            if (out->tls_http->hasHeader("icy-metaint")) {
                *out_metaint = out->tls_http->header("icy-metaint").toInt();
            }
            if (out->tls_http->hasHeader("icy-br")) {
                *out_br = out->tls_http->header("icy-br").toInt();
            }
            if (out_icy_name && icy_name_sz > 0) {
                String nm = out->tls_http->header("icy-name");
                strncpy(out_icy_name, nm.c_str(), icy_name_sz - 1);
                out_icy_name[icy_name_sz - 1] = '\0';
            }
            out->eof = false;
            return HTTP_CODE_OK;
        }

        if (strncmp(cur_url, "http://", 7) != 0) return -1;
        const char *p = cur_url + 7;
        const char *slash = strchr(p, '/');
        size_t hostport_len = slash ? (size_t)(slash - p) : strlen(p);
        if (hostport_len == 0 || hostport_len >= 112) return -1;

        char hostport[112] = {0};
        memcpy(hostport, p, hostport_len);
        hostport[hostport_len] = '\0';

        char host[96] = {0};
        uint16_t port = 80;
        char *colon = strchr(hostport, ':');
        if (colon) {
            *colon = '\0';
            int parsed_port = atoi(colon + 1);
            if (parsed_port > 0 && parsed_port <= 65535) port = (uint16_t)parsed_port;
        }
        strncpy(host, hostport, sizeof(host) - 1);

        const char *req_path = (slash && slash[0]) ? slash : "/";

        IPAddress remote_ip;
        if (!resolve_icy_host_cached(host, &remote_ip)) {
            return -1;
        }

        // Send HTTP/1.0 GET so Icecast/Shoutcast/CDN servers always stream raw unchunked MP3 + ICY frames
        char req_buf[384];
        int req_len = (port == 80)
            ? snprintf(req_buf, sizeof(req_buf),
                       "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: VLC/3.0.20\r\nIcy-MetaData: 1\r\nAccept: */*\r\nConnection: close\r\n\r\n",
                       req_path, host)
            : snprintf(req_buf, sizeof(req_buf),
                       "GET %s HTTP/1.0\r\nHost: %s:%u\r\nUser-Agent: VLC/3.0.20\r\nIcy-MetaData: 1\r\nAccept: */*\r\nConnection: close\r\n\r\n",
                       req_path, host, (unsigned)port);

        // 4500ms connect budget (> lwIP's 3000ms CONFIG_LWIP_TCP_RTO_TIME SYN retransmit timer)
        int cr = s_raw_tcp.connect_and_send(remote_ip, port, req_buf, req_len, 4500U);
        if (cr < 0) {
            if (cr == -1) invalidate_icy_host_cache(host);
            return cr;
        }

        // Read response headers directly into out->preread (up to 4095 bytes)
        int hdr_total = 0;
        int hdr_end_pos = -1;
        int body_start_pos = -1;
        uint32_t t_hdr = millis();
        while (hdr_total < (int)sizeof(out->preread) - 1 && (millis() - t_hdr < 4500U)) {
            if (radio_should_abort_stream_open()) {
                s_raw_tcp.close_raw();
                return -2;
            }
            int n = s_raw_tcp.read_some(out->preread + hdr_total, (size_t)((int)sizeof(out->preread) - 1 - hdr_total));
            if (n > 0) {
                hdr_total += n;
                out->preread[hdr_total] = '\0';
                const char *crlf2 = strstr((const char *)out->preread, "\r\n\r\n");
                if (crlf2) {
                    hdr_end_pos = (int)(crlf2 - (const char *)out->preread);
                    body_start_pos = hdr_end_pos + 4;
                    break;
                }
                const char *lf2 = strstr((const char *)out->preread, "\n\n");
                if (lf2) {
                    hdr_end_pos = (int)(lf2 - (const char *)out->preread);
                    body_start_pos = hdr_end_pos + 2;
                    break;
                }
                continue;
            }
            if (n < 0) break;
            s_raw_tcp.wait_readable(60U);
        }

        if (hdr_end_pos <= 0 || body_start_pos < 0) {
            s_raw_tcp.close_raw();
            return -1;
        }

        // Temporarily NUL-terminate header block to parse status & ICY headers cleanly
        uint8_t saved_byte = out->preread[hdr_end_pos];
        out->preread[hdr_end_pos] = '\0';

        int status_code = -1;
        const char *sp1 = strchr((const char *)out->preread, ' ');
        if (sp1) {
            status_code = atoi(sp1 + 1);
        }

        char location_url[200] = {0};
        const char *line = (const char *)out->preread;
        while (line && *line) {
            const char *next_nl = strchr(line, '\n');
            size_t line_len = next_nl ? (size_t)(next_nl - line) : strlen(line);
            if (line_len > 0 && line[line_len - 1] == '\r') line_len--;

            const char *colon_pos = (const char *)memchr(line, ':', line_len);
            if (colon_pos) {
                size_t klen = (size_t)(colon_pos - line);
                const char *val = colon_pos + 1;
                size_t vlen = line_len - klen - 1;
                while (vlen > 0 && (*val == ' ' || *val == '\t')) {
                    val++;
                    vlen--;
                }
                if (klen == 8 && strncasecmp(line, "location", 8) == 0 && vlen > 0) {
                    size_t cp = (vlen < sizeof(location_url) - 1) ? vlen : (sizeof(location_url) - 1);
                    memcpy(location_url, val, cp);
                    location_url[cp] = '\0';
                } else if (klen == 11 && strncasecmp(line, "icy-metaint", 11) == 0 && vlen > 0) {
                    *out_metaint = atoi(val);
                } else if (klen == 6 && strncasecmp(line, "icy-br", 6) == 0 && vlen > 0) {
                    *out_br = atoi(val);
                } else if (klen == 8 && strncasecmp(line, "icy-name", 8) == 0 && vlen > 0 && out_icy_name && icy_name_sz > 0) {
                    size_t cp = (vlen < icy_name_sz - 1) ? vlen : (icy_name_sz - 1);
                    memcpy(out_icy_name, val, cp);
                    out_icy_name[cp] = '\0';
                }
            }
            line = next_nl ? (next_nl + 1) : NULL;
        }

        out->preread[hdr_end_pos] = saved_byte;

        if ((status_code == 301 || status_code == 302 || status_code == 303 ||
             status_code == 307 || status_code == 308) && location_url[0] != '\0') {
            s_raw_tcp.close_raw();
            if (location_url[0] == '/') {
                if (port == 80) {
                    snprintf(cur_url, sizeof(cur_url), "http://%s%s", host, location_url);
                } else {
                    snprintf(cur_url, sizeof(cur_url), "http://%s:%u%s", host, (unsigned)port, location_url);
                }
            } else {
                strncpy(cur_url, location_url, sizeof(cur_url) - 1);
                cur_url[sizeof(cur_url) - 1] = '\0';
            }
            continue;
        }

        if (status_code != 200) {
            s_raw_tcp.close_raw();
            return status_code > 0 ? status_code : -1;
        }

        out->use_raw_tcp = true;
        out->eof = false;
        int body_rem = hdr_total - body_start_pos;
        if (body_rem > 0) {
            memmove(out->preread, out->preread + body_start_pos, (size_t)body_rem);
            out->preread_pos = 0;
            out->preread_len = body_rem;
        } else {
            out->preread_pos = 0;
            out->preread_len = 0;
        }
        return HTTP_CODE_OK;
    }

    return -1;
}

// Task 1: HTTP Icecast reader + MP3 decoder -> 512 KiB PSRAM PCM ring
static void http_decoder_task(void *arg) {
    // Allocate MP3 decoder state & frame buffers in Octal PSRAM (stays hot in 32KB L1 DCache at 38 frames/sec)
    // to preserve contiguous Internal DMA SRAM for Wi-Fi TX/RX buffers during AirPlay 2 streaming.
    mp3dec_t *mp3d = (mp3dec_t *)heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mp3d) mp3d = (mp3dec_t *)heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *pcm_out = (int16_t *)heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm_out) pcm_out = (int16_t *)heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *stereo_tmp = (int16_t *)heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *mp3_buf = (uint8_t *)heap_caps_malloc(MP3_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *meta_buf = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    int mirror_idx = 0;
    int active_preset = -1;
    uint8_t ap_stall_strikes = 0;
    uint32_t last_ap_stall_ms = 0;
    // Ordered by fastest measured TTFB (ice4: 212ms, ice2: 222ms, ice6: 230ms, ice1: 257ms)
    const char *somafm_mirrors[] = {"ice4.somafm.com", "ice2.somafm.com", "ice6.somafm.com", "ice1.somafm.com"};

    for (;;) {
        bool want_play = false;
        bool wifi_ok = false;
        bool reconfig = false;
        bool native_without_monitor = false;
        int cur_preset = -1;
        char target_url[128] = {0};
        char station_name[40] = {0};

        bool boot_mdns_busy = false;
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            want_play = g_app.playing && !g_app.ota_in_progress;
            native_without_monitor = (g_app.sonos_native || g_app.sonos_radio_selected) &&
                                     !g_app.also_local_dac && !g_app.whitecap_active;
            wifi_ok = g_app.wifi_connected;
            uint32_t since_wifi = (g_app.wifi_connected_since_ms > 0) ? (millis() - g_app.wifi_connected_since_ms) : 0;
            boot_mdns_busy = wifi_ok && (g_app.speaker_scanning || since_wifi < 1400) && (ring_available_frames() == 0);
            reconfig = g_app.stream_reconfig_needed;
            cur_preset = g_app.current_preset;
            if (g_app.current_preset >= 0 && g_app.current_preset < g_app.preset_count) {
                strncpy(target_url, g_app.presets[g_app.current_preset].url, sizeof(target_url) - 1);
                strncpy(station_name, g_app.presets[g_app.current_preset].name, sizeof(station_name) - 1);
            }
            if (!boot_mdns_busy) {
                g_app.stream_reconfig_needed = false;
            }
            xSemaphoreGive(g_app.mutex);
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        if (boot_mdns_busy) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (native_without_monitor) {
            ring_clear();
            active_preset = -1;
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                g_app.mp3_buf_bytes = 0;
                g_app.pcm_buf_ms = 0;
                g_app.pcm_buf_kb = 0;
                memset(g_app.vu_bars, 0, sizeof(g_app.vu_bars));
                g_app.vu_peak = 0;
                xSemaphoreGive(g_app.mutex);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!want_play || !wifi_ok || target_url[0] == '\0') {
            ring_clear();
            active_preset = -1;
            ap_stall_strikes = 0;
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                memset(g_app.vu_bars, 0, sizeof(g_app.vu_bars));
                g_app.vu_peak = 0;
                g_app.mp3_buf_bytes = 0;
                g_app.pcm_buf_ms = 0;
                g_app.pcm_buf_kb = 0;
                if (!want_play) {
                    g_app.status = STREAM_STOPPED;
                    strncpy(g_app.status_text, "Paused", sizeof(g_app.status_text));
                } else if (!wifi_ok) {
                    g_app.status = STREAM_CONNECTING_RADIO;
                    strncpy(g_app.status_text, "Waiting for Wi-Fi...", sizeof(g_app.status_text));
                }
                xSemaphoreGive(g_app.mutex);
            }
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        bool station_changed = reconfig || (cur_preset != active_preset);
        if (station_changed) {
            ring_clear();
            active_preset = cur_preset;
            ap_stall_strikes = 0;
        }

        // Apply mirror selection if URL is a somafm ice server
        char effective_url[140];
        strncpy(effective_url, target_url, sizeof(effective_url) - 1);
        effective_url[sizeof(effective_url) - 1] = '\0';
        char *ice_pos = strstr(effective_url, "ice6.somafm.com");
        if (!ice_pos) ice_pos = strstr(effective_url, "ice1.somafm.com");
        if (!ice_pos) ice_pos = strstr(effective_url, "ice2.somafm.com");
        if (!ice_pos) ice_pos = strstr(effective_url, "ice4.somafm.com");
        if (ice_pos) {
            memcpy(ice_pos, somafm_mirrors[mirror_idx % 4], 15);
        }

        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            g_app.mp3_buf_bytes = 0;
            uint32_t af = ring_available_frames();
            g_app.pcm_buf_ms = (uint32_t)(((uint64_t)af * 1000ULL) / 44100ULL);
            g_app.pcm_buf_kb = (af * 4U) / 1024U;
            if (af < RAOP_CHUNK_FRAMES) {
                g_app.status = STREAM_CONNECTING_RADIO;
                snprintf(g_app.status_text, sizeof(g_app.status_text), "Tuning %s...", station_name);
                snprintf(g_app.track_title, sizeof(g_app.track_title), "Connecting to %s...", station_name);
            }
            xSemaphoreGive(g_app.mutex);
        }

        uint32_t t_open_ms = millis();
        Serial.printf("[RADIO] Opening stream: %s (keep_ring=%d buf_ms=%lu)\n",
                      effective_url, (int)!station_changed, (unsigned long)radio_get_buffered_ms());

        IcyStreamHandle stream_conn;
        int metaint = 0;
        int br = 128;
        char icy_name[64] = {0};
        int code = open_icy_stream(effective_url, &stream_conn, &metaint, &br, icy_name, sizeof(icy_name));
        uint32_t open_dur_ms = millis() - t_open_ms;
        if (code != HTTP_CODE_OK) {
            if (code != -2) {
                Serial.printf("[RADIO] HTTP GET failed (%d) on %s after %lums\n",
                              code, effective_url, (unsigned long)open_dur_ms);
                mirror_idx++;
                if (WiFi.status() == WL_CONNECTED) {
                    uint32_t now_fail = millis();
                    if (last_ap_stall_ms != 0 && (now_fail - last_ap_stall_ms) > 90000U) {
                        ap_stall_strikes = 0;
                    }
                    ap_stall_strikes++;
                    last_ap_stall_ms = now_fail;
                    if (ap_stall_strikes >= 2) {
                        ap_stall_strikes = 0;
                        app_report_wifi_stream_stall();
                    }
                }
                if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    if (ring_available_frames() < RAOP_CHUNK_FRAMES) {
                        g_app.status = STREAM_ERROR;
                        snprintf(g_app.status_text, sizeof(g_app.status_text), "Retrying %s...", station_name);
                    }
                    xSemaphoreGive(g_app.mutex);
                }
                vTaskDelay(pdMS_TO_TICKS(150));
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            stream_conn.close();
            continue;
        }

        Serial.printf("[RADIO] Stream connected in %lums! metaint=%d br=%d name=%s\n",
                      (unsigned long)open_dur_ms, metaint, br, icy_name);

        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            g_app.bitrate_kbps = (br > 0) ? br : 128;
            if (icy_name[0] != '\0') {
                strncpy(g_app.icy_station, icy_name, sizeof(g_app.icy_station) - 1);
            } else {
                strncpy(g_app.icy_station, station_name, sizeof(g_app.icy_station) - 1);
            }
            if (g_app.track_title[0] == '\0' || strncmp(g_app.track_title, "Connecting", 10) == 0) {
                snprintf(g_app.track_title, sizeof(g_app.track_title), "%s - Live Stream", station_name);
            }
            xSemaphoreGive(g_app.mutex);
        }

        mp3dec_init(mp3d);

        int bytes_until_meta = metaint;
        int mp3_fill = 0;
        int mp3_off = 0;
        uint32_t stream_start_ms = millis();
        uint32_t last_data_ms = stream_start_ms;
        uint32_t last_state_check_ms = 0;
        uint32_t last_diag_ms = stream_start_ms;
        uint32_t diag_loops = 0;
        uint32_t diag_rx_bytes = 0;
        uint32_t diag_dec_frames = 0;
        uint32_t rate_window_start_ms = stream_start_ms;
        uint32_t rate_window_rx_bytes = 0;
        bool marked_ap_healthy = false;

        while (stream_conn.is_open()) {
            diag_loops++;
            uint32_t now_ms = millis();
            if (!marked_ap_healthy && (now_ms - stream_start_ms >= 20000U) &&
                (mp3_fill - mp3_off) >= 131072 && radio_get_buffered_ms() >= 2400) {
                marked_ap_healthy = true;
                ap_stall_strikes = 0;
                app_mark_wifi_stream_healthy();
            }
            if (now_ms - last_diag_ms >= 1000) {
                int rx_ring = stream_conn.queued_bytes();
                if (!g_screenshot_dump_active) {
                    Serial.printf("[HTTP-1S] loops=%lu rxB=%lu decF=%lu rxRing=%d mp3Rem=%d bufMs=%lu raop=%d\n",
                                  (unsigned long)diag_loops,
                                  (unsigned long)diag_rx_bytes,
                                  (unsigned long)diag_dec_frames,
                                  rx_ring,
                                  (mp3_fill - mp3_off),
                                  (unsigned long)radio_get_buffered_ms(),
                                  (int)g_app.raop_connected);
                }
                diag_loops = 0;
                diag_rx_bytes = 0;
                diag_dec_frames = 0;
                last_diag_ms = now_ms;
            }
            if (now_ms - last_state_check_ms >= 20) {
                last_state_check_ms = now_ms;
                bool keep_going = true;
                if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                    if (!g_app.playing || !g_app.wifi_connected || g_app.stream_reconfig_needed || g_app.ota_in_progress ||
                        ((g_app.sonos_native || g_app.sonos_radio_selected) &&
                         !g_app.also_local_dac && !g_app.whitecap_active)) {
                        keep_going = false;
                    }
                    g_app.mp3_buf_bytes = (uint32_t)(mp3_fill - mp3_off);
                    uint32_t af = ring_available_frames();
                    g_app.pcm_buf_ms = (uint32_t)(((uint64_t)af * 1000ULL) / 44100ULL);
                    g_app.pcm_buf_kb = (af * 4U) / 1024U;
                    xSemaphoreGive(g_app.mutex);
                }
                if (!keep_going) break;
            }

            // Keep compressed input buffered when both PCM ring and 256 KB MP3 reserve are full
            if (ring_free_frames() < 4608 && (mp3_fill - mp3_off) > (int)(MP3_BUF_SIZE - 8192)) {
                bool waiting_airplay = false;
                if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
                    bool is_loc = (g_app.current_speaker >= 0 && g_app.current_speaker < g_app.speaker_count)
                                      ? g_app.speakers[g_app.current_speaker].is_local_dac
                                      : true;
                    waiting_airplay = !is_loc && !g_app.raop_connected;
                    xSemaphoreGive(g_app.mutex);
                }
                if (waiting_airplay && stream_conn.wait_readable(0)) {
                    // Slide live window forward while waiting for AirPlay speaker handshake
                    // so Icecast never drops our TCP connection due to prolonged ZeroWindow!
                    int16_t discard_pcm[RAOP_CHUNK_FRAMES * 2];
                    for (int d = 0; d < 4; d++) {
                        ring_pop_352(discard_pcm);
                    }
                } else {
                    rate_window_start_ms = millis();
                    rate_window_rx_bytes = 0;
                    vTaskDelay(pdMS_TO_TICKS(8));
                    continue;
                }
            }

            // Compact sliding MP3 buffer when needed
            if (mp3_off > 0 && (mp3_off >= 32768 || ((int)MP3_BUF_SIZE - mp3_fill) < 16384)) {
                int rem = mp3_fill - mp3_off;
                if (rem > 0) {
                    memmove(mp3_buf, mp3_buf + mp3_off, rem);
                }
                mp3_fill = rem;
                mp3_off = 0;
            }

            // Drain available TCP segments directly from the socket into the 256 KB PSRAM mp3_buf
            // without 1436B wrapper copies or 1000 Hz MSG_PEEK/FIONREAD lwIP core-lock contention.
            int total_read_this_turn = 0;
            bool stream_eof = false;
            while (total_read_this_turn < 65536) {
                int space = (int)MP3_BUF_SIZE - mp3_fill;
                if (space < 8192 && mp3_off > 0) {
                    int rem = mp3_fill - mp3_off;
                    if (rem > 0) memmove(mp3_buf, mp3_buf + mp3_off, rem);
                    mp3_fill = rem;
                    mp3_off = 0;
                    space = (int)MP3_BUF_SIZE - mp3_fill;
                }
                if (space <= 512) {
                    break; // MP3 buffer is full; decode frames below first
                }

                int to_read = (space > 16384) ? 16384 : space;
                if (metaint > 0 && to_read > bytes_until_meta) {
                    to_read = bytes_until_meta;
                }

                int n = stream_conn.read_some(mp3_buf + mp3_fill, (size_t)to_read);
                if (n == 0) break; // EWOULDBLOCK - socket recvmbox completely drained!
                if (n < 0) {
                    stream_eof = true;
                    break;
                }

                last_data_ms = millis();
                mp3_fill += n;
                total_read_this_turn += n;
                diag_rx_bytes += (uint32_t)n;
                rate_window_rx_bytes += (uint32_t)n;

                if (metaint > 0) {
                    bytes_until_meta -= n;
                    if (bytes_until_meta == 0) {
                        uint8_t len_byte = 0;
                        int lr = stream_conn.read_exact(&len_byte, 1, 2000);
                        if (lr < 1) {
                            stream_eof = true;
                            break;
                        }
                        if (len_byte > 0) {
                            int mlen = (int)len_byte * 16;
                            int mread = stream_conn.read_exact((uint8_t *)meta_buf, mlen, 2500);
                            if (mread < mlen) {
                                stream_eof = true;
                                break;
                            }
                            meta_buf[mread] = '\0';
                            parse_icy_metadata(meta_buf, mread);
                        }
                        bytes_until_meta = metaint;
                    }
                }
            }

            if (stream_eof) {
                Serial.printf("[RADIO] Stream closed by peer (mp3_rem=%d, buf_ms=%lu), reconnecting...\n",
                              (mp3_fill - mp3_off), (unsigned long)radio_get_buffered_ms());
                mirror_idx++;
                break;
            }

            // Watchdog 1: Complete network stall (5.5s with zero bytes while reserve is low)
            // Watchdog 2: Sustained TCP trickle starvation (< 50% of bitrate over 8s while buffers are low)
            bool degraded_trickle = false;
            uint32_t rate_elapsed_ms = millis() - rate_window_start_ms;
            if (rate_elapsed_ms >= 8000U) {
                uint32_t expected_8s = (uint32_t)((br > 0 ? br : 128) * 1000U); // bytes in 8s = kbps * 1000
                if ((millis() - stream_start_ms >= 9000U) &&
                    rate_window_rx_bytes < (expected_8s / 2U) &&
                    (mp3_fill - mp3_off) < 16384 &&
                    ring_available_frames() < AudioBuffer::frames_for_ms(1200)) {
                    degraded_trickle = true;
                    Serial.printf("[RADIO] Degraded TCP throughput (%lu B in %lums, mp3_rem=%d, buf_ms=%lu), rotating mirror...\n",
                                  (unsigned long)rate_window_rx_bytes, (unsigned long)rate_elapsed_ms,
                                  (mp3_fill - mp3_off), (unsigned long)radio_get_buffered_ms());
                }
                rate_window_start_ms = millis();
                rate_window_rx_bytes = 0;
            }

            if (degraded_trickle ||
                (total_read_this_turn == 0 && (mp3_fill - mp3_off) < 65536 && (millis() - last_data_ms > 5500))) {
                if (!degraded_trickle) {
                    Serial.printf("[RADIO] Stream read timeout (5.5s, mp3_rem=%d, buf_ms=%lu), rotating mirror...\n",
                                  (mp3_fill - mp3_off), (unsigned long)radio_get_buffered_ms());
                }
                if (WiFi.status() == WL_CONNECTED) {
                    uint32_t now_stall = millis();
                    if (last_ap_stall_ms != 0 && (now_stall - last_ap_stall_ms) > 90000U) {
                        ap_stall_strikes = 0;
                    }
                    uint8_t add_strikes = (open_dur_ms >= 2800U && radio_get_buffered_ms() < 1500U) ? 2 : 1;
                    ap_stall_strikes = (uint8_t)(ap_stall_strikes + add_strikes);
                    last_ap_stall_ms = now_stall;
                    if (ap_stall_strikes >= 2) {
                        ap_stall_strikes = 0;
                        app_report_wifi_stream_stall();
                    }
                }
                // Decode any remaining MP3 frames that fit into the PCM ringbuffer before reconnecting
                while ((mp3_fill - mp3_off) >= 640 && ring_free_frames() >= 1152) {
                    mp3dec_frame_info_t info = {};
                    int samples = mp3dec_decode_frame(mp3d, mp3_buf + mp3_off, mp3_fill - mp3_off, pcm_out, &info);
                    if (info.frame_bytes == 0) break;
                    if (samples > 0) {
                        if (info.channels == 2) {
                            ring_push_frames(pcm_out, (uint32_t)samples);
                        } else if (info.channels == 1) {
                            for (int i = 0; i < samples; i++) {
                                stereo_tmp[i * 2]     = pcm_out[i];
                                stereo_tmp[i * 2 + 1] = pcm_out[i];
                            }
                            ring_push_frames(stereo_tmp, (uint32_t)samples);
                        }
                    }
                    mp3_off += info.frame_bytes;
                }
                mirror_idx++;
                break;
            }

            if (total_read_this_turn == 0 && (mp3_fill - mp3_off) < 640) {
                stream_conn.wait_readable(12);
                continue;
            }

            if (ring_free_frames() < 1152) {
                if (total_read_this_turn == 0) {
                    if (((int)MP3_BUF_SIZE - (mp3_fill - mp3_off)) >= 4096) {
                        stream_conn.wait_readable(10);
                    } else {
                        vTaskDelay(pdMS_TO_TICKS(6));
                    }
                }
                continue;
            }

            // Decode MP3 frames into PCM ringbuffer, yielding immediately if new TCP segments arrive
            // while mp3_buf still has plenty of room so lwIP's 6-slot recvmbox stays wide open.
            int decoded_batches = 0;
            while ((mp3_fill - mp3_off) >= 640 && ring_free_frames() >= 1152 && decoded_batches < 8) {
                mp3dec_frame_info_t info = {};
                int samples = mp3dec_decode_frame(mp3d, mp3_buf + mp3_off, mp3_fill - mp3_off, pcm_out, &info);
                if (info.frame_bytes == 0) {
                    break; // need more MP3 bytes
                }
                if (samples > 0) {
                    if (info.channels == 2) {
                        ring_push_frames(pcm_out, (uint32_t)samples);
                    } else if (info.channels == 1) {
                        for (int i = 0; i < samples; i++) {
                            stereo_tmp[i * 2]     = pcm_out[i];
                            stereo_tmp[i * 2 + 1] = pcm_out[i];
                        }
                        ring_push_frames(stereo_tmp, (uint32_t)samples);
                    }
                }
                mp3_off += info.frame_bytes;
                decoded_batches++;
                diag_dec_frames++;
                if ((decoded_batches & 1) == 0 &&
                    ((int)MP3_BUF_SIZE - (mp3_fill - mp3_off)) >= 16384 &&
                    stream_conn.wait_readable(0)) {
                    break; // Immediately drain incoming TCP segments from lwIP!
                }
            }

            if (decoded_batches == 0 && total_read_this_turn == 0) {
                stream_conn.wait_readable(12);
            }
        }

        stream_conn.close();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// Task 2: Real-time 44.1kHz RAOP AirPlay + Local DAC sender
static void raop_sender_task(void *arg) {
    void *raop_mem = heap_caps_malloc(sizeof(RaopClient), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    RaopClient *raop = raop_mem ? new (raop_mem) RaopClient() : new RaopClient();
    SonosRadioClient sonos;
    char native_url[128] = {0};
    char native_rejected_ip[20] = {0};
    char active_speaker_ip[20] = {0};
    bool active_is_local_dac = true;
    bool speaker_initialized = false;
    int connect_failures = 0;
    char last_sent_title[128] = {0};
    uint32_t last_sent_art_seq = 0;
    uint8_t *art_jpg_buf = (uint8_t *)heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *chunk_pcm = (int16_t *)heap_caps_calloc(RAOP_CHUNK_FRAMES * 2, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint64_t next_send_us = esp_timer_get_time();
    bool prebuffering = true;

    bool have_snapshot = false;
    uint64_t last_snapshot_us = 0;
    bool want_play = false;
    bool wifi_ok = false;
    int spk_idx = 0;
    int vol = 65;
    bool also_local = false;
    bool want_visualizer = false;
    bool prefer_sonos_native = false;
    bool force_pin_reconnect = false;
    bool pin_halted = false;
    bool radio_connecting = false;
    uint32_t mp3_bytes_buffered = 0;
    AirPlaySpeaker spk = {};
    char cur_title[128] = {0};
    char cur_station[40] = {0};
    char cur_url[128] = {0};

    for (;;) {
        bool vol_dirty = false;
        uint64_t snap_now_us = esp_timer_get_time();
        bool need_snapshot = !have_snapshot || prebuffering || !raop->isConnected() ||
                             g_app.volume_dirty || g_app.stream_reconfig_needed ||
                             (snap_now_us - last_snapshot_us >= 20000ULL);
        if (need_snapshot) {
            TickType_t lock_wait = have_snapshot ? 0 : pdMS_TO_TICKS(10);
            if (xSemaphoreTake(g_app.mutex, lock_wait) == pdTRUE) {
                have_snapshot = true;
                last_snapshot_us = snap_now_us;
                pin_halted = false;
                want_play = g_app.playing && !g_app.ota_in_progress;
                wifi_ok = g_app.wifi_connected;
                spk_idx = g_app.current_speaker;
                vol = g_app.volume;
                vol_dirty = g_app.volume_dirty;
                g_app.volume_dirty = false;
                also_local = g_app.also_local_dac;
                want_visualizer = g_app.whitecap_active;
                prefer_sonos_native = g_app.prefer_sonos_native;
                force_pin_reconnect = g_app.pin_manual_request;
                radio_connecting = (g_app.status == STREAM_CONNECTING_RADIO || g_app.stream_reconfig_needed);
                mp3_bytes_buffered = g_app.mp3_buf_bytes;
                memset(&spk, 0, sizeof(spk));
                if (spk_idx >= 0 && spk_idx < g_app.speaker_count) {
                    spk = g_app.speakers[spk_idx];
                } else {
                    spk.is_local_dac = true;
                    strncpy(spk.name, "Local 3.5mm Jack", sizeof(spk.name) - 1);
                }
                if (spk.is_local_dac && force_pin_reconnect) {
                    g_app.pin_manual_request = false;
                    force_pin_reconnect = false;
                }
                if (!spk.is_local_dac && spk.ip[0] != '\0' && g_app.pin_halted_ip[0] != '\0' &&
                    strcmp(spk.ip, g_app.pin_halted_ip) == 0 && !force_pin_reconnect) {
                    pin_halted = true;
                }
                strncpy(cur_title, g_app.track_title, sizeof(cur_title) - 1);
                cur_title[sizeof(cur_title) - 1] = '\0';
                if (g_app.current_preset >= 0 && g_app.current_preset < g_app.preset_count) {
                    strncpy(cur_station, g_app.presets[g_app.current_preset].name, sizeof(cur_station) - 1);
                    cur_station[sizeof(cur_station) - 1] = '\0';
                    strncpy(cur_url, g_app.presets[g_app.current_preset].url, sizeof(cur_url) - 1);
                    cur_url[sizeof(cur_url) - 1] = '\0';
                }
                uint32_t af = ring_available_frames();
                g_app.pcm_buf_ms = (uint32_t)(((uint64_t)af * 1000ULL) / 44100ULL);
                g_app.pcm_buf_kb = (af * 4U) / 1024U;
                g_app.raop_connected = raop->isConnected();
                g_app.raop_is_ap2 = raop->isAirPlay2();
                g_app.sonos_native = sonos.isConnected();
                // Stop the duplicate source download before the native handshake,
                // rather than waiting until it succeeds. Otherwise recovery from
                // a congested AirPlay stream keeps competing with local decoding.
                g_app.sonos_radio_selected = prefer_sonos_native && !force_pin_reconnect &&
                                            SonosRadioClient::isCandidate(spk) &&
                                            strcmp(native_rejected_ip, spk.ip) != 0;
                g_app.raop_pkts_sent = raop->getPktsSent();
                g_app.raop_ntp_replies = raop->getNtpReplies();
                g_app.raop_retransmits = raop->getRetransmits();
                g_app.raop_tx_accepted = raop->txStats().accepted;
                g_app.raop_tx_rejected = raop->txStats().rejected;
                g_app.raop_tx_retries = raop->txStats().retries;
                g_app.raop_tx_last_error = raop->txStats().last_error;
                g_app.raop_repair_rejected = raop->repairStats().rejected;
                g_app.raop_repair_pending = raop->pendingRepairs();
                g_app.raop_max_audio_gap_ms = raop->maxAudioGapMs();
                g_app.raop_audio_gap_events = raop->audioGapEvents();
                g_app.raop_max_control_write_ms = raop->maxControlWriteMs();
                xSemaphoreGive(g_app.mutex);
            } else if (!have_snapshot) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
        }

        if (!want_play || !wifi_ok) {
            if (!want_play) sonos.stop();
            if (raop->isConnected()) {
                raop->disconnect();
            }
            speaker_initialized = false;
            prebuffering = true;
            vTaskDelay(pdMS_TO_TICKS(40));
            next_send_us = esp_timer_get_time();
            continue;
        }

        // Compare speaker by IP / is_local_dac so background mDNS scans never trigger false reconnects!
        bool speaker_changed = !speaker_initialized ||
                               (spk.is_local_dac != active_is_local_dac) ||
                               (!spk.is_local_dac && strcmp(spk.ip, active_speaker_ip) != 0);

        if (force_pin_reconnect && SonosRadioClient::isCandidate(spk)) {
            strncpy(native_rejected_ip, spk.ip, sizeof(native_rejected_ip) - 1);
        }
        bool use_sonos = prefer_sonos_native && SonosRadioClient::isCandidate(spk) &&
                         strcmp(native_rejected_ip, spk.ip) != 0;
        if (!use_sonos && sonos.isConnected()) sonos.stop();

        if (use_sonos) {
            if (raop->isConnected()) raop->disconnect();
            bool same_stream = sonos.isConnected() &&
                               strcmp(active_speaker_ip, spk.ip) == 0 && strcmp(native_url, cur_url) == 0;
            if (!same_stream) {
                if (sonos.isConnected() && strcmp(active_speaker_ip, spk.ip) != 0) sonos.stop();
                if (!sonos.start(spk, cur_url, cur_station, vol)) {
                    if (sonos.rejected()) strncpy(native_rejected_ip, spk.ip, sizeof(native_rejected_ip) - 1);
                    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                        g_app.status = STREAM_ERROR;
                        snprintf(g_app.status_text, sizeof(g_app.status_text), "Sonos retry: %s", spk.name);
                        xSemaphoreGive(g_app.mutex);
                    }
                    vTaskDelay(pdMS_TO_TICKS(1200));
                    continue;
                }
                strncpy(native_url, cur_url, sizeof(native_url) - 1);
                prebuffering = true;
                next_send_us = esp_timer_get_time();
            }
            speaker_initialized = true;
            active_is_local_dac = false;
            strncpy(active_speaker_ip, spk.ip, sizeof(active_speaker_ip) - 1);
            if (vol_dirty) sonos.setVolume(vol);
            sonos.poll();
            if (!also_local && !want_visualizer) {
                if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    g_app.status = STREAM_PLAYING;
                    snprintf(g_app.status_text, sizeof(g_app.status_text), "Sonos radio -> %s", spk.name);
                    xSemaphoreGive(g_app.mutex);
                }
                prebuffering = true;
                next_send_us = esp_timer_get_time();
                vTaskDelay(pdMS_TO_TICKS(40));
                continue;
            }
        } else if (pin_halted && !force_pin_reconnect) {
            if (raop->isConnected()) {
                raop->disconnect();
            }
            speaker_initialized = true;
            active_is_local_dac = spk.is_local_dac;
            strncpy(active_speaker_ip, spk.ip, sizeof(active_speaker_ip) - 1);
            prebuffering = true;
            vTaskDelay(pdMS_TO_TICKS(80));
            next_send_us = esp_timer_get_time();
            continue;
        } else if (force_pin_reconnect || speaker_changed || (!spk.is_local_dac && !raop->isConnected())) {
            if (raop->isConnected()) {
                raop->disconnect();
            }
            speaker_initialized = true;
            active_is_local_dac = spk.is_local_dac;
            strncpy(active_speaker_ip, spk.ip, sizeof(active_speaker_ip) - 1);
            last_sent_title[0] = '\0';
            last_sent_art_seq = 0;
            prebuffering = true;

            if (!spk.is_local_dac) {
                if (spk.ip[0] == '\0') {
                    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                        g_app.status = STREAM_CONNECTING_AIRPLAY;
                        snprintf(g_app.status_text, sizeof(g_app.status_text), "Finding %s...", spk.name);
                        xSemaphoreGive(g_app.mutex);
                    }
                    speaker_initialized = false;
                    vTaskDelay(pdMS_TO_TICKS(150));
                    next_send_us = esp_timer_get_time();
                    continue;
                }
                // Complete initial discovery and collect 1s PCM (or 0.5s plus
                // compressed reserve) before the blocking pairing handshake.
                // Thresholds must remain below the 2.97s PCM capacity.
                uint32_t since_wifi_ms = (g_app.wifi_connected_since_ms > 0)
                                             ? (millis() - g_app.wifi_connected_since_ms)
                                             : 0;
                bool mdns_ready = (g_app.speaker_count >= 10) || (since_wifi_ms >= 3500);
                bool audio_prebuffered = AudioBuffer::ready_to_connect(
                    ring_available_frames(), mp3_bytes_buffered, since_wifi_ms);
                if (!force_pin_reconnect && (!mdns_ready || !audio_prebuffered)) {
                    speaker_initialized = false;
                    vTaskDelay(pdMS_TO_TICKS(100));
                    next_send_us = esp_timer_get_time();
                    continue;
                }
                if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    g_app.status = STREAM_CONNECTING_AIRPLAY;
                    snprintf(g_app.status_text, sizeof(g_app.status_text), "AirPlay -> %s...", spk.name);
                    xSemaphoreGive(g_app.mutex);
                }
                if (!raop->connect(spk, vol)) {
                    connect_failures++;
                    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                        g_app.status = STREAM_ERROR;
                        bool now_halted = (g_app.pin_halted_ip[0] != '\0' && strcmp(spk.ip, g_app.pin_halted_ip) == 0);
                        if (!now_halted) {
                            snprintf(g_app.status_text, sizeof(g_app.status_text), "AirPlay retry: %s", spk.name);
                            if (connect_failures >= 2) {
                                g_app.speaker_scan_requested = true;
                            }
                        }
                        xSemaphoreGive(g_app.mutex);
                    }
                    vTaskDelay(pdMS_TO_TICKS(1200));
                    next_send_us = esp_timer_get_time();
                    continue;
                }
                connect_failures = 0;
            }
            next_send_us = esp_timer_get_time();
        }

        if (vol_dirty && raop->isConnected()) {
            raop->setVolume(vol);
        }

        if (raop->isConnected() && !prebuffering &&
            cur_title[0] != '\0' && strncmp(cur_title, "Connecting", 10) != 0 &&
            strcmp(cur_title, last_sent_title) != 0) {
            strncpy(last_sent_title, cur_title, sizeof(last_sent_title) - 1);
            raop->setMetadata(cur_title, cur_station, "SomaFM Radio");
        }

        uint32_t cur_art_seq = artwork_get_jpeg_seq();
        if (raop->isConnected() && !prebuffering && art_jpg_buf &&
            cur_art_seq != 0 && cur_art_seq != last_sent_art_seq) {
            uint32_t copied_seq = 0;
            size_t jpg_len = artwork_copy_jpeg(art_jpg_buf, 64 * 1024, &copied_seq);
            if (jpg_len > 0) {
                last_sent_art_seq = copied_seq;
                raop->setArtwork(art_jpg_buf, jpg_len);
            }
        }

        raop->pollControlAndKeepalive(true);

        // Start/rebuffer at 1s PCM with compressed reserve, otherwise 1.5s.
        // Keep the longer network-jitter cushion in the compressed MP3 buffer.
        uint32_t avail = ring_available_frames();
        if (prebuffering) {
            uint32_t min_start_frames = AudioBuffer::playback_start_frames(mp3_bytes_buffered);
            if (avail < min_start_frames) {
                vTaskDelay(pdMS_TO_TICKS(4));
                next_send_us = esp_timer_get_time();
                continue;
            }
            prebuffering = false;
            if (!spk.is_local_dac && raop->isConnected()) {
                raop->resyncTimestamp();
            }
            next_send_us = esp_timer_get_time();
        } else if (avail < RAOP_CHUNK_FRAMES) {
            if (!radio_connecting) {
                Serial.println("[RADIO] PCM ringbuffer underrun; re-buffering...");
                if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                    g_app.stream_underruns++;
                    xSemaphoreGive(g_app.mutex);
                }
            }
            prebuffering = true;
            vTaskDelay(pdMS_TO_TICKS(5));
            next_send_us = esp_timer_get_time();
            continue;
        }

        if (g_app.status != STREAM_PLAYING && xSemaphoreTake(g_app.mutex, 0) == pdTRUE) {
            if (g_app.status != STREAM_PLAYING) {
                g_app.status = STREAM_PLAYING;
                snprintf(g_app.status_text, sizeof(g_app.status_text), "%s -> %s",
                         use_sonos ? "Sonos radio" : "Streaming", spk.name);
            }
            xSemaphoreGive(g_app.mutex);
        }

        if (!spk.is_local_dac && raop->isConnected()) {
            if (!raop->acceptFrames()) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            if (ring_pop_352(chunk_pcm)) {
                update_vu_meters(chunk_pcm);
                raop->sendChunk(chunk_pcm);
                (void)also_local;
            }
        } else if (spk.is_local_dac || (use_sonos && sonos.isConnected())) {
            uint64_t now_us = esp_timer_get_time();
            if (now_us < next_send_us) {
                uint64_t wait_us = next_send_us - now_us;
                uint32_t delay_ms = (uint32_t)(wait_us / 1000ULL);
                vTaskDelay(pdMS_TO_TICKS(delay_ms > 0 ? delay_ms : 1));
                continue;
            }
            if (ring_pop_352(chunk_pcm)) {
                update_vu_meters(chunk_pcm);
                if (spk.is_local_dac || also_local) local_dac_write_frames(chunk_pcm, RAOP_CHUNK_FRAMES, vol);
                next_send_us += 7982ULL;
                uint64_t after_us = esp_timer_get_time();
                if (after_us > next_send_us + 50000ULL) {
                    next_send_us = after_us;
                } else if (after_us < next_send_us) {
                    uint64_t wait_us = next_send_us - after_us;
                    uint32_t delay_ms = (uint32_t)(wait_us / 1000ULL);
                    vTaskDelay(pdMS_TO_TICKS(delay_ms > 0 ? delay_ms : 1));
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

void radio_streamer_start(void) {
    if (!s_pcm_ring) {
        s_pcm_ring = (int16_t *)heap_caps_calloc(PCM_RING_FRAMES * 2, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    local_dac_init();

    // Allocate large audio decoder & RAOP task stacks in PSRAM to keep internal SRAM free for Wi-Fi DMA & TLS
    StackType_t *http_stack = (StackType_t *)heap_caps_malloc(28672, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticTask_t *http_tcb  = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    xTaskCreateStaticPinnedToCore(http_decoder_task, "radio_http", 28672 / sizeof(StackType_t), NULL, 4, http_stack, http_tcb, 0);

    StackType_t *raop_stack = (StackType_t *)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticTask_t *raop_tcb  = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (raop_stack && raop_tcb) {
        xTaskCreateStaticPinnedToCore(raop_sender_task, "raop_tx", 16384 / sizeof(StackType_t), NULL, 6, raop_stack, raop_tcb, 1);
    } else {
        xTaskCreatePinnedToCore(raop_sender_task, "raop_tx", 14336, NULL, 6, NULL, 1);
    }
}

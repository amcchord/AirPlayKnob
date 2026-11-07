"""Host checks for traffic gating, send accounting, TLS allocation and paced repair.

Compiles the firmware helpers and actual repair/allocator methods with fake IO.
Run: python3 tests/test_network_policy.py
"""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
art = (ROOT / 'src/audio/artwork_fetcher.cpp').read_text()
allocator = art[art.index('extern "C" void *__wrap_esp_mbedtls_mem_calloc('):art.index('ArtworkDiagnostics artwork_diagnostics()')]
raop = (ROOT / 'src/audio/raop_AnySpeaker.cpp').read_text()
repair = raop[raop.index('void RaopClient::handleControlSocket()'):raop.index('void RaopClient::pollControlAndKeepalive(')]
fixture = r'''
#include "network_diagnostics.h"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <arpa/inet.h>
#include <sys/socket.h>
using TaskHandle_t = void *;
static std::atomic<TaskHandle_t> s_art_task{(void *)1};
static std::atomic<uint32_t> s_tls_psram{0};
static TaskHandle_t current_task = (void *)1;
static int internal_allocs = 0, external_allocs = 0;
static bool external_fails = false;
static constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
static TaskHandle_t xTaskGetCurrentTaskHandle() { return current_task; }
extern "C" void *__real_esp_mbedtls_mem_calloc(size_t n, size_t s) { ++internal_allocs; return calloc(n,s); }
static void *heap_caps_calloc(size_t n, size_t s, int caps) {
    assert(caps == 3); ++external_allocs; return external_fails ? nullptr : calloc(n,s);
}
#define BACKLOG_SIZE 512
static uint64_t fake_now = 100000;
static uint64_t esp_timer_get_time() { return fake_now; }
static int send_calls = 0, fake_result = 0, receive_calls = 0, requests_left = 0;
static int fake_sendto(int, const void *, size_t len, int, const sockaddr *, socklen_t) {
    ++send_calls; errno = ENOMEM; return fake_result < 0 ? -1 : (int)len;
}
static int fake_recv(int, void *data, size_t, int) {
    ++receive_calls;
    if (!requests_left) return -1;
    --requests_left;
    uint8_t msg[8] = {0x80,0xD5,0,0,0,0,0,32}; memcpy(data,msg,8); return 8;
}
#define sendto fake_sendto
#define recv fake_recv
class RaopClient {
public:
    struct BacklogSlot { uint16_t seq=0,len=0; uint64_t last_sent_us=0; bool repair_pending=false; uint8_t data[1480]={}; };
    BacklogSlot *m_backlog = new BacklogSlot[512];
    uint8_t *m_rtp_pkt = new uint8_t[1484];
    bool m_connected=true, m_ap2=true;
    int m_ctrl_sock=1, m_audio_sock=2;
    uint32_t m_peer_ip_addr=0, m_pending_repairs=0, m_retransmits=0;
    uint16_t m_remote_audio_port=1,m_remote_ctrl_port=2,m_repair_cursor=0;
    uint64_t m_last_repair_us=0,m_last_pkt_us=0,m_last_reject_us=0;
    UdpTxStats m_repair_tx;
    ~RaopClient(){delete[] m_backlog; delete[] m_rtp_pkt;}
    void handleControlSocket(); void sendPendingRepair();
};
'''
cases = r'''
int main() {
    // An empty native PCM buffer must not disable artwork; real relay audio
    // still needs a cushion and all jobs yield to discovery/OTA.
    assert(artwork_network_allowed(true,true,false,0,6000));
    assert(!artwork_network_allowed(true,false,false,5999,6000));
    assert(artwork_network_allowed(true,false,false,6000,6000));
    assert(!artwork_network_allowed(true,true,true,0,6000));
    assert(artwork_network_allowed(false,false,false,0,6000));
    assert(discovery_must_wait(true,false,false,false,true,false));
    assert(discovery_must_wait(true,false,false,true,false,true));
    assert(!discovery_must_wait(true,false,true,false,false,false));
    assert(!discovery_must_wait(true,false,false,true,false,false));
    assert(!discovery_must_wait(true,true,false,false,false,false));
    assert(discovery_must_wait(true,true,false,false,true,false));
    assert(discovery_must_wait(true,true,false,false,false,true));
    assert(!discovery_must_wait(false,false,false,false,false,false));
    UdpTxStats tx;
    assert(!tx.record(-1,1452,ENOMEM)); assert(tx.accepted==0 && tx.rejected==1);
    assert(!tx.record(100,1452,EIO)); assert(tx.last_error==EIO);
    assert(tx.record(1452,1452,0)); assert(tx.attempted==3 && tx.accepted==1 && tx.rejected==2);
    uint8_t p[80]={};
    assert(!network_is_mdns(nullptr,0)); assert(!network_is_mdns(p,13));
    p[12]=8; p[14]=0x45; p[23]=17; p[36]=0x14; p[37]=0xE9;
    assert(network_is_mdns(p,42)); assert(!network_is_mdns(p,41));
    p[21]=1; assert(!network_is_mdns(p,42)); p[21]=0;
    p[14]=0x4F; assert(!network_is_mdns(p,42));
    memset(p,0,sizeof(p)); p[12]=0x86;p[13]=0xDD;p[20]=17;p[56]=0x14;p[57]=0xE9;
    assert(network_is_mdns(p,62)); assert(!network_is_mdns(p,61));
    void *a=__wrap_esp_mbedtls_mem_calloc(1,16384);assert(a && external_allocs==1 && internal_allocs==0);free(a);
    current_task=(void *)2;a=__wrap_esp_mbedtls_mem_calloc(1,16384);assert(a && internal_allocs==1);free(a);
    current_task=(void *)1;a=__wrap_esp_mbedtls_mem_calloc(1,128);assert(a && internal_allocs==2);free(a);
    external_fails=true;assert(!__wrap_esp_mbedtls_mem_calloc(1,16384));assert(internal_allocs==2);
    assert(!__wrap_esp_mbedtls_mem_calloc(SIZE_MAX,2));assert(s_tls_psram.load()==1);
    RaopClient c;
    for(int i=0;i<32;i++){c.m_backlog[i].seq=i;c.m_backlog[i].len=100;}
    requests_left=100;c.handleControlSocket();assert(receive_calls==32 && c.m_pending_repairs==32);
    c.sendPendingRepair();assert(send_calls==1 && c.m_pending_repairs==31 && c.m_retransmits==1);
    fake_now+=1000;c.sendPendingRepair();assert(send_calls==1);
    fake_now+=8000;c.m_last_pkt_us=fake_now-1000;c.sendPendingRepair();assert(send_calls==1);
    fake_now+=4000;fake_result=-1;c.sendPendingRepair();
    assert(send_calls==2 && c.m_pending_repairs==31 && c.m_repair_tx.rejected==1 && c.m_retransmits==1);
    fake_now+=8000;c.sendPendingRepair();assert(send_calls==2);
    fake_now+=20000;fake_result=0;c.sendPendingRepair();assert(send_calls==3 && c.m_pending_repairs==30);
    printf("PASS network gating, frame parsing, accepted/rejected accounting, task-scoped TLS allocation and actual paced-repair methods.\n");
}
'''
(ROOT / 'scratch').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='network-test-', dir=ROOT / 'scratch') as directory:
    temp = pathlib.Path(directory)
    (temp / 'test.cpp').write_text(fixture + allocator + repair + cases)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-I', str(ROOT / 'include'),
                    str(temp / 'test.cpp'), '-o', str(temp / 'test')], check=True)
    subprocess.run([str(temp / 'test')], check=True)

"""Exercise the firmware's actual RTSP transaction method with delayed replies.

Run: python3 tests/test_rtsp_responses.py
Requires a host C++ compiler; generated files stay in ignored scratch/.
"""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
source = (ROOT / 'src/audio/raop_AnySpeaker.cpp').read_text()
start = source.index('bool RaopClient::execRtsp(')
end = source.index('bool RaopClient::reconnectRtspSocket()', start)
method = source[start:end]

fixture = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cassert>
struct Logger {
    template<class... Args> void printf(const char *, Args...) {}
    void println(const char *) {}
} Serial;
static uint32_t millis() { static uint32_t now = 0; return ++now; }
static void *raop_alloc(size_t n) { return malloc(n); }
class RaopClient {
public:
    int m_rtsp_sock = 1, m_cseq = 0;
    bool m_ap2 = false;
    char m_rtsp_url[128] = "rtsp://test/session";
    char m_user_agent[48] = "test", m_sci[20] = "", m_dacp_id[20] = "";
    char m_active_remote[16] = "", m_session[64] = "";
    uint8_t *m_rx_plain_buf = nullptr;
    size_t m_rx_plain_len = 0, index = 0;
    bool send_fails = false;
    std::vector<std::string> chunks;
    std::string request;
    ~RaopClient() { free(m_rx_plain_buf); }
    int rtspSend(const uint8_t *data, int len) {
        request.assign(reinterpret_cast<const char *>(data), len);
        return send_fails ? -1 : len;
    }
    void drainRtspResponses() {} // No replies are available until after send.
    bool rtspFillPlain(int) {
        if (index == chunks.size()) return false;
        const auto &part = chunks[index++];
        m_rx_plain_buf = (uint8_t *)realloc(m_rx_plain_buf, m_rx_plain_len + part.size() + 1);
        memcpy(m_rx_plain_buf + m_rx_plain_len, part.data(), part.size());
        m_rx_plain_len += part.size(); m_rx_plain_buf[m_rx_plain_len] = 0;
        return true;
    }
    bool execRtsp(const char *, const char *, const char *, const uint8_t *, int,
                  const char *, char *, int, uint8_t **, int *, int *);
};
'''

cases = r'''
static std::string reply(int seq, int status, std::string body = "") {
    return "RTSP/1.0 " + std::to_string(status) + " Result\r\nCSeq: " + std::to_string(seq) +
           "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}
static int checks = 0;
static void check(std::vector<std::string> chunks, bool expected_ok, int expected_status,
                  std::string expected_body = "", int initial_seq = 0, bool send_fails = false) {
    RaopClient c; c.chunks = chunks; c.m_cseq = initial_seq; c.send_fails = send_fails;
    uint8_t *body = nullptr; int len = -1, status = -1; char headers[1024] = {};
    bool ok = c.execRtsp("SETUP", nullptr, nullptr, nullptr, 0, nullptr,
                         headers, sizeof(headers), &body, &len, &status);
    assert(ok == expected_ok); assert(status == expected_status);
    assert(std::string(body ? (char *)body : "", len) == expected_body);
    if (status) assert(strstr(headers, ("CSeq: " + std::to_string(initial_seq + 1)).c_str()));
    assert(c.request.find("CSeq: " + std::to_string(initial_seq + 1) + "\r\n") != std::string::npos);
    free(body); checks++;
}
int main() {
    check({reply(1,200,"ok")}, true, 200, "ok");
    check({reply(0,500,"old") + reply(1,200,"ok")}, true, 200, "ok");
    check({reply(0,200,"old"), reply(1,200,"ok")}, true, 200, "ok");
    check({reply(4,500,"late RECORD") + reply(5,200,"stream")}, true, 200, "stream", 4);
    check({reply(0,200,"\r\nCSeq: 1\r\n\r\nfake") + reply(1,200,"real")}, true, 200, "real");
    auto response = reply(1,200,"bplist-body");
    check({response.substr(0,9), response.substr(9,37), response.substr(46)}, true, 200, "bplist-body");
    check({reply(1,403,"denied")}, false, 403, "denied");
    check({reply(0,200,"old") + reply(1,500,"failed")}, false, 500, "failed");
    check({reply(0,200)}, false, 0);
    check({reply(2,200)}, false, 0);
    check({"RTSP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n"}, false, 0);
    check({"RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: -1\r\n\r\n"}, false, 0);
    check({"RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 99999999\r\n\r\n"}, false, 0);
    check({"RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 9\r\n\r\nshort"}, false, 0);
    check({}, false, 0);
    check({reply(1,200)}, false, 0, "", 0, true);
    printf("PASS: %d firmware RTSP transaction cases.\n", checks);
}
'''

(ROOT / 'scratch').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='rtsp-test-', dir=ROOT / 'scratch') as directory:
    temp = pathlib.Path(directory)
    (temp / 'test.cpp').write_text(fixture + method + cases)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', str(temp / 'test.cpp'),
                    '-o', str(temp / 'test')], check=True)
    subprocess.run([str(temp / 'test')], check=True)

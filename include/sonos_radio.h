#pragma once
#include "app_state.h"
#include <Arduino.h>

// Sonos fetches the original radio stream itself. The knob retains playback,
// station, and volume control without relaying CD-rate PCM over its Wi-Fi link.
class SonosRadioClient {
public:
    static bool isCandidate(const AirPlaySpeaker &speaker);
    bool start(const AirPlaySpeaker &speaker, const char *url, const char *title, int volume);
    bool setVolume(int volume);
    void stop();
    bool isConnected() const { return m_connected; }
    void poll();
    bool rejected() const { return m_rejected; }
private:
    bool verifySpeaker();
    bool request(const char *service, const char *action, const String &arguments, String *response = NULL);
    char m_ip[20] = {0};
    bool m_verified = false;
    bool m_connected = false;
    bool m_rejected = false;
    uint32_t m_last_poll_ms = 0;
};

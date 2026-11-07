#include "sonos_radio.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <strings.h>

static String xml_escape(const char *text) {
    String out;
    if (!text) return out;
    out.reserve(strlen(text) + 32);
    for (const char *p = text; *p; ++p) {
        switch (*p) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += *p;
        }
    }
    return out;
}

static String xml_unescape(String text) {
    text.replace("&lt;", "<"); text.replace("&gt;", ">");
    text.replace("&quot;", "\""); text.replace("&apos;", "'");
    text.replace("&amp;", "&");
    return text;
}

static String xml_value(const String &xml, const char *tag) {
    String begin = String("<") + tag + ">";
    int start = xml.indexOf(begin);
    if (start < 0) return String();
    start += begin.length();
    int end = xml.indexOf(String("</") + tag + ">", start);
    return end < 0 ? String() : xml.substring(start, end);
}

bool SonosRadioClient::isCandidate(const AirPlaySpeaker &speaker) {
    if (speaker.is_local_dac) return false;
    if (strcasestr(speaker.host, "Sonos-") || strcasestr(speaker.am, "Sonos")) return true;
    const char *models[] = {"Five", "Amp", "Port", "Arc", "Beam", "Ray", "One", "Move", "Roam"};
    for (const char *model : models) if (strcasecmp(speaker.am, model) == 0) return true;
    return strncasecmp(speaker.am, "Era ", 4) == 0 ||
           strncasecmp(speaker.am, "SYMFONISK", 9) == 0;
}

bool SonosRadioClient::verifySpeaker() {
    if (m_verified) return true;
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(2500);
    String url = String("http://") + m_ip + ":1400/xml/device_description.xml";
    if (!http.begin(client, url)) return false;
    int code = http.GET();
    if (code == 200) {
        String description = http.getString();
        m_verified = description.indexOf("<manufacturer>Sonos, Inc.</manufacturer>") >= 0;
        if (!m_verified) m_rejected = true;
    } else if (code >= 400) {
        m_rejected = true;
    }
    http.end();
    return m_verified;
}

bool SonosRadioClient::request(const char *service, const char *action,
                               const String &arguments, String *response) {
    if (!m_ip[0]) return false;
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(2500);
    String url = String("http://") + m_ip + ":1400/MediaRenderer/" + service + "/Control";
    if (!http.begin(client, url)) return false;
    String type = String("urn:schemas-upnp-org:service:") + service + ":1";
    String body = String("<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><u:") + action +
                  " xmlns:u=\"" + type + "\"><InstanceID>0</InstanceID>" + arguments +
                  "</u:" + action + "></s:Body></s:Envelope>";
    http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
    http.addHeader("SOAPACTION", String("\"") + type + "#" + action + "\"");
    int code = http.POST(body);
    if (response && code == 200) *response = http.getString();
    if (code != 200) {
        Serial.printf("[SONOS] %s failed (%d)\n", action, code);
        // A receiver rejecting native control can still use the existing
        // AirPlay path. A transient network failure should retry native control.
        if (code >= 400) m_rejected = true;
    }
    http.end();
    return code == 200;
}

bool SonosRadioClient::setVolume(int volume) {
    volume = constrain(volume, 0, 100);
    return request("RenderingControl", "SetVolume",
                   String("<Channel>Master</Channel><DesiredVolume>") + volume + "</DesiredVolume>");
}

bool SonosRadioClient::start(const AirPlaySpeaker &speaker, const char *url,
                            const char *title, int volume) {
    m_connected = false;
    m_rejected = false;
    if (strcmp(m_ip, speaker.ip) != 0) {
        strncpy(m_ip, speaker.ip, sizeof(m_ip) - 1);
        m_verified = false;
    }
    if (!verifySpeaker() || !url || !url[0]) return false;
    String uri(url);
    if (uri.startsWith("http://")) uri = String("x-rincon-mp3radio:") + uri.substring(5);
    String metadata = String("<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" ") +
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">" +
        "<item id=\"-1\" parentID=\"-1\" restricted=\"true\"><dc:title>" + xml_escape(title) +
        "</dc:title><upnp:class>object.item.audioItem.audioBroadcast</upnp:class></item></DIDL-Lite>";
    String arguments = String("<CurrentURI>") + xml_escape(uri.c_str()) +
        "</CurrentURI><CurrentURIMetaData>" + xml_escape(metadata.c_str()) + "</CurrentURIMetaData>";
    if (!setVolume(volume)) return false;
    if (volume > 0 && !request("RenderingControl", "SetMute",
            "<Channel>Master</Channel><DesiredMute>0</DesiredMute>")) return false;
    String current_media;
    bool same_uri = request("AVTransport", "GetMediaInfo", "", &current_media) &&
        current_media.indexOf(String("<CurrentURI>") + xml_escape(uri.c_str()) + "</CurrentURI>") >= 0;
    if ((!same_uri && !request("AVTransport", "SetAVTransportURI", arguments)) ||
        !request("AVTransport", "Play", "<Speed>1</Speed>")) return false;
    m_connected = true;
    m_last_poll_ms = millis();
    Serial.printf("[SONOS] Native radio -> %s (speaker fetches %s)\n", speaker.name, url);
    return true;
}

void SonosRadioClient::stop() {
    if (m_connected) request("AVTransport", "Stop", "");
    m_connected = false;
}

void SonosRadioClient::poll() {
    if (!m_connected || millis() - m_last_poll_ms < 10000) return;
    m_last_poll_ms = millis();
    String response;
    // A brief controller Wi-Fi outage should not interrupt the speaker's own
    // stream. Only restart when the receiver explicitly reports that it stopped.
    if (request("AVTransport", "GetTransportInfo", "", &response) &&
        (response.indexOf("<CurrentTransportState>STOPPED</CurrentTransportState>") >= 0 ||
         response.indexOf("<CurrentTransportState>NO_MEDIA_PRESENT</CurrentTransportState>") >= 0)) {
        m_connected = false;
    }
    if (m_connected && request("AVTransport", "GetPositionInfo", "", &response)) {
        String metadata = xml_unescape(xml_value(response, "TrackMetaData"));
        String title = xml_unescape(xml_value(metadata, "r:streamContent"));
        if (title.length() > 0 && xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (g_app.current_speaker >= 0 && g_app.current_speaker < g_app.speaker_count &&
                strcmp(g_app.speakers[g_app.current_speaker].ip, m_ip) == 0) {
                strncpy(g_app.track_title, title.c_str(), sizeof(g_app.track_title) - 1);
            }
            xSemaphoreGive(g_app.mutex);
        }
    }
}

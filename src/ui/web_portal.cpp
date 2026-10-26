#include "web_portal.h"
#include "app_state.h"
#include "knob_ui.h"
#include "bsp/lcd_bsp.h"
#include "bsp/cst816.h"
#include "bsp/lcd_config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>

static WebServer s_server(80);

static const char WEB_UI_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>AirPlayKnob — SomaFM & AirPlay Controller</title>
<style>
  :root {
    --bg: #050608;
    --card: #0c1017;
    --card2: #151c28;
    --text: #f8fafc;
    --muted: #94a3b8;
    --accent: #10b981;
    --border: rgba(255,255,255,0.08);
  }
  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Inter", "Segoe UI", Roboto, sans-serif; }
  body { background: var(--bg); color: var(--text); padding: 20px; max-width: 960px; margin: 0 auto; }
  header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 20px; flex-wrap: wrap; gap: 12px; }
  .brand { display: flex; align-items: center; gap: 12px; }
  .knob-logo { width: 40px; height: 40px; border-radius: 50%; background: radial-gradient(circle at 30% 30%, var(--accent), #050608 75%); border: 2px solid var(--accent); box-shadow: 0 0 18px rgba(16,185,129,0.3); }
  h1 { font-size: 20px; font-weight: 700; letter-spacing: -0.3px; }
  .sub { font-size: 12px; color: var(--muted); }
  .wifi-pill { background: var(--card); border: 1px solid var(--border); padding: 6px 14px; border-radius: 999px; font-size: 12px; color: #38bdf8; }

  .now-card {
    background: linear-gradient(145deg, #0d131f 0%, #070a10 100%);
    border: 1px solid var(--border);
    border-radius: 22px;
    padding: 22px;
    margin-bottom: 20px;
    box-shadow: 0 16px 38px rgba(0,0,0,0.5);
  }
  .now-top { display: flex; justify-content: space-between; align-items: center; gap: 18px; flex-wrap: wrap; }
  .now-left { display: flex; align-items: center; gap: 18px; flex: 1; min-width: 260px; }
  .art-box {
    width: 92px; height: 92px; border-radius: 16px; overflow: hidden; flex-shrink: 0;
    background: #111827; border: 1px solid rgba(255,255,255,0.12);
    box-shadow: 0 8px 24px rgba(0,0,0,0.45);
  }
  .art-box img { width: 100%; height: 100%; object-fit: cover; display: block; }
  .badge { display: inline-block; padding: 3px 10px; border-radius: 99px; font-size: 10px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.8px; background: rgba(16,185,129,0.15); color: var(--accent); border: 1px solid var(--accent); margin-bottom: 6px; }
  .now-track { font-size: 22px; font-weight: 700; line-height: 1.2; color: #fff; margin-bottom: 4px; }
  .now-station { font-size: 14px; color: var(--muted); }
  .now-status { font-size: 12px; color: var(--accent); margin-top: 4px; }

  .hero-controls { display: flex; align-items: center; gap: 16px; margin-top: 18px; flex-wrap: wrap; padding-top: 16px; border-top: 1px solid var(--border); }
  .btn { cursor: pointer; border: none; border-radius: 999px; padding: 10px 18px; font-size: 13px; font-weight: 600; transition: all 0.15s; display: inline-flex; align-items: center; gap: 6px; }
  .btn:active { transform: scale(0.96); }
  .btn-primary { background: var(--accent); color: #050608; }
  .btn-secondary { background: var(--card2); color: var(--text); border: 1px solid var(--border); }
  .btn-purple { background: rgba(56,189,248,0.15); color: #38bdf8; border: 1px solid rgba(56,189,248,0.35); }
  .btn-danger { background: rgba(239,68,68,0.16); color: #f87171; border: 1px solid rgba(239,68,68,0.3); }
  .btn-sm { padding: 6px 12px; font-size: 12px; }

  .vol-wrap { flex: 1; display: flex; align-items: center; gap: 10px; min-width: 200px; }
  input[type=range] { flex: 1; accent-color: var(--accent); height: 6px; cursor: pointer; }

  .grid-2 { display: grid; grid-template-columns: 1.35fr 1fr; gap: 20px; }
  @media (max-width: 760px) { .grid-2 { grid-template-columns: 1fr; } }

  .panel { background: var(--card); border: 1px solid var(--border); border-radius: 18px; padding: 18px; margin-bottom: 20px; }
  .panel-hdr { display: flex; justify-content: space-between; align-items: center; margin-bottom: 14px; }
  .panel-title { font-size: 14px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.6px; color: var(--muted); }

  .preset-list { max-height: 440px; overflow-y: auto; display: flex; flex-direction: column; gap: 8px; padding-right: 4px; }
  .preset-item {
    display: flex; align-items: center; justify-content: space-between;
    background: rgba(255,255,255,0.02); border: 1px solid var(--border);
    border-radius: 12px; padding: 10px 12px; gap: 10px; transition: border 0.15s;
  }
  .preset-item.active { border-color: var(--accent); background: rgba(16,185,129,0.08); }
  .preset-left { display: flex; align-items: center; gap: 10px; flex: 1; cursor: pointer; min-width: 0; }
  .color-bar { width: 5px; height: 32px; border-radius: 4px; flex-shrink: 0; }
  .preset-info { min-width: 0; flex: 1; }
  .preset-name { font-size: 14px; font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .preset-meta { font-size: 11px; color: var(--muted); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .preset-actions { display: flex; gap: 6px; }

  .form-row { display: flex; flex-direction: column; gap: 10px; margin-top: 10px; }
  input[type=text], input[type=password], input[type=file] {
    width: 100%; background: #070a10; border: 1px solid var(--border);
    color: var(--text); padding: 10px 12px; border-radius: 10px; font-size: 13px;
  }
  .spk-item {
    display: flex; align-items: center; justify-content: space-between;
    padding: 10px 12px; border-radius: 12px; border: 1px solid var(--border);
    margin-bottom: 8px; cursor: pointer; background: rgba(255,255,255,0.02);
  }
  .spk-item.active { border-color: #38bdf8; background: rgba(56,189,248,0.1); }
  .progress-bar { width: 100%; height: 8px; background: #1e293b; border-radius: 99px; overflow: hidden; display: none; margin-top: 8px; }
  .progress-fill { height: 100%; width: 0%; background: #38bdf8; transition: width 0.15s; }
  .toast { position: fixed; bottom: 20px; right: 20px; background: #10b981; color: #050608; font-weight: 700; padding: 10px 18px; border-radius: 99px; display: none; box-shadow: 0 8px 24px rgba(0,0,0,0.4); }
</style>
</head>
<body>
<div>
  <header>
    <div class="brand">
      <div class="knob-logo"></div>
      <div>
        <h1>AirPlayKnob</h1>
        <div class="sub">Minimalist SomaFM Internet Radio to AirPlay Streamer</div>
      </div>
    </div>
    <div class="wifi-pill" id="wifiBadge">Connecting...</div>
  </header>

  <section class="now-card">
    <div class="now-top">
      <div class="now-left">
        <div class="art-box">
          <img id="nowArt" src="https://somafm.com/img/groovesalad120.jpg" alt="Cover Art">
        </div>
        <div>
          <span class="badge" id="nowGenre">SOMAFM</span>
          <div class="now-track" id="nowTrack">Loading...</div>
          <div class="now-station" id="nowStation">Groove Salad</div>
          <div class="now-status" id="nowStatus">Ready</div>
        </div>
      </div>
      <div style="display:flex;gap:8px;align-items:center;flex-wrap:wrap;">
        <button class="btn btn-secondary" onclick="stepPreset(-1)">⏮</button>
        <button class="btn btn-primary" id="playBtn" onclick="togglePlay()">⏸ Pause</button>
        <button class="btn btn-secondary" onclick="stepPreset(1)">⏭</button>
        <button class="btn btn-purple" id="wcBtn" onclick="toggleWhiteCap()">✨ WhiteCap Visualizer</button>
      </div>
    </div>
    <div class="hero-controls">
      <div class="vol-wrap">
        <span style="font-size:13px;color:var(--muted);min-width:95px;">Volume: <strong id="volVal" style="color:#fff">65%</strong></span>
        <input type="range" id="volSlider" min="0" max="100" value="65" oninput="onVolInput(this.value)" onchange="setVolume(this.value)">
      </div>
      <div style="display:flex;align-items:center;gap:8px;">
        <span style="font-size:13px;color:var(--muted);">Knob Dial:</span>
        <button class="btn btn-secondary btn-sm" id="knobModeBtn" onclick="cycleKnobMode()">VOL</button>
      </div>
    </div>
  </section>

  <div class="grid-2">
    <div>
      <div class="panel">
        <div class="panel-hdr">
          <div class="panel-title">Radio Presets (<span id="presetCount">0</span>)</div>
          <button class="btn btn-secondary btn-sm" onclick="resetPresets()">Restore SomaFM Defaults</button>
        </div>
        <div class="preset-list" id="presetList"></div>
      </div>

      <div class="panel">
        <div class="panel-hdr">
          <div class="panel-title" id="formTitle">Add or Edit Station Preset</div>
          <button class="btn btn-secondary btn-sm" onclick="clearForm()">Clear</button>
        </div>
        <div class="form-row">
          <input type="hidden" id="editIdx" value="-1">
          <input type="text" id="stName" placeholder="Station Name (e.g. Groove Salad)">
          <div style="display:flex;gap:8px;">
            <input type="text" id="stGenre" placeholder="Genre (e.g. Ambient / Chill)" style="flex:1;">
            <input type="color" id="stColor" value="#10b981" style="width:52px;height:40px;border:none;background:none;cursor:pointer;">
          </div>
          <input type="text" id="stUrl" placeholder="MP3 Stream URL (http://ice6.somafm.com/groovesalad-128-mp3)">
          <button class="btn btn-primary" onclick="savePreset()">Save Station Preset</button>
        </div>
      </div>
    </div>

    <div>
      <div class="panel">
        <div class="panel-hdr">
          <div class="panel-title">AirPlay Speakers</div>
          <button class="btn btn-purple btn-sm" onclick="scanSpeakers()">Rescan AirPlay</button>
        </div>
        <div id="speakerList"></div>
        <label style="display:flex;align-items:center;gap:8px;margin-top:12px;font-size:13px;color:var(--muted);cursor:pointer;">
          <input type="checkbox" id="localDacChk" onchange="toggleLocalDac(this.checked)">
          Also output audio to onboard 3.5mm stereo jack
        </label>
      </div>

      <div class="panel">
        <div class="panel-hdr">
          <div class="panel-title">OTA Firmware Update</div>
        </div>
        <div class="form-row">
          <input type="file" id="otaFile" accept=".bin">
          <button class="btn btn-purple" onclick="uploadOta()">Upload Firmware (.bin)</button>
          <div class="progress-bar" id="otaBar"><div class="progress-fill" id="otaFill"></div></div>
          <div id="otaStatus" style="font-size:12px;color:var(--muted);">Also supports ArduinoOTA at airplayknob.local:3232</div>
        </div>
      </div>

      <div class="panel">
        <div class="panel-hdr">
          <div class="panel-title">Wi-Fi Network</div>
        </div>
        <div class="form-row">
          <input type="text" id="wifiSsid" placeholder="Wi-Fi SSID">
          <input type="password" id="wifiPass" placeholder="Wi-Fi Password">
          <button class="btn btn-secondary" onclick="saveWifi()">Join Wi-Fi Network</button>
        </div>
      </div>
    </div>
  </div>
</div>
<div class="toast" id="toast">Saved!</div>

<script>
let state = null;
let draggingVol = false;

function showToast(msg) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.style.display = 'block';
  setTimeout(() => t.style.display = 'none', 2200);
}

async function api(path, method='GET', body=null) {
  const opts = { method, headers: {} };
  if (body) {
    opts.headers['Content-Type'] = 'application/json';
    opts.body = JSON.stringify(body);
  }
  const r = await fetch(path, opts);
  return await r.json();
}

function hexColor(num) {
  return '#' + ('000000' + (num || 0x10b981).toString(16)).slice(-6);
}

async function refreshState() {
  try {
    const s = await api('/api/status');
    state = s;
    const cur = s.presets[s.current_preset] || { name: 'None', genre: '', color: 0x10b981 };
    const col = hexColor(cur.color);
    document.documentElement.style.setProperty('--accent', col);
    document.getElementById('nowStation').textContent = cur.name;
    document.getElementById('nowGenre').textContent = cur.genre || 'SOMAFM';
    document.getElementById('nowTrack').textContent = s.track_title || cur.name;
    document.getElementById('nowStatus').textContent = s.status_text || '';
    document.getElementById('playBtn').textContent = s.playing ? '⏸ Pause' : '▶ Play';
    document.getElementById('wcBtn').textContent = s.whitecap_active ? '✨ Exit WhiteCap' : '✨ WhiteCap Visualizer';
    document.getElementById('wifiBadge').textContent = `${s.wifi_ssid} • ${s.wifi_ip}`;
    document.getElementById('localDacChk').checked = !!s.also_local_dac;
    if (s.artwork_url && document.getElementById('nowArt').src !== s.artwork_url) {
      document.getElementById('nowArt').src = s.artwork_url;
    }
    const modes = ['VOL', 'TUNE', 'SPKR'];
    document.getElementById('knobModeBtn').textContent = modes[s.knob_mode] || 'VOL';

    if (!draggingVol) {
      document.getElementById('volSlider').value = s.volume;
      document.getElementById('volVal').textContent = s.volume + '%';
    }
    renderPresets(s);
    renderSpeakers(s);
  } catch (e) {}
}

function renderPresets(s) {
  document.getElementById('presetCount').textContent = s.presets.length;
  const el = document.getElementById('presetList');
  el.innerHTML = s.presets.map((p, i) => `
    <div class="preset-item ${i === s.current_preset ? 'active' : ''}">
      <div class="preset-left" onclick="playPreset(${i})">
        <div class="color-bar" style="background:${hexColor(p.color)}"></div>
        <div class="preset-info">
          <div class="preset-name">${i + 1}. ${p.name}</div>
          <div class="preset-meta">${p.genre} • ${p.url}</div>
        </div>
      </div>
      <div class="preset-actions">
        <button class="btn btn-secondary btn-sm" title="Edit" onclick="editPreset(${i})">✎</button>
        <button class="btn btn-danger btn-sm" title="Delete" onclick="deletePreset(${i})">✕</button>
      </div>
    </div>
  `).join('');
}

function renderSpeakers(s) {
  const el = document.getElementById('speakerList');
  el.innerHTML = s.speakers.map((sp, i) => `
    <div class="spk-item ${i === s.current_speaker ? 'active' : ''}" onclick="selectSpeaker(${i})">
      <div>
        <div style="font-weight:600;font-size:14px;">${sp.name}</div>
        <div style="font-size:11px;color:var(--muted);">${sp.is_local_dac ? 'Onboard 3.5mm Stereo Jack' : (sp.ip + ':' + sp.port + ' ' + sp.am)}</div>
      </div>
      <span style="font-size:12px;color:#38bdf8;font-weight:600;">${i === s.current_speaker ? 'Active' : 'Select'}</span>
    </div>
  `).join('');
}

async function playPreset(idx) {
  await api('/api/control', 'POST', { preset: idx, play: true });
  showToast('Tuning station...');
  refreshState();
}

async function stepPreset(dir) {
  if (!state || !state.presets.length) return;
  const next = (state.current_preset + dir + state.presets.length) % state.presets.length;
  await playPreset(next);
}

async function togglePlay() {
  await api('/api/control', 'POST', { toggle_play: true });
  refreshState();
}

async function toggleWhiteCap() {
  await api('/api/control', 'POST', { toggle_whitecap: true });
  refreshState();
}

function onVolInput(v) {
  draggingVol = true;
  document.getElementById('volVal').textContent = v + '%';
}

async function setVolume(v) {
  draggingVol = false;
  await api('/api/control', 'POST', { volume: parseInt(v, 10) });
}

async function cycleKnobMode() {
  if (!state) return;
  const next = (state.knob_mode + 1) % 3;
  await api('/api/control', 'POST', { knob_mode: next });
  refreshState();
}

async function selectSpeaker(idx) {
  await api('/api/control', 'POST', { speaker: idx });
  showToast('Routing audio...');
  refreshState();
}

async function scanSpeakers() {
  await api('/api/control', 'POST', { scan_speakers: true });
  showToast('Scanning AirPlay speakers...');
  setTimeout(refreshState, 3500);
}

async function toggleLocalDac(val) {
  await api('/api/control', 'POST', { also_local_dac: val });
}

function editPreset(idx) {
  const p = state.presets[idx];
  if (!p) return;
  document.getElementById('editIdx').value = idx;
  document.getElementById('stName').value = p.name;
  document.getElementById('stGenre').value = p.genre;
  document.getElementById('stUrl').value = p.url;
  document.getElementById('stColor').value = hexColor(p.color);
  document.getElementById('formTitle').textContent = `Editing #${idx + 1}: ${p.name}`;
}

function clearForm() {
  document.getElementById('editIdx').value = -1;
  document.getElementById('stName').value = '';
  document.getElementById('stGenre').value = '';
  document.getElementById('stUrl').value = '';
  document.getElementById('formTitle').textContent = 'Add or Edit Station Preset';
}

async function savePreset() {
  const idx = parseInt(document.getElementById('editIdx').value, 10);
  const name = document.getElementById('stName').value.trim();
  const genre = document.getElementById('stGenre').value.trim() || 'Radio';
  const url = document.getElementById('stUrl').value.trim();
  const color = parseInt(document.getElementById('stColor').value.replace('#',''), 16);
  if (!name || !url) return showToast('Enter name & stream URL');
  await api('/api/presets', 'POST', { action: idx >= 0 ? 'update' : 'add', index: idx, name, genre, url, color });
  clearForm();
  showToast('Preset saved!');
  refreshState();
}

async function deletePreset(idx) {
  if (!confirm('Delete this station preset?')) return;
  await api('/api/presets', 'POST', { action: 'delete', index: idx });
  showToast('Deleted');
  refreshState();
}

async function resetPresets() {
  if (!confirm('Restore all 30 SomaFM stations?')) return;
  await api('/api/presets', 'POST', { action: 'reset' });
  showToast('SomaFM defaults restored!');
  refreshState();
}

async function saveWifi() {
  const ssid = document.getElementById('wifiSsid').value.trim();
  const pass = document.getElementById('wifiPass').value;
  if (!ssid) return showToast('Enter Wi-Fi SSID');
  await api('/api/wifi', 'POST', { ssid, pass });
  showToast('Connecting to ' + ssid + '...');
}

function uploadOta() {
  const fileInput = document.getElementById('otaFile');
  if (!fileInput.files.length) return showToast('Select a firmware .bin file');
  const file = fileInput.files[0];
  const formData = new FormData();
  formData.append('update', file);

  const bar = document.getElementById('otaBar');
  const fill = document.getElementById('otaFill');
  const status = document.getElementById('otaStatus');
  bar.style.display = 'block';
  fill.style.width = '0%';
  status.textContent = 'Uploading firmware...';

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/ota', true);
  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) {
      const pct = Math.round((e.loaded / e.total) * 100);
      fill.style.width = pct + '%';
      status.textContent = `Uploading firmware: ${pct}%`;
    }
  };
  xhr.onload = () => {
    if (xhr.status === 200) {
      status.textContent = 'Update complete! Rebooting AirPlayKnob...';
      showToast('OTA Success! Rebooting...');
      setTimeout(() => location.reload(), 6000);
    } else {
      status.textContent = 'OTA failed: ' + xhr.responseText;
    }
  };
  xhr.send(formData);
}

refreshState();
setInterval(refreshState, 2200);
</script>
</body>
</html>
)rawliteral";

static void json_escape(const char *in, char *out, size_t out_max) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 2 < out_max; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = c;
        } else if ((unsigned char)c >= 0x20) {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

static int extract_json_int(const String &body, const char *key, int def_val) {
    String pat = String("\"") + key + "\":";
    int idx = body.indexOf(pat);
    if (idx < 0) return def_val;
    idx += pat.length();
    while (idx < (int)body.length() && body[idx] == ' ') idx++;
    return body.substring(idx).toInt();
}

static bool extract_json_bool(const String &body, const char *key, bool *found) {
    String pat = String("\"") + key + "\":";
    int idx = body.indexOf(pat);
    if (idx < 0) {
        if (found) *found = false;
        return false;
    }
    if (found) *found = true;
    idx += pat.length();
    while (idx < (int)body.length() && body[idx] == ' ') idx++;
    return body.substring(idx).startsWith("true");
}

static String extract_json_str(const String &body, const char *key) {
    String pat = String("\"") + key + "\":\"";
    int idx = body.indexOf(pat);
    if (idx < 0) return "";
    idx += pat.length();
    String out = "";
    while (idx < (int)body.length()) {
        char c = body[idx++];
        if (c == '\\' && idx < (int)body.length()) {
            out += body[idx++];
        } else if (c == '"') {
            break;
        } else {
            out += c;
        }
    }
    return out;
}

static void handle_root() {
    s_server.send_P(200, "text/html", WEB_UI_HTML);
}

static void handle_status() {
    String json;
    json.reserve(6144);

    if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        char esc1[160], esc2[160], esc3[280];
        json += "{";
        json += "\"playing\":"; json += (g_app.playing ? "true" : "false"); json += ",";
        json += "\"whitecap_active\":"; json += (g_app.whitecap_active ? "true" : "false"); json += ",";
        json += "\"volume\":"; json += String(g_app.volume); json += ",";
        json += "\"current_preset\":"; json += String(g_app.current_preset); json += ",";
        json += "\"current_speaker\":"; json += String(g_app.current_speaker); json += ",";
        json += "\"knob_mode\":"; json += String((int)g_app.knob_mode); json += ",";
        json += "\"also_local_dac\":"; json += (g_app.also_local_dac ? "true" : "false"); json += ",";
        json_escape(g_app.track_title, esc1, sizeof(esc1));
        json_escape(g_app.status_text, esc2, sizeof(esc2));
        json_escape(g_app.artwork_url, esc3, sizeof(esc3));
        json += "\"track_title\":\""; json += esc1; json += "\",";
        json += "\"status_text\":\""; json += esc2; json += "\",";
        json += "\"artwork_url\":\""; json += esc3; json += "\",";
        json_escape(g_app.wifi_ssid, esc1, sizeof(esc1));
        json += "\"wifi_ssid\":\""; json += esc1; json += "\",";
        json += "\"wifi_ip\":\""; json += g_app.wifi_ip; json += "\",";

        json += "\"presets\":[";
        for (int i = 0; i < g_app.preset_count; i++) {
            if (i > 0) json += ",";
            json_escape(g_app.presets[i].name, esc1, sizeof(esc1));
            json_escape(g_app.presets[i].genre, esc2, sizeof(esc2));
            json_escape(g_app.presets[i].url, esc3, sizeof(esc3));
            json += "{\"name\":\""; json += esc1;
            json += "\",\"genre\":\""; json += esc2;
            json += "\",\"url\":\""; json += esc3;
            json += "\",\"color\":"; json += String(g_app.presets[i].color);
            json += "}";
        }
        json += "],\"speakers\":[";
        for (int i = 0; i < g_app.speaker_count; i++) {
            if (i > 0) json += ",";
            json_escape(g_app.speakers[i].name, esc1, sizeof(esc1));
            json_escape(g_app.speakers[i].am, esc2, sizeof(esc2));
            json += "{\"name\":\""; json += esc1;
            json += "\",\"ip\":\""; json += g_app.speakers[i].ip;
            json += "\",\"port\":"; json += String(g_app.speakers[i].port);
            json += ",\"am\":\""; json += esc2;
            json += "\",\"is_local_dac\":"; json += (g_app.speakers[i].is_local_dac ? "true" : "false");
            json += "}";
        }
        json += "]}";
        xSemaphoreGive(g_app.mutex);
    } else {
        json = "{\"error\":\"busy\"}";
    }
    s_server.send(200, "application/json", json);
}

static void handle_control() {
    String body = s_server.arg("plain");
    bool has_toggle = false;
    extract_json_bool(body, "toggle_play", &has_toggle);
    if (has_toggle) {
        haptic_play_effect(HAPTIC_SHARP_CLICK);
        app_toggle_play();
    }
    bool has_wc = false;
    extract_json_bool(body, "toggle_whitecap", &has_wc);
    if (has_wc) {
        haptic_play_effect(HAPTIC_DOUBLE_CLICK);
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            g_app.whitecap_active = !g_app.whitecap_active;
            xSemaphoreGive(g_app.mutex);
        }
    }
    int preset = extract_json_int(body, "preset", -1);
    if (preset >= 0) {
        haptic_play_effect(HAPTIC_DOUBLE_CLICK);
        app_select_preset(preset, true);
    }
    int speaker = extract_json_int(body, "speaker", -1);
    if (speaker >= 0) {
        haptic_play_effect(HAPTIC_DOUBLE_CLICK);
        app_select_speaker(speaker);
    }
    int vol = extract_json_int(body, "volume", -1);
    if (vol >= 0 && vol <= 100) {
        app_set_volume(vol);
    }
    int km = extract_json_int(body, "knob_mode", -1);
    if (km >= 0 && km <= 2) {
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            g_app.knob_mode = (KnobMode)km;
            xSemaphoreGive(g_app.mutex);
        }
    }
    bool has_scan = false;
    if (extract_json_bool(body, "scan_speakers", &has_scan) && has_scan) {
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            g_app.speaker_scan_requested = true;
            xSemaphoreGive(g_app.mutex);
        }
    }
    bool has_dac = false;
    bool dac_val = extract_json_bool(body, "also_local_dac", &has_dac);
    if (has_dac) {
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            g_app.also_local_dac = dac_val;
            xSemaphoreGive(g_app.mutex);
        }
    }
    int tab = extract_json_int(body, "tab", -1);
    if (tab >= 0 && tab <= 3) {
        knob_ui_set_tab(tab);
    }
    int rot = extract_json_int(body, "rotate", 0);
    if (rot != 0) {
        knob_ui_on_rotate(rot);
    }
    s_server.send(200, "application/json", "{\"ok\":true}");
}

static void handle_screenshot_bmp() {
    const uint16_t *fb = bsp_get_screen_fb();
    if (!fb) {
        s_server.send(500, "text/plain", "No framebuffer");
        return;
    }

    const int w = EXAMPLE_LCD_H_RES;
    const int h = EXAMPLE_LCD_V_RES;
    const uint32_t row_bytes = w * 3;
    const uint32_t img_bytes = row_bytes * h;
    const uint32_t file_size = 54 + img_bytes;

    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (uint8_t)(file_size);
    hdr[3] = (uint8_t)(file_size >> 8);
    hdr[4] = (uint8_t)(file_size >> 16);
    hdr[5] = (uint8_t)(file_size >> 24);
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = (uint8_t)(w);
    hdr[19] = (uint8_t)(w >> 8);
    // Negative height (-360) for top-down BMP
    int32_t neg_h = -h;
    hdr[22] = (uint8_t)(neg_h);
    hdr[23] = (uint8_t)(neg_h >> 8);
    hdr[24] = (uint8_t)(neg_h >> 16);
    hdr[25] = (uint8_t)(neg_h >> 24);
    hdr[26] = 1;
    hdr[28] = 24; // 24-bit BGR

    WiFiClient client = s_server.client();
    client.print("HTTP/1.1 200 OK\r\nContent-Type: image/bmp\r\nContent-Length: ");
    client.print(file_size);
    client.print("\r\nConnection: close\r\n\r\n");
    client.write(hdr, 54);

    uint8_t *row_buf = (uint8_t *)malloc(row_bytes * 4);
    if (row_buf) {
        for (int y = 0; y < h; y += 4) {
            int lines = (y + 4 <= h) ? 4 : (h - y);
            uint8_t *dst = row_buf;
            for (int ly = 0; ly < lines; ly++) {
                const uint16_t *src = &fb[(y + ly) * w];
                for (int x = 0; x < w; x++) {
                    uint16_t sw = src[x];
                    uint16_t rgb565 = (sw >> 8) | (sw << 8);
                    uint8_t r = ((rgb565 >> 11) & 0x1F) * 255 / 31;
                    uint8_t g = ((rgb565 >> 5)  & 0x3F) * 255 / 63;
                    uint8_t b = ( rgb565        & 0x1F) * 255 / 31;
                    *dst++ = b;
                    *dst++ = g;
                    *dst++ = r;
                }
            }
            client.write(row_buf, row_bytes * lines);
        }
        free(row_buf);
    }
    client.stop();
}

static void handle_presets() {
    String body = s_server.arg("plain");
    String action = extract_json_str(body, "action");

    if (action == "reset") {
        app_state_reset_default_presets();
    } else if (action == "delete") {
        int idx = extract_json_int(body, "index", -1);
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (idx >= 0 && idx < g_app.preset_count && g_app.preset_count > 1) {
                for (int i = idx; i < g_app.preset_count - 1; i++) {
                    g_app.presets[i] = g_app.presets[i + 1];
                }
                g_app.preset_count--;
                if (g_app.current_preset >= g_app.preset_count) {
                    g_app.current_preset = g_app.preset_count - 1;
                }
            }
            xSemaphoreGive(g_app.mutex);
            app_state_save_presets();
        }
    } else if (action == "add" || action == "update") {
        String name = extract_json_str(body, "name");
        String genre = extract_json_str(body, "genre");
        String url = extract_json_str(body, "url");
        uint32_t color = (uint32_t)extract_json_int(body, "color", 0x10B981);
        int idx = extract_json_int(body, "index", -1);

        if (name.length() > 0 && url.length() > 0) {
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                int target = (action == "update" && idx >= 0 && idx < g_app.preset_count)
                             ? idx
                             : (g_app.preset_count < MAX_PRESETS ? g_app.preset_count++ : -1);
                if (target >= 0) {
                    snprintf(g_app.presets[target].id, sizeof(g_app.presets[target].id), "custom_%d", target);
                    strncpy(g_app.presets[target].name, name.c_str(), sizeof(g_app.presets[target].name) - 1);
                    strncpy(g_app.presets[target].genre, genre.c_str(), sizeof(g_app.presets[target].genre) - 1);
                    strncpy(g_app.presets[target].url, url.c_str(), sizeof(g_app.presets[target].url) - 1);
                    g_app.presets[target].color = color;
                }
                xSemaphoreGive(g_app.mutex);
                app_state_save_presets();
            }
        }
    }
    s_server.send(200, "application/json", "{\"ok\":true}");
}

static void handle_wifi() {
    String body = s_server.arg("plain");
    String ssid = extract_json_str(body, "ssid");
    String pass = extract_json_str(body, "pass");
    if (ssid.length() > 0) {
        app_state_save_wifi(ssid.c_str(), pass.c_str());
    }
    s_server.send(200, "application/json", "{\"ok\":true}");
}

static void handle_ota_done() {
    bool ok = !Update.hasError();
    s_server.sendHeader("Connection", "close");
    s_server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "FAIL");
    delay(400);
    ESP.restart();
}

static void handle_ota_upload() {
    HTTPUpload &upload = s_server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA] Web OTA Update Start: %s\n", upload.filename.c_str());
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_app.ota_in_progress = true;
            g_app.ota_progress_pct = 0;
            xSemaphoreGive(g_app.mutex);
        }
        Update.begin(UPDATE_SIZE_UNKNOWN);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        Update.write(upload.buf, upload.currentSize);
        if (Update.size() > 0) {
            int pct = (int)((Update.progress() * 100) / Update.size());
            if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                g_app.ota_progress_pct = pct;
                xSemaphoreGive(g_app.mutex);
            }
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("[OTA] Web OTA Success: %u bytes\n", (unsigned)upload.totalSize);
        } else {
            Update.printError(Serial);
        }
        if (xSemaphoreTake(g_app.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_app.ota_in_progress = false;
            xSemaphoreGive(g_app.mutex);
        }
    }
}

void web_portal_start(void) {
    s_server.on("/", HTTP_GET, handle_root);
    s_server.on("/api/status", HTTP_GET, handle_status);
    s_server.on("/api/screenshot.bmp", HTTP_GET, handle_screenshot_bmp);
    s_server.on("/api/control", HTTP_POST, handle_control);
    s_server.on("/api/presets", HTTP_POST, handle_presets);
    s_server.on("/api/wifi", HTTP_POST, handle_wifi);
    s_server.on("/api/ota", HTTP_POST, handle_ota_done, handle_ota_upload);
    s_server.begin();
    Serial.println("[WEB] Web Preset, AirPlay & OTA Manager listening on port 80");
}

void web_portal_poll(void) {
    s_server.handleClient();
}

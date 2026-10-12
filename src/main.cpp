/*
 * ESP32 Local Web Server + Wi-Fi Provisioning (captive portal)
 * Altrobyte Lab · Truck-07 example
 *
 * What it does
 *  1. On boot, reads saved Wi-Fi credentials from flash (NVS / Preferences).
 *  2. If credentials exist, tries to join that Wi-Fi (station mode).
 *  3. If there are none, or joining fails, it starts its own Wi-Fi network
 *     "Truck07-Setup" (password setup1234) with a captive portal:
 *     connect a phone, a setup page opens automatically, pick your network,
 *     type the password, Save. The ESP32 stores it and reboots.
 *  4. Once online, it runs a local dashboard at http://truck07.local
 *     (or its IP address) with live status and an LED switch.
 *  5. Hold the BOOT button for 3 seconds to forget the Wi-Fi (factory reset).
 *
 * Works with Arduino-ESP32 core 2.x and 3.x. No external libraries needed.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>

// ---------------- settings ----------------
static const char *AP_SSID   = "Truck07-Setup";
static const char *AP_PASS   = "setup1234";        // at least 8 characters
static const char *HOSTNAME  = "truck07";          // -> http://truck07.local
static const int   LED_PIN   = 2;                  // on-board LED on most DevKits
static const int   ldr   = 34;                 // GPIO 34 (ADC1) for LDR reading
static const int   RESET_BTN_PIN  = 0;                  // BOOT button (LOW when pressed)
static const uint32_t CONNECT_TIMEOUT_MS = 15000;  // give up joining after 15 s
static const uint32_t PORTAL_RETRY_MS    = 300000; // with saved creds, retry after 5 min in portal
static const uint32_t RESET_HOLD_MS      = 3000;   // hold BOOT 3 s to forget Wi-Fi

// ---------------- globals ----------------
WebServer   server(80);
DNSServer   dns;
Preferences prefs;

bool     portalMode  = false;
bool     hasSaved    = false;
bool     ledOn       = false;
uint32_t portalStart = 0;
String   scanJson    = "[]";    // cached scan result (scanning inside a request freezes the AP)
bool     scanning    = false;

// ---------------- web pages ----------------
static const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Truck-07 Setup</title>
<style>
body{font-family:system-ui,sans-serif;background:#E8EEF0;color:#14343F;margin:0;padding:20px}
.card{max-width:380px;margin:0 auto;background:#fff;border-radius:10px;padding:20px;box-shadow:0 2px 8px rgba(0,0,0,.08)}
h1{font-size:20px;margin:0 0 4px} p{color:#4D6975;font-size:14px;margin:0 0 14px}
label{font-size:13px;font-weight:600;display:block;margin:12px 0 4px}
select,input{width:100%;box-sizing:border-box;padding:10px;font-size:15px;border:1px solid #c5d0d4;border-radius:6px}
button{width:100%;margin-top:16px;padding:12px;font-size:16px;font-weight:600;background:#14343F;color:#fff;border:0;border-radius:6px}
.small{font-size:12px;color:#7C949D;margin-top:10px;text-align:center}
#rescan{background:none;color:#AE6529;margin-top:6px;padding:4px;font-size:13px}
</style></head><body><div class="card">
<h1>&#128666; Truck-07 Wi-Fi Setup</h1>
<p>Choose the Wi-Fi network this device should use.</p>
<form method="POST" action="/save">
<label>Network</label>
<select id="list" onchange="document.getElementById('ssid').value=this.value"><option>Scanning...</option></select>
<button type="button" id="rescan" onclick="scan(1)">&#8635; Scan again</button>
<label>Network name (SSID)</label>
<input id="ssid" name="ssid" maxlength="32" required>
<label>Password</label>
<input name="pass" type="password" maxlength="64" placeholder="leave empty for open networks">
<button type="submit">Save &amp; Connect</button>
</form>
<div class="small">Device ID: %CHIPID%</div>
</div>
<script>
function scan(r){
  var l=document.getElementById('list'); l.innerHTML='<option>Scanning...</option>';
  fetch(r?'/scan?r=1':'/scan').then(function(r){return r.json()}).then(function(n){
    l.innerHTML='<option value="">-- select --</option>';
    n.forEach(function(w){var o=document.createElement('option');o.value=w.ssid;
      o.textContent=w.ssid+'  ('+w.rssi+' dBm'+(w.open?', open':'')+')';l.appendChild(o);});
    if(r) setTimeout(function(){scan()},5000);
    if(!n.length) l.innerHTML='<option value="">No networks found</option>';
  }).catch(function(){l.innerHTML='<option value="">Scan failed, type the name below</option>'});
}
scan();
</script></body></html>
)rawliteral";

static const char SAVED_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Saved</title>
<style>body{font-family:system-ui,sans-serif;background:#E8EEF0;color:#14343F;padding:20px;text-align:center}
.card{max-width:380px;margin:40px auto;background:#fff;border-radius:10px;padding:24px}</style></head>
<body><div class="card"><h2>&#9989; Saved</h2>
<p>The device is restarting and joining <b>%SSID%</b>.</p>
<p>Reconnect your phone to your normal Wi-Fi, then open<br><b>http://truck07.local</b></p>
<p style="color:#7C949D;font-size:13px">If it can't connect, the "Truck07-Setup" network will appear again in about 20 seconds.</p>
</div></body></html>
)rawliteral";

static const char DASH_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Truck-07</title>
<style>
body{font-family:system-ui,sans-serif;background:#E8EEF0;color:#14343F;margin:0;padding:20px}
.card{max-width:420px;margin:0 auto;background:#fff;border-radius:10px;padding:20px}
h1{font-size:20px;margin:0 0 12px}
.row{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid #eef2f3;font-size:15px}
.row b{font-family:ui-monospace,monospace}
button{width:100%;margin-top:14px;padding:12px;font-size:16px;font-weight:600;border:0;border-radius:6px;color:#fff;background:#14343F}
#forget{background:#B93B2C;font-size:13px;padding:8px}
</style></head><body><div class="card">
<h1>&#128666; Truck-07 local dashboard</h1>
<div class="row"><span>Wi-Fi</span><b id="ssid">-</b></div>
<div class="row"><span>IP address</span><b id="ip">-</b></div>
<div class="row"><span>Signal</span><b id="rssi">-</b></div>
<div class="row"><span>Uptime</span><b id="up">-</b></div>
<div class="row"><span>Free memory</span><b id="heap">-</b></div>
<div class="row"><span>LDR (GPIO 34)</span><b id="ldr">-</b></div>
<div class="row"><span>LED (GPIO 2)</span><b id="led">-</b></div>
<button onclick="post('/api/led')">Toggle LED</button>
<button id="forget" onclick="if(confirm('Forget Wi-Fi and restart in setup mode?'))post('/api/forget')">Forget Wi-Fi</button>
</div>
<script>

function post(u) {
  fetch(u, { method: 'POST' }).then(load);
}

function load() {
  fetch('/api/status')
    .then(function(r) {
      return r.json();
    })
    .then(function(s) {
      document.getElementById('ssid').textContent = s.ssid;
      document.getElementById('ip').textContent = s.ip;
      document.getElementById('rssi').textContent = s.rssi + ' dBm';
      document.getElementById('up').textContent = s.uptime_s + ' s';
      document.getElementById('heap').textContent =
        Math.round(s.free_heap / 1024) + ' KB';
      document.getElementById('ldr').textContent = s.ldr;
      document.getElementById('led').textContent =
        s.led ? 'ON' : 'OFF';
    })
    .catch(function(error) {
      console.log('Dashboard update failed:', error);
    });
}

load();
setInterval(load, 2000);

</script></body></html>
)rawliteral";

// ---------------- helpers ----------------
String jsonEscape(const String &s) {
  String o;
  o.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) { o += ' '; }
    else o += c;
  }
  return o;
}

String htmlEscape(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

String chipId() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[13];
  snprintf(buf, sizeof(buf), "%04X%08lX", (unsigned)(uint16_t)(mac >> 32), (unsigned long)(uint32_t)mac);
  return String(buf);
}

void setLed(bool on) {
  ledOn = on;
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

void readLdr() {
  pinMode(34, INPUT);
  int ldr = analogRead(34);
  Serial.printf("[ldr] GPIO 34 = %d\n", ldr);
}

// ---------------- credential storage (NVS) ----------------
bool loadCreds(String &ssid, String &pass) {
  prefs.begin("wifi", true);                 // read-only
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
  return ssid.length() > 0;
}

void saveCreds(const String &ssid, const String &pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

void clearCreds() {
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
}

// ---------------- station mode ----------------
bool connectStation(const String &ssid, const String &pass) {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("[wifi] joining \"%s\"", ssid.c_str());   // never print the password

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
    setLed(!ledOn);                          // fast blink while connecting
  }
  setLed(false);
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] connected, IP %s, RSSI %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.printf("[wifi] failed (status %d)\n", WiFi.status());
  WiFi.disconnect(true);
  return false;
}

// ---------------- captive portal (setup mode) ----------------
void redirectToPortal() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

void handlePortalRoot() {
  String page = FPSTR(PORTAL_HTML);
  page.replace("%CHIPID%", chipId());
  server.send(200, "text/html", page);
}

String buildScanJson(int n) {
  String json = "[";
  int added = 0;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;           // skip hidden networks
    bool dup = false;                        // skip duplicates (mesh / repeaters)
    for (int j = 0; j < i; j++) if (WiFi.SSID(j) == s) { dup = true; break; }
    if (dup) continue;
    if (added++) json += ",";
    json += "{\"ssid\":\"" + jsonEscape(s) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
            ",\"open\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  json += "]";
  return json;
}

void handleScan() {
  if (server.hasArg("r") && !scanning) {     // async rescan, result is picked up in loop()
    WiFi.scanNetworks(true);
    scanning = true;
  }
  server.send(200, "application/json", scanJson);
}

void pollScan() {
  if (!scanning) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  if (n >= 0) scanJson = buildScanJson(n);
  WiFi.scanDelete();
  scanning = false;
}

void handleSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim();
  if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 64 ||
      (pass.length() > 0 && pass.length() < 8)) {
    server.send(400, "text/plain", "Invalid input: SSID 1-32 chars, password empty or 8-64 chars.");
    return;
  }
  saveCreds(ssid, pass);
  Serial.printf("[setup] saved network \"%s\", restarting\n", ssid.c_str());

  String page = FPSTR(SAVED_HTML);
  page.replace("%SSID%", htmlEscape(ssid));
  server.send(200, "text/html", page);
  delay(1500);                               // let the page reach the phone
  ESP.restart();
}

void startPortal() {
  portalMode  = true;
  portalStart = millis();

  WiFi.mode(WIFI_STA);                       // scan once BEFORE the AP is up (no channel hopping)
  WiFi.disconnect();
  delay(100);
  scanJson = buildScanJson(WiFi.scanNetworks());
  WiFi.scanDelete();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setSleep(false);
  delay(100);
  IPAddress ip = WiFi.softAPIP();            // 192.168.4.1
  dns.start(53, "*", ip);                    // answer EVERY name with our IP -> captive portal

  server.on("/", HTTP_GET, handlePortalRoot);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/save", HTTP_POST, handleSave);
  // OS connectivity checks -> redirect so the phone pops up the setup page
  server.on("/generate_204", redirectToPortal);         // Android
  server.on("/gen_204", redirectToPortal);              // Android (some)
  server.on("/hotspot-detect.html", redirectToPortal);  // Apple
  server.on("/connecttest.txt", redirectToPortal);      // Windows
  server.on("/ncsi.txt", redirectToPortal);             // Windows (older)
  server.onNotFound(redirectToPortal);
  server.begin();

  Serial.printf("[setup] portal running: join Wi-Fi \"%s\" (password %s), open http://%s\n",
                AP_SSID, AP_PASS, ip.toString().c_str());
}

// ---------------- local dashboard (normal mode) ----------------

void handleStatus() {
  int ldrValue = analogRead(34);

  String json = "{";
  json += "\"device\":\"" + chipId() + "\",";
  json += "\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"uptime_s\":" + String(millis() / 1000) + ",";
  json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"led\":" + String(ledOn ? "true" : "false") + ",";
  json += "\"ldr\":" + String(ldrValue);
  json += "}";

  server.send(200, "application/json", json);
}


void handleLed() {
  if (server.hasArg("state")) setLed(server.arg("state") == "on");
  else setLed(!ledOn);
  server.send(200, "application/json", String("{\"led\":") + (ledOn ? "true" : "false") + "}");
}

void handleldr() {
  readLdr();
  int ldr = analogRead(34);
  server.send(200, "application/json", String("{\"ldr\":") + ldr + "}");
}
void handleForget() {
  server.send(200, "application/json", "{\"ok\":true,\"next\":\"restarting in setup mode\"}");
  delay(500);
  clearCreds();
  ESP.restart();
}

void startDashboard() {
  portalMode = false;
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[web] open http://%s.local or http://%s\n",
                  HOSTNAME, WiFi.localIP().toString().c_str());
  }
  server.on("/", HTTP_GET, []() { server.send(200, "text/html", FPSTR(DASH_HTML)); });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/led", HTTP_POST, handleLed);
  server.on("/api/ldr", HTTP_GET, handleldr);
  server.on("/api/forget", HTTP_POST, handleForget);
  server.onNotFound([]() { server.send(404, "application/json", "{\"error\":\"not found\"}"); });
  server.begin();
}

// ---------------- factory reset button ----------------
void checkResetButton() {
  static uint32_t pressedAt = 0;
  if (digitalRead(RESET_BTN_PIN) == LOW) {
    if (pressedAt == 0) pressedAt = millis();
    if (millis() - pressedAt > RESET_HOLD_MS) {
      Serial.println("[reset] BOOT held: forgetting Wi-Fi and restarting");
      for (int i = 0; i < 6; i++) { setLed(!ledOn); delay(100); }
      clearCreds();
      ESP.restart();
    }
  } else {
    pressedAt = 0;
  }
}

// ---------------- Arduino entry points ----------------
void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);
  setLed(false);
  Serial.printf("\n[boot] Truck-07 device %s\n", chipId().c_str());

  String ssid, pass;
  hasSaved = loadCreds(ssid, pass);

  if (hasSaved && connectStation(ssid, pass)) {
    startDashboard();
  } else {
    if (!hasSaved) Serial.println("[boot] no saved Wi-Fi");
    startPortal();
  }
}

void loop() {
  if (portalMode) {
    dns.processNextRequest();
    pollScan();
    // Router was just down? With saved credentials, try again after a while.
    if (hasSaved && millis() - portalStart > PORTAL_RETRY_MS) {
      Serial.println("[setup] retrying saved Wi-Fi");
      ESP.restart();
    }
    // Slow blink = setup mode
    setLed((millis() / 500) % 2);
  }
  server.handleClient();
  checkResetButton();
}
#include "portal.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include "deck_ui.h"
#include "link.h"
#include "settings.h"

namespace {

enum class State { Idle, Connecting, Ok, Failed };

WebServer gServer(80);
DNSServer gDns;
State gState = State::Idle;
String gSsid, gPass, gError;
uint32_t gConnectStarted = 0;
uint32_t gRestartAt = 0;
String gApName;
String gScanOptions;

constexpr uint32_t kConnectTimeoutMs = 20000;

String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length());
  for (char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

String jsonEscape(const String& in) {
  String out;
  for (char c : in) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

void scan() {
  const int n = WiFi.scanNetworks();
  gScanOptions = "";
  for (int i = 0; i < n; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.length() == 0 || gScanOptions.indexOf(">" + htmlEscape(ssid) + "<") >= 0) continue;
    gScanOptions += "<option value=\"" + htmlEscape(ssid) + "\">" + htmlEscape(ssid) + "</option>";
  }
  WiFi.scanDelete();
}

const char kPageHead[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>CYD Stream Deck setup</title>
<style>
body{font-family:system-ui,sans-serif;background:#10141c;color:#e8ecf2;margin:0;padding:24px}
.card{max-width:420px;margin:auto;background:#1b2230;border-radius:14px;padding:24px}
h1{font-size:1.4em;margin:0 0 4px}p{color:#a9b3c4}
label{display:block;margin:16px 0 6px;font-weight:600}
input,select{width:100%;box-sizing:border-box;padding:12px;border-radius:8px;border:1px solid #3a465c;background:#0f141d;color:#fff;font-size:1em}
button{margin-top:20px;width:100%;padding:14px;border:0;border-radius:8px;background:#2f7cf6;color:#fff;font-size:1.05em;font-weight:600}
.row{display:flex;gap:8px;align-items:center;margin-top:8px;color:#a9b3c4}.row input{width:auto}
.err{background:#4a1d24;color:#ffb4bd;padding:10px;border-radius:8px}
a{color:#7cb4ff}
</style></head><body><div class="card">)HTML";

void handleRoot() {
  String html = FPSTR(kPageHead);
  html += "<h1>CYD Stream Deck</h1><p>Connect this device to your WiFi network.</p>";
  if (gState == State::Failed) html += "<p class=err>" + htmlEscape(gError) + "</p>";
  html += R"HTML(<form method="post" action="/connect">
<label for="pick">Network</label>
<select id="pick" onchange="document.getElementById('ssid').value=this.value">
<option value="">Choose a network...</option>)HTML";
  html += gScanOptions;
  html += R"HTML(</select>
<label for="ssid">Network name (SSID)</label>
<input id="ssid" name="ssid" required maxlength="32" autocomplete="off">
<label for="pass">Password</label>
<input id="pass" name="pass" type="password" maxlength="64">
<div class="row"><input type="checkbox" id="show" onclick="pass.type=this.checked?'text':'password'"><span>Show password</span></div>
<button type="submit">Connect</button></form>
<p><a href="/rescan">Scan again</a></p></div></body></html>)HTML";
  gServer.send(200, "text/html", html);
}

void handleConnect() {
  gSsid = gServer.arg("ssid");
  gPass = gServer.arg("pass");
  gSsid.trim();
  if (gSsid.length() == 0) {
    gServer.sendHeader("Location", "/");
    gServer.send(302);
    return;
  }
  gState = State::Connecting;
  gConnectStarted = millis();
  WiFi.disconnect(false, false);
  WiFi.begin(gSsid.c_str(), gPass.c_str());
  DeckUI::showMessage("WiFi setup", "Trying to connect to\n" + gSsid + " ...");

  String html = FPSTR(kPageHead);
  html += R"HTML(<h1>Connecting...</h1><p id="msg">Checking the credentials. This takes up to 20 seconds.</p>
<p>If this page stops responding, look at the device screen for the result.</p>
<script>
function poll(){fetch('/status').then(r=>r.json()).then(s=>{
 if(s.state=='ok'){document.getElementById('msg').innerHTML='Connected! Device IP address: <b>'+s.ip+'</b>. The device is restarting.';}
 else if(s.state=='failed'){location.href='/';}
 else setTimeout(poll,1000);}).catch(()=>setTimeout(poll,1500));}
setTimeout(poll,1500);
</script></div></body></html>)HTML";
  gServer.send(200, "text/html", html);
}

void handleStatus() {
  const char* s = gState == State::Ok ? "ok" : gState == State::Failed ? "failed"
                  : gState == State::Connecting ? "connecting" : "idle";
  String body = String("{\"state\":\"") + s + "\",\"ip\":\"" +
                (gState == State::Ok ? WiFi.localIP().toString() : String()) + "\",\"error\":\"" +
                jsonEscape(gError) + "\"}";
  gServer.send(200, "application/json", body);
}

void showSetupScreen() {
  DeckUI::showMessage(
      "WiFi setup",
      "No WiFi network is configured.\n\n"
      "1. On your phone or PC, join the WiFi network:\n"
      "       " + gApName + "\n"
      "2. The setup page should open automatically.\n"
      "    If not, browse to  http://192.168.4.1\n"
      "3. Choose your network and enter its password.\n\n"
      "(WiFi can also be set from the PC config app over USB.)");
}

void pollConnection() {
  if (gState != State::Connecting) return;
  const wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED) {
    gState = State::Ok;
    Settings::saveWifi(gSsid, gPass);
    gRestartAt = millis() + 5000;
    Serial.printf("[portal] connected, IP %s; credentials saved\n", WiFi.localIP().toString().c_str());
    DeckUI::showMessage("WiFi setup", "Connected to " + gSsid + "\nIP address: " +
                                           WiFi.localIP().toString() + "\n\nRestarting...");
    return;
  }
  if (millis() - gConnectStarted > kConnectTimeoutMs || st == WL_CONNECT_FAILED ||
      st == WL_NO_SSID_AVAIL) {
    gState = State::Failed;
    gError = st == WL_NO_SSID_AVAIL ? "Network \"" + gSsid + "\" was not found."
                                    : "Could not connect to \"" + gSsid + "\". Check the password.";
    Serial.printf("[portal] connect failed (status %d)\n", st);
    WiFi.disconnect(false, false);
    showSetupScreen();
  }
}

}  // namespace

namespace Portal {

void run() {
  char suffix[8];
  snprintf(suffix, sizeof(suffix), "%s", Settings::deviceId().c_str() + 6);
  gApName = String("CYD-StreamDeck-") + suffix;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(gApName.c_str());
  delay(200);
  scan();
  gDns.setErrorReplyCode(DNSReplyCode::NoError);
  gDns.start(53, "*", WiFi.softAPIP());

  gServer.on("/", HTTP_GET, handleRoot);
  gServer.on("/connect", HTTP_POST, handleConnect);
  gServer.on("/status", HTTP_GET, handleStatus);
  gServer.on("/rescan", HTTP_GET, [] {
    scan();
    gServer.sendHeader("Location", "/");
    gServer.send(302);
  });
  // Anything else (OS captive-portal probes etc.) goes to the setup page.
  gServer.onNotFound([] {
    gServer.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
    gServer.send(302);
  });
  gServer.begin();

  Serial.printf("[portal] AP '%s' up at %s\n", gApName.c_str(), WiFi.softAPIP().toString().c_str());
  showSetupScreen();

  for (;;) {
    gDns.processNextRequest();
    gServer.handleClient();
    Link::loopSerial();  // allows WiFi to be set over USB too
    pollConnection();
    if (gRestartAt && millis() > gRestartAt) ESP.restart();
    delay(5);
  }
}

}  // namespace Portal

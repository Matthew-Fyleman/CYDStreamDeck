#include "api_server.h"

#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>

#include "auth.h"
#include "deck_config.h"
#include "deck_ui.h"
#include "icon_store.h"
#include "link.h"
#include "settings.h"

namespace {

WebServer gServer(Deck::kHttpPort);
ApiServer::ChangeHandler gOnChange = nullptr;
ApiServer::PressHandler gOnPress = nullptr;
bool gStarted = false;
bool gUploadOk = false;
bool gUploadUnauthorized = false;
Auth::Sha256* gUploadHash = nullptr;

// No CORS headers: the configuration app is not a browser, and web pages must
// not be able to read or drive the API.
void sendJson(int code, const String& body) { gServer.send(code, "application/json", body); }

void sendResult(bool ok, const String& err = "") {
  if (ok) {
    sendJson(200, "{\"ok\":true}");
  } else {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = err;
    String out;
    serializeJson(doc, out);
    sendJson(400, out);
  }
}

// Rejects requests that did not come from the configuration app:
//  * Host must name this device, which defeats DNS rebinding.
//  * POSTs must carry X-CYDSD: 1 and no Origin. Browsers always send Origin on
//    cross-site POSTs and cannot add a custom header without a CORS preflight,
//    which this server never approves.
bool requestAllowed() {
  String host = gServer.hostHeader();
  if (host.endsWith(":80")) host.remove(host.length() - 3);
  const bool hostOk = host == WiFi.localIP().toString() ||
                      host.equalsIgnoreCase(Settings::deviceId()) ||
                      host.equalsIgnoreCase(Settings::deviceId() + ".local");
  if (!hostOk) return false;
  if (gServer.method() != HTTP_POST) return true;
  return !gServer.hasHeader("Origin") && gServer.header("X-CYDSD") == "1";
}

bool isJsonRequest() { return gServer.header("Content-Type").startsWith("application/json"); }

// Once paired, API requests must carry "X-CYDSD-Auth: <challenge> <mac>",
// where the challenge comes from GET /api/challenge and
//   mac = HMAC(key, "CYDSD-HTTP <method> <path> <pos or -> <challenge> <sha256 of body>")
// The challenge is single use, so a captured request cannot be replayed.
bool authorized(const String& bodySha) {
  if (!Auth::paired()) return true;
  const String h = gServer.header("X-CYDSD-Auth");
  const int sp = h.indexOf(' ');
  if (sp <= 0) return false;
  const String challenge = h.substring(0, sp);
  const String text = String("CYDSD-HTTP ") + (gServer.method() == HTTP_POST ? "POST " : "GET ") +
                      gServer.uri() + " " + (gServer.hasArg("pos") ? gServer.arg("pos") : String("-")) + " " +
                      challenge + " " + bodySha;
  return Auth::verify(text, h.substring(sp + 1)) && Auth::consumeChallenge(challenge);
}

String plainBodySha() {
  const String& body = gServer.arg("plain");
  return Auth::sha256Hex(reinterpret_cast<const uint8_t*>(body.c_str()), body.length());
}

// Returns true (after replying 403/401) if the request must not be handled.
// needAuth is false only where authentication was already done elsewhere.
bool reject(bool needAuth = true) {
  if (!requestAllowed()) {
    sendJson(403, "{\"ok\":false,\"error\":\"forbidden\"}");
    return true;
  }
  if (needAuth && !authorized(plainBodySha())) {
    sendJson(401, "{\"ok\":false,\"error\":\"unauthorized (pair this PC with the deck over USB)\"}");
    return true;
  }
  return false;
}

int posArg() {
  if (!gServer.hasArg("pos")) return -1;
  const int pos = gServer.arg("pos").toInt();
  return pos >= 0 && pos < Deck::kButtons ? pos : -1;
}

void handleRoot() {
  if (reject(false)) return;
  String html =
      "<!doctype html><html><head><meta name=viewport content='width=device-width'>"
      "<title>CYD Stream Deck</title><style>body{font-family:sans-serif;margin:2em;"
      "background:#111;color:#eee}code{color:#7cf}</style></head><body>"
      "<h1>CYD Stream Deck</h1><p>Device <code>" +
      Settings::deviceId() + "</code>, firmware " + Deck::kFirmwareVersion +
      "</p><p>Use the <b>CYD Stream Deck configuration application</b> on your PC to set up "
      "icons and actions.</p><p><a href='/api/info' style='color:#7cf'>Device info (JSON)</a></p>"
      "</body></html>";
  gServer.send(200, "text/html", html);
}

void handleSettings() {
  if (reject()) return;
  if (!isJsonRequest()) return sendResult(false, "expected application/json");
  String err;
  const bool ok = Settings::applyJson(gServer.arg("plain"), err);
  sendResult(ok, err);
  if (ok && gOnChange) gOnChange(-1);
}

void handleIconUpload() {
  HTTPUpload& up = gServer.upload();
  if (up.status == UPLOAD_FILE_START) {
    gUploadUnauthorized = false;
    delete gUploadHash;
    gUploadHash = new Auth::Sha256();
    gUploadOk = requestAllowed() && posArg() >= 0 && IconStore::beginWrite(posArg());
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (gUploadOk) {
      gUploadHash->update(up.buf, up.currentSize);
      gUploadOk = IconStore::write(up.buf, up.currentSize);
    }
  } else if (up.status == UPLOAD_FILE_END) {
    // The temporary file only replaces the icon if the MAC covers its bytes.
    if (gUploadOk && !authorized(gUploadHash->hex())) {
      gUploadOk = false;
      gUploadUnauthorized = true;
      IconStore::abortWrite();
    }
    if (gUploadOk) gUploadOk = IconStore::endWrite();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    IconStore::abortWrite();
    gUploadOk = false;
  }
}

void handleIconDone() {
  delete gUploadHash;
  gUploadHash = nullptr;
  if (reject(false)) return;  // authenticated in handleIconUpload against the file bytes
  if (gUploadUnauthorized) {
    gUploadUnauthorized = false;
    return sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
  }
  const int pos = posArg();
  sendResult(gUploadOk, "icon upload failed (expect " + String(Deck::kIconBytes) + " bytes RGB565)");
  if (gUploadOk && gOnChange) gOnChange(pos);
  gUploadOk = false;
}

void handleIconDelete() {
  if (reject()) return;
  const int pos = posArg();
  if (pos < 0) return sendResult(false, "bad pos");
  sendResult(IconStore::remove(pos), "delete failed");
  if (gOnChange) gOnChange(pos);
}

void handleWifi() {
  if (reject()) return;
  JsonDocument doc;
  if (!isJsonRequest() || deserializeJson(doc, gServer.arg("plain")) ||
      !doc["ssid"].is<const char*>() || doc["ssid"].as<String>().length() == 0) {
    return sendResult(false, "expected application/json {ssid, pass}");
  }
  const String ssid = doc["ssid"].as<String>();
  // Someone must be at the deck to accept a network change.
  const bool accepted = DeckUI::confirm(
      "Change WiFi?", "The PC asked this deck to join the network:\n\n    " + ssid +
                          "\n\nTap Yes to accept. No reply in 30 s means No.");
  if (!accepted) {
    DeckUI::redrawAll();
    JsonDocument err;
    err["ok"] = false;
    err["error"] = "declined on the deck";
    String out;
    serializeJson(err, out);
    return sendJson(403, out);
  }
  Settings::saveWifi(ssid, doc["pass"] | "");
  DeckUI::showMessage("WiFi", "Joining " + ssid + "\n\nRestarting...");
  sendResult(true);
  delay(500);
  ESP.restart();
}

#ifdef CYDSD_DEBUG_API
void handlePress() {
  if (reject()) return;
  const int pos = posArg();
  if (pos < 0) return sendResult(false, "bad pos");
  if (gOnPress) gOnPress(pos);
  sendResult(true);
}
#endif

}  // namespace

namespace ApiServer {

void begin(ChangeHandler onChange, PressHandler onPress) {
  gOnChange = onChange;
  gOnPress = onPress;
  if (gStarted) return;
  static const char* kHeaders[] = {"Origin", "X-CYDSD", "Content-Type", "X-CYDSD-Auth"};
  gServer.collectHeaders(kHeaders, sizeof(kHeaders) / sizeof(kHeaders[0]));
  gServer.on("/", HTTP_GET, handleRoot);
  gServer.on("/api/challenge", HTTP_GET, [] {
    if (!reject(false)) sendJson(200, "{\"challenge\":\"" + Auth::issueChallenge() + "\"}");
  });
  gServer.on("/api/info", HTTP_GET, [] {
    if (!reject()) sendJson(200, Link::infoJson());
  });
  gServer.on("/api/settings", HTTP_POST, handleSettings);
  gServer.on("/api/icon", HTTP_POST, handleIconDone, handleIconUpload);
  gServer.on("/api/icon/delete", HTTP_POST, handleIconDelete);
  gServer.on("/api/wifi", HTTP_POST, handleWifi);
#ifdef CYDSD_DEBUG_API
  gServer.on("/api/press", HTTP_POST, handlePress);  // simulated touch, for bench testing only
#endif
  gServer.onNotFound([] { gServer.send(404, "text/plain", "not found"); });
  gServer.begin();
  gStarted = true;
  Serial.printf("[api] HTTP server on port %u\n", Deck::kHttpPort);
}

void loop() {
  if (gStarted) gServer.handleClient();
}

}  // namespace ApiServer

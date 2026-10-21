#include "api_server.h"

#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>

#include "deck_config.h"
#include "icon_store.h"
#include "link.h"
#include "settings.h"

namespace {

WebServer gServer(Deck::kHttpPort);
ApiServer::ChangeHandler gOnChange = nullptr;
ApiServer::PressHandler gOnPress = nullptr;
bool gStarted = false;
bool gUploadOk = false;

void sendJson(int code, const String& body) {
  gServer.sendHeader("Access-Control-Allow-Origin", "*");
  gServer.send(code, "application/json", body);
}

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

int posArg() {
  if (!gServer.hasArg("pos")) return -1;
  const int pos = gServer.arg("pos").toInt();
  return pos >= 0 && pos < Deck::kButtons ? pos : -1;
}

void handleRoot() {
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
  String err;
  const bool ok = Settings::applyJson(gServer.arg("plain"), err);
  sendResult(ok, err);
  if (ok && gOnChange) gOnChange(-1);
}

void handleIconUpload() {
  HTTPUpload& up = gServer.upload();
  if (up.status == UPLOAD_FILE_START) {
    gUploadOk = posArg() >= 0 && IconStore::beginWrite(posArg());
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (gUploadOk) gUploadOk = IconStore::write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (gUploadOk) gUploadOk = IconStore::endWrite();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    IconStore::abortWrite();
    gUploadOk = false;
  }
}

void handleIconDone() {
  const int pos = posArg();
  sendResult(gUploadOk, "icon upload failed (expect " + String(Deck::kIconBytes) + " bytes RGB565)");
  if (gUploadOk && gOnChange) gOnChange(pos);
  gUploadOk = false;
}

void handleIconDelete() {
  const int pos = posArg();
  if (pos < 0) return sendResult(false, "bad pos");
  sendResult(IconStore::remove(pos), "delete failed");
  if (gOnChange) gOnChange(pos);
}

void handleWifi() {
  JsonDocument doc;
  if (deserializeJson(doc, gServer.arg("plain")) || !doc["ssid"].is<const char*>() ||
      doc["ssid"].as<String>().length() == 0) {
    return sendResult(false, "expected {ssid, pass}");
  }
  Settings::saveWifi(doc["ssid"].as<String>(), doc["pass"] | "");
  sendResult(true);
  delay(500);
  ESP.restart();
}

void handlePress() {
  const int pos = posArg();
  if (pos < 0) return sendResult(false, "bad pos");
  if (gOnPress) gOnPress(pos);
  sendResult(true);
}

}  // namespace

namespace ApiServer {

void begin(ChangeHandler onChange, PressHandler onPress) {
  gOnChange = onChange;
  gOnPress = onPress;
  if (gStarted) return;
  gServer.on("/", HTTP_GET, handleRoot);
  gServer.on("/api/info", HTTP_GET, [] { sendJson(200, Link::infoJson()); });
  gServer.on("/api/settings", HTTP_POST, handleSettings);
  gServer.on("/api/icon", HTTP_POST, handleIconDone, handleIconUpload);
  gServer.on("/api/icon/delete", HTTP_POST, handleIconDelete);
  gServer.on("/api/wifi", HTTP_POST, handleWifi);
  gServer.on("/api/press", HTTP_POST, handlePress);
  gServer.onNotFound([] { gServer.send(404, "text/plain", "not found"); });
  gServer.begin();
  gStarted = true;
  Serial.printf("[api] HTTP server on port %u\n", Deck::kHttpPort);
}

void loop() {
  if (gStarted) gServer.handleClient();
}

}  // namespace ApiServer

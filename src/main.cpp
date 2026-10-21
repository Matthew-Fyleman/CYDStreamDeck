// CYD Stream Deck firmware
// Hardware: LCDWiki/Hosyond 4.0" ESP32-32E (ST7796S 480x320 + XPT2046)

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>

#include "api_server.h"
#include "deck_config.h"
#include "deck_ui.h"
#include "display_init.h"
#include "icon_store.h"
#include "link.h"
#include "pins.h"
#include "portal.h"
#include "settings.h"

TFT_eSPI tft;
XPT2046_Touchscreen touch(Pins::kTouchCs, Pins::kTouchIrq);

namespace {

bool gOnline = false;
uint32_t gOfflineSince = 0;  // millis() when the connection was lost (or boot)
uint32_t gLastKick = 0;

void onPress(int pos) { Link::sendPress(pos); }

void onChange(int pos) {
  if (pos < 0) {
    DeckUI::redrawAll();
  } else {
    DeckUI::redrawIcon(pos);
  }
}

void initHardware() {
  pinMode(Pins::kTftBl, OUTPUT);
  digitalWrite(Pins::kTftBl, HIGH);
  pinMode(Pins::kTftCs, OUTPUT);
  pinMode(Pins::kTouchCs, OUTPUT);
  digitalWrite(Pins::kTftCs, HIGH);
  digitalWrite(Pins::kTouchCs, HIGH);

  SPI.begin(Pins::kSpiSck, Pins::kSpiMiso, Pins::kSpiMosi, Pins::kTftCs);

  tft.init();
  applyManufacturerSt7796Init(tft);
  tft.setRotation(1);
  tft.writecommand(0x36);  // MADCTL for landscape on this panel (from Clocky)
  tft.writedata(0x28);

  touch.begin(SPI);
}

void onWifiUp() {
  Serial.printf("[wifi] connected, IP %s\n", WiFi.localIP().toString().c_str());
  configTzTime(Settings::get().tz.c_str(), "pool.ntp.org", "time.google.com", "time.nist.gov");
  Link::startUdp();
  ApiServer::begin(onChange, onPress);
  DeckUI::redrawStatus();
}

// The core's auto-reconnect can give up for good, e.g. after the router
// restarts or changes the DHCP lease, so this watchdog restarts the
// connection itself and, as a last resort, the whole device.
void checkWifi() {
  const bool up = WiFi.isConnected();
  const uint32_t now = millis();
  if (up) {
    if (!gOnline) {
      gOnline = true;
      onWifiUp();
    }
    return;
  }
  if (gOnline) {
    gOnline = false;
    gOfflineSince = now;
    gLastKick = now;
    Serial.println("[wifi] connection lost, auto-reconnecting");
  }
  const uint32_t offline = now - gOfflineSince;
  if (offline >= Deck::kWifiRestartMs && IconStore::writingPos() < 0) {
    Serial.printf("[wifi] offline for %lu s, restarting\n", static_cast<unsigned long>(offline / 1000));
    Serial.flush();
    ESP.restart();
  }
  if (now - gLastKick >= Deck::kWifiKickMs) {
    gLastKick = now;
    Serial.printf("[wifi] still offline after %lu s, reconnecting\n", static_cast<unsigned long>(offline / 1000));
    WiFi.disconnect(false, false);
    WiFi.begin(Settings::wifiSsid().c_str(), Settings::wifiPass().c_str());
  }
}

}  // namespace

void setup() {
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[boot] CYD Stream Deck firmware %s\n", Deck::kFirmwareVersion);

  initHardware();
  Settings::begin();
  IconStore::begin();
  Link::begin(onChange);
  DeckUI::begin(tft, touch, onPress);
  Serial.printf("[boot] device id %s\n", Settings::deviceId().c_str());

  if (!Settings::hasWifi()) {
    Serial.println("[boot] no WiFi credentials stored, starting setup portal");
    Portal::run();
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(Settings::deviceId().c_str());
  WiFi.setAutoReconnect(true);
  WiFi.begin(Settings::wifiSsid().c_str(), Settings::wifiPass().c_str());
  WiFi.setSleep(false);  // lower latency for press events
  Serial.printf("[wifi] connecting to '%s'\n", Settings::wifiSsid().c_str());

  DeckUI::redrawAll();
}

void loop() {
  checkWifi();
  Link::loop();
  ApiServer::loop();
  DeckUI::loop();
  delay(5);
}

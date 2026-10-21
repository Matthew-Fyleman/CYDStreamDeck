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

void checkWifi() {
  const bool up = WiFi.isConnected();
  if (up && !gOnline) {
    gOnline = true;
    onWifiUp();
  } else if (!up && gOnline) {
    gOnline = false;
    Serial.println("[wifi] connection lost, auto-reconnecting");
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

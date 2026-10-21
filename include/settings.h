#pragma once

#include <Arduino.h>

#include "deck_config.h"

// Persistent settings held in NVS (Preferences). WiFi credentials live in
// their own namespace and are never reported back over any interface.
struct DeckSettings {
  String tz = "GMT0BST,M3.5.0/1,M10.5.0";  // POSIX TZ string
  String timeFmt = "%H:%M";                // strftime
  String dateFmt = "%a %d %b %Y";          // strftime
  String name = "CYD Stream Deck";
  String labels[Deck::kButtons];
};

namespace Settings {

void begin();
DeckSettings& get();
void save();

bool hasWifi();
String wifiSsid();
String wifiPass();
void saveWifi(const String& ssid, const String& pass);
void clearWifi();

// Apply a JSON settings document (from HTTP or serial). Returns false and sets
// err on validation failure. Saves and applies the timezone on success.
bool applyJson(const String& json, String& err);

void applyTimezone();

// Unique device id derived from the MAC, e.g. "CYDSD-A1B2C3".
const String& deviceId();

}  // namespace Settings

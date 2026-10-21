#include "settings.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <time.h>

namespace {

DeckSettings gSettings;
String gDeviceId;

constexpr const char* kNsSettings = "deck";
constexpr const char* kNsWifi = "wifi";

bool validFormat(const String& fmt) {
  return fmt.length() > 0 && fmt.length() <= 32;
}

}  // namespace

namespace Settings {

void begin() {
  Preferences prefs;
  prefs.begin(kNsSettings, false);
  gSettings.tz = prefs.getString("tz", gSettings.tz);
  gSettings.timeFmt = prefs.getString("tfmt", gSettings.timeFmt);
  gSettings.dateFmt = prefs.getString("dfmt", gSettings.dateFmt);
  gSettings.name = prefs.getString("name", gSettings.name);
  for (int i = 0; i < Deck::kButtons; ++i) {
    gSettings.labels[i] = prefs.getString(("l" + String(i)).c_str(), "");
  }
  prefs.end();

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char id[16];
  snprintf(id, sizeof(id), "CYDSD-%02X%02X%02X", mac[3], mac[4], mac[5]);
  gDeviceId = id;

  applyTimezone();
}

DeckSettings& get() { return gSettings; }

void save() {
  Preferences prefs;
  prefs.begin(kNsSettings, false);
  prefs.putString("tz", gSettings.tz);
  prefs.putString("tfmt", gSettings.timeFmt);
  prefs.putString("dfmt", gSettings.dateFmt);
  prefs.putString("name", gSettings.name);
  for (int i = 0; i < Deck::kButtons; ++i) {
    prefs.putString(("l" + String(i)).c_str(), gSettings.labels[i]);
  }
  prefs.end();
}

bool hasWifi() { return wifiSsid().length() > 0; }

// Opened read-write so a missing namespace is created rather than logged as an error.
String wifiSsid() {
  Preferences prefs;
  prefs.begin(kNsWifi, false);
  String ssid = prefs.getString("ssid", "");
  prefs.end();
  return ssid;
}

String wifiPass() {
  Preferences prefs;
  prefs.begin(kNsWifi, false);
  String pass = prefs.getString("pass", "");
  prefs.end();
  return pass;
}

void saveWifi(const String& ssid, const String& pass) {
  Preferences prefs;
  prefs.begin(kNsWifi, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

void clearWifi() {
  Preferences prefs;
  prefs.begin(kNsWifi, false);
  prefs.clear();
  prefs.end();
}

bool applyJson(const String& json, String& err) {
  JsonDocument doc;
  DeserializationError de = deserializeJson(doc, json);
  if (de) {
    err = String("bad json: ") + de.c_str();
    return false;
  }

  DeckSettings next = gSettings;
  if (doc["tz"].is<const char*>()) next.tz = doc["tz"].as<String>();
  if (doc["time_fmt"].is<const char*>()) next.timeFmt = doc["time_fmt"].as<String>();
  if (doc["date_fmt"].is<const char*>()) next.dateFmt = doc["date_fmt"].as<String>();
  if (doc["name"].is<const char*>()) next.name = doc["name"].as<String>().substring(0, 32);
  JsonArrayConst labels = doc["labels"].as<JsonArrayConst>();
  if (!labels.isNull()) {
    for (int i = 0; i < Deck::kButtons; ++i) {
      next.labels[i] = i < static_cast<int>(labels.size()) ? labels[i].as<String>().substring(0, 24) : "";
    }
  }

  if (next.tz.length() == 0 || next.tz.length() > 64) {
    err = "invalid tz";
    return false;
  }
  if (!validFormat(next.timeFmt) || !validFormat(next.dateFmt)) {
    err = "invalid time/date format";
    return false;
  }

  gSettings = next;
  save();
  applyTimezone();
  return true;
}

void applyTimezone() {
  setenv("TZ", gSettings.tz.c_str(), 1);
  tzset();
}

const String& deviceId() { return gDeviceId; }

}  // namespace Settings

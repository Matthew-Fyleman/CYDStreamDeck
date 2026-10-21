#include "link.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <mbedtls/base64.h>
#include <time.h>

#include "auth.h"
#include "deck_config.h"
#include "deck_ui.h"
#include "icon_store.h"
#include "settings.h"

namespace {

Link::ChangeHandler gOnChange = nullptr;
WiFiUDP gUdp;
bool gUdpStarted = false;

// A daemon that asked for presses. When paired, each subscriber is identified
// by the nonce its daemon chose at start-up, and its SUBSCRIBE timestamps must
// keep increasing so a captured SUBSCRIBE cannot be replayed.
struct Subscriber {
  IPAddress ip;
  uint16_t port = 0;
  uint32_t lastSeen = 0;
  String nonce;  // empty for unauthenticated (unpaired) subscribers
  uint32_t lastTs = 0;
};
constexpr int kMaxSubscribers = 4;
Subscriber gSubs[kMaxSubscribers];

struct Pending {
  uint32_t seq = 0;
  int pos = -1;
  uint32_t sentAt = 0;
  uint8_t tries = 0;
  bool active = false;
};
constexpr int kMaxPending = 4;
constexpr uint8_t kMaxTries = 3;
constexpr uint32_t kRetryMs = 250;
Pending gPending[kMaxPending];
uint32_t gSeq = 0;

uint32_t gLastSerialPing = 0;
constexpr uint32_t kSerialTtlMs = 30000;

String gSerialLine;
constexpr size_t kMaxSerialLine = 1500;

void notify(int pos) {
  if (gOnChange) gOnChange(pos);
}

String hereReply() {
  const String ip = WiFi.isConnected() ? WiFi.localIP().toString() : String("0.0.0.0");
  return String(Deck::kProtoTag) + " HERE " + Settings::deviceId() + " " + ip + " " +
         Deck::kFirmwareVersion + " " + Settings::get().name;
}

void serialReply(const String& msg) {
  Serial.print(Deck::kProtoTag);
  Serial.print(' ');
  Serial.println(msg);
}

void udpSend(const IPAddress& ip, uint16_t port, const String& msg) {
  if (!gUdpStarted) return;
  gUdp.beginPacket(ip, port);
  gUdp.write(reinterpret_cast<const uint8_t*>(msg.c_str()), msg.length());
  gUdp.endPacket();
}

void clearSubscribers() {
  for (Subscriber& s : gSubs) s = Subscriber();
}

// nonce is empty for an unauthenticated subscription.
void subscribe(const IPAddress& ip, uint16_t port, const String& nonce = String(), uint32_t ts = 0) {
  const uint32_t now = millis();
  int slot = -1;
  for (int i = 0; i < kMaxSubscribers; ++i) {
    const bool same = nonce.length() ? gSubs[i].nonce == nonce
                                     : gSubs[i].nonce.length() == 0 && gSubs[i].port == port && gSubs[i].ip == ip;
    if (gSubs[i].port && same) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    // Reuse an empty/expired slot, else the oldest.
    slot = 0;
    for (int i = 0; i < kMaxSubscribers; ++i) {
      if (gSubs[i].port == 0 || now - gSubs[i].lastSeen > Deck::kSubscriberTtlMs) {
        slot = i;
        break;
      }
      if (gSubs[i].lastSeen < gSubs[slot].lastSeen) slot = i;
    }
    gSubs[slot] = Subscriber();
    gSubs[slot].nonce = nonce;
    Serial.printf("[link] subscriber %s:%u%s\n", ip.toString().c_str(), port,
                  nonce.length() ? " (authenticated)" : "");
  }
  gSubs[slot].ip = ip;
  gSubs[slot].port = port;
  gSubs[slot].lastTs = ts;
  gSubs[slot].lastSeen = now;
}

const Subscriber* subscriberByNonce(const String& nonce) {
  const uint32_t now = millis();
  for (const Subscriber& s : gSubs) {
    if (s.port && s.nonce.length() && s.nonce == nonce && now - s.lastSeen <= Deck::kSubscriberTtlMs) return &s;
  }
  return nullptr;
}

// Splits on single spaces into at most maxParts fields. Returns the count.
int split(const String& in, String* parts, int maxParts) {
  int n = 0, start = 0;
  while (n < maxParts && start <= static_cast<int>(in.length())) {
    int sp = in.indexOf(' ', start);
    if (sp < 0 || n == maxParts - 1) sp = in.length();
    parts[n++] = in.substring(start, sp);
    start = sp + 1;
  }
  return n;
}

bool isHex(const String& s, size_t len) {
  if (s.length() != len) return false;
  for (char c : s) {
    if (!isxdigit(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

void transmitPress(const Pending& p) {
  // USB is trusted (physical access), so the serial copy is never signed.
  const String msg = String(Deck::kProtoTag) + " PRESS " + Settings::deviceId() + " " +
                     String(p.pos) + " " + String(p.seq);
  Serial.println(msg);
  const uint32_t now = millis();
  for (const Subscriber& s : gSubs) {
    if (!s.port || now - s.lastSeen > Deck::kSubscriberTtlMs) continue;
    if (s.nonce.length()) {
      const String text = msg + " " + s.nonce + " " + Auth::bootId();
      udpSend(s.ip, s.port, text + " " + Auth::mac(text));
    } else if (!Auth::paired()) {
      udpSend(s.ip, s.port, msg);
    }
  }
}

void handleAck(uint32_t seq) {
  for (Pending& p : gPending) {
    if (p.active && p.seq == seq) {
      p.active = false;
      DeckUI::onPressAcked(p.pos);
    }
  }
}

// Splits "CYDSD VERB rest" -> verb, rest. Returns false if not ours.
bool parse(const String& line, String& verb, String& rest) {
  const String tag = String(Deck::kProtoTag) + " ";
  if (!line.startsWith(tag)) return false;
  String body = line.substring(tag.length());
  body.trim();
  const int sp = body.indexOf(' ');
  verb = sp < 0 ? body : body.substring(0, sp);
  rest = sp < 0 ? String() : body.substring(sp + 1);
  rest.trim();
  return true;
}

// Signed UDP commands, used once the device is paired:
//   CYDSD SUBSCRIBE <id> <nonce> <ts> <mac>
//     -> CYDSD SUBSCRIBED <id> <nonce> <ts> <boot> <mac>
//   CYDSD ACK <seq> <boot> <nonce> <mac>
void handleSignedUdp(const String& line, const String& verb, const String& rest) {
  String text;
  if (!Auth::verifySigned(line, text)) return;  // silently drop forgeries
  String f[5];
  if (verb == "SUBSCRIBE") {
    if (split(rest, f, 5) != 4 || !f[0].equalsIgnoreCase(Settings::deviceId()) || !isHex(f[1], 16)) return;
    const uint32_t ts = strtoul(f[2].c_str(), nullptr, 10);
    const time_t now = time(nullptr);
    if (now > 1700000000 && labs(static_cast<long>(now) - static_cast<long>(ts)) > 120) return;
    String nonce = f[1];
    nonce.toLowerCase();
    const Subscriber* existing = subscriberByNonce(nonce);
    if (existing && ts <= existing->lastTs) return;  // replayed
    subscribe(gUdp.remoteIP(), gUdp.remotePort(), nonce, ts);
    const String reply = String(Deck::kProtoTag) + " SUBSCRIBED " + Settings::deviceId() + " " + nonce + " " +
                         f[2] + " " + Auth::bootId();
    udpSend(gUdp.remoteIP(), gUdp.remotePort(), reply + " " + Auth::mac(reply));
  } else if (verb == "ACK") {
    if (split(rest, f, 5) != 4 || f[1] != Auth::bootId()) return;
    String nonce = f[2];
    nonce.toLowerCase();
    if (!subscriberByNonce(nonce)) return;
    handleAck(strtoul(f[0].c_str(), nullptr, 10));
  }
}

// Commands common to UDP and serial. Returns true if handled.
bool handleCommon(const String& verb, const String& rest, String& reply, bool* subscribeReq) {
  if (verb == "DISCOVER") {
    reply = hereReply();
    return true;
  }
  if (verb == "SUBSCRIBE") {
    if (rest == "*" || rest.length() == 0 || rest.equalsIgnoreCase(Settings::deviceId())) {
      if (subscribeReq) *subscribeReq = true;
      reply = String(Deck::kProtoTag) + " SUBSCRIBED " + Settings::deviceId();
    }
    return true;
  }
  if (verb == "ACK") {
    handleAck(strtoul(rest.c_str(), nullptr, 10));
    return true;
  }
  return false;
}

void handleUdp() {
  if (!gUdpStarted) return;
  int size;
  while ((size = gUdp.parsePacket()) > 0) {
    char buf[256];
    const int n = gUdp.read(buf, sizeof(buf) - 1);
    if (n <= 0) continue;
    buf[n] = 0;
    String line(buf);
    line.trim();
    String verb, rest, reply;
    if (!parse(line, verb, rest)) continue;
    if (Auth::paired() && verb != "DISCOVER") {
      handleSignedUdp(line, verb, rest);
      continue;
    }
    bool sub = false;
    handleCommon(verb, rest, reply, &sub);
    if (sub) subscribe(gUdp.remoteIP(), gUdp.remotePort());
    if (reply.length()) udpSend(gUdp.remoteIP(), gUdp.remotePort(), reply);
  }
}

void handleSerialLine(const String& line) {
  String verb, rest, reply;
  if (!parse(line, verb, rest)) return;
  gLastSerialPing = millis();

  if (handleCommon(verb, rest, reply, nullptr)) {
    if (reply.length()) Serial.println(reply);
    return;
  }
  if (verb == "HELLO") {
    const String ip = WiFi.isConnected() ? WiFi.localIP().toString() : String("0.0.0.0");
    serialReply("ID " + Settings::deviceId() + " " + Deck::kFirmwareVersion + " " + ip);
  } else if (verb == "PING") {
    serialReply("PONG");
  } else if (verb == "INFO") {
    serialReply("INFO " + Link::infoJson());
  } else if (verb == "SET") {
    String err;
    if (Settings::applyJson(rest, err)) {
      serialReply("OK");
      notify(-1);
    } else {
      serialReply("ERR " + err);
    }
  } else if (verb == "WIFI") {
    JsonDocument doc;
    if (deserializeJson(doc, rest) || !doc["ssid"].is<const char*>() ||
        doc["ssid"].as<String>().length() == 0) {
      serialReply("ERR bad wifi json");
      return;
    }
    Settings::saveWifi(doc["ssid"].as<String>(), doc["pass"] | "");
    serialReply("OK");
    Serial.flush();
    delay(500);
    ESP.restart();
  } else if (verb == "ICON") {
    serialReply(IconStore::beginWrite(rest.toInt()) && rest.length() ? "OK" : "ERR bad position");
  } else if (verb == "DATA") {
    uint8_t buf[1100];
    size_t n = 0;
    if (mbedtls_base64_decode(buf, sizeof(buf), &n, reinterpret_cast<const uint8_t*>(rest.c_str()),
                              rest.length()) != 0 ||
        !IconStore::write(buf, n)) {
      IconStore::abortWrite();
      serialReply("ERR data");
      return;
    }
    serialReply("OK");
  } else if (verb == "END") {
    const int pos = IconStore::writingPos();
    if (IconStore::endWrite()) {
      serialReply("OK");
      notify(pos);
    } else {
      serialReply("ERR incomplete icon");
    }
  } else if (verb == "PAIR") {
    uint8_t key[64];
    size_t n = 0;
    if (mbedtls_base64_decode(key, sizeof(key), &n, reinterpret_cast<const uint8_t*>(rest.c_str()),
                              rest.length()) != 0 ||
        !Auth::setKey(key, n)) {
      serialReply("ERR expected base64 of a 32-byte key");
      return;
    }
    memset(key, 0, sizeof(key));
    clearSubscribers();  // unauthenticated subscribers must re-subscribe with the key
    Serial.println("[link] paired");
    serialReply("OK");
  } else if (verb == "UNPAIR") {
    Auth::clearKey();
    clearSubscribers();
    Serial.println("[link] unpaired");
    serialReply("OK");
  } else if (verb == "DELICON") {
    const int pos = rest.toInt();
    IconStore::remove(pos);
    serialReply("OK");
    notify(pos);
#ifdef CYDSD_DEBUG_API
  } else if (verb == "DROPWIFI") {
    // Leaves the network without auto-reconnect, to exercise the WiFi watchdog.
    WiFi.disconnect(false, false);
    serialReply("OK");
#endif
  } else {
    serialReply("ERR unknown command");
  }
}

}  // namespace

namespace Link {

void begin(ChangeHandler onChange) { gOnChange = onChange; }

void startUdp() {
  if (gUdpStarted) return;
  gUdpStarted = gUdp.begin(Deck::kUdpPort);
  Serial.printf("[link] UDP listening on %u: %s\n", Deck::kUdpPort, gUdpStarted ? "ok" : "FAILED");
}

void loopSerial() {
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      if (gSerialLine.length()) {
        handleSerialLine(gSerialLine);
        gSerialLine = "";
      }
    } else if (gSerialLine.length() < kMaxSerialLine) {
      gSerialLine += c;
    }
  }
}

void loop() {
  loopSerial();
  handleUdp();
  const uint32_t now = millis();
  for (Pending& p : gPending) {
    if (!p.active || now - p.sentAt < kRetryMs) continue;
    if (p.tries >= kMaxTries) {
      p.active = false;
      continue;
    }
    p.tries++;
    p.sentAt = now;
    transmitPress(p);
  }
}

void sendPress(int pos) {
  // Use a free slot, else overwrite the oldest.
  Pending* slot = &gPending[0];
  for (Pending& p : gPending) {
    if (!p.active) {
      slot = &p;
      break;
    }
    if (p.sentAt < slot->sentAt) slot = &p;
  }
  slot->seq = ++gSeq;
  slot->pos = pos;
  slot->sentAt = millis();
  slot->tries = 1;
  slot->active = true;
  Serial.printf("[link] press %d seq %lu\n", pos, static_cast<unsigned long>(slot->seq));
  transmitPress(*slot);
}

bool pcConnected() {
  const uint32_t now = millis();
  if (gLastSerialPing && now - gLastSerialPing <= kSerialTtlMs) return true;
  for (const Subscriber& s : gSubs) {
    if (s.port && now - s.lastSeen <= Deck::kSubscriberTtlMs) return true;
  }
  return false;
}

String infoJson() {
  JsonDocument doc;
  const DeckSettings& s = Settings::get();
  doc["id"] = Settings::deviceId();
  doc["name"] = s.name;
  doc["fw"] = Deck::kFirmwareVersion;
  doc["ip"] = WiFi.isConnected() ? WiFi.localIP().toString() : String("");
  doc["ssid"] = Settings::wifiSsid();
  doc["rssi"] = WiFi.isConnected() ? WiFi.RSSI() : 0;
  doc["icon_size"] = Deck::kImageSize;
  doc["rows"] = Deck::kRows;
  doc["cols"] = Deck::kCols;
  doc["tz"] = s.tz;
  doc["time_fmt"] = s.timeFmt;
  doc["date_fmt"] = s.dateFmt;
  doc["time"] = static_cast<uint32_t>(time(nullptr));
  doc["pc_connected"] = pcConnected();
  doc["paired"] = Auth::paired();
  JsonArray icons = doc["icons"].to<JsonArray>();
  JsonArray labels = doc["labels"].to<JsonArray>();
  for (int i = 0; i < Deck::kButtons; ++i) {
    icons.add(IconStore::exists(i));
    labels.add(s.labels[i]);
  }
  String out;
  serializeJson(doc, out);
  return out;
}

}  // namespace Link

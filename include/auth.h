#pragma once

#include <Arduino.h>

// Pairing key and message authentication shared by the UDP link and the HTTP
// API. Until a key is stored (CYDSD PAIR over USB) the device accepts the
// unauthenticated protocol; once paired, WiFi traffic must carry a MAC:
//   mac = hex(HMAC-SHA256(key, text)[0:16]), appended as the last field.
namespace Auth {

constexpr size_t kKeyBytes = 32;
constexpr size_t kMacHexLen = 32;

void begin();  // loads the key, picks the boot id
bool paired();
bool setKey(const uint8_t* key, size_t len);  // stores in NVS
void clearKey();

// Random per boot, 8 hex chars. Lets the PC reject presses replayed from an
// earlier boot (the press sequence number restarts at 1 on every boot).
const String& bootId();

String mac(const String& text);
// Constant-time check of a hex MAC against text. False if not paired.
bool verify(const String& text, const String& macHex);
// Splits "<text> <mac>" at the last space and verifies it.
bool verifySigned(const String& line, String& text);

String sha256Hex(const uint8_t* data, size_t len);

// Incremental SHA-256, for streamed icon uploads.
class Sha256 {
 public:
  Sha256();
  ~Sha256();
  void update(const uint8_t* data, size_t len);
  String hex();

 private:
  void* ctx_;
};

String randomHex(size_t bytes);

// One-time HTTP challenges: issued by GET /api/challenge, valid for 60 s,
// consumed by the first request that presents them.
String issueChallenge();
bool consumeChallenge(const String& challenge);

}  // namespace Auth

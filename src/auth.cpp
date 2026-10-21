#include "auth.h"

#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/md.h>

namespace {

constexpr const char* kNsPair = "pair";

uint8_t gKey[Auth::kKeyBytes];
bool gPaired = false;
String gBootId;

struct Challenge {
  String value;
  uint32_t issuedAt = 0;
};
constexpr int kMaxChallenges = 4;
constexpr uint32_t kChallengeTtlMs = 60000;
Challenge gChallenges[kMaxChallenges];

String toHex(const uint8_t* data, size_t len) {
  static const char kDigits[] = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += kDigits[data[i] >> 4];
    out += kDigits[data[i] & 0x0F];
  }
  return out;
}

bool ctEqual(const String& a, const String& b) {
  if (a.length() != b.length()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.length(); ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

const mbedtls_md_info_t* sha256Info() { return mbedtls_md_info_from_type(MBEDTLS_MD_SHA256); }

}  // namespace

namespace Auth {

void begin() {
  Preferences prefs;
  prefs.begin(kNsPair, false);
  gPaired = prefs.getBytes("key", gKey, sizeof(gKey)) == sizeof(gKey);
  prefs.end();
  gBootId = randomHex(4);
}

bool paired() { return gPaired; }

bool setKey(const uint8_t* key, size_t len) {
  if (len != kKeyBytes) return false;
  Preferences prefs;
  prefs.begin(kNsPair, false);
  const bool ok = prefs.putBytes("key", key, len) == len;
  prefs.end();
  if (ok) {
    memcpy(gKey, key, len);
    gPaired = true;
  }
  return ok;
}

void clearKey() {
  Preferences prefs;
  prefs.begin(kNsPair, false);
  prefs.clear();
  prefs.end();
  memset(gKey, 0, sizeof(gKey));
  gPaired = false;
}

const String& bootId() { return gBootId; }

String mac(const String& text) {
  uint8_t out[32];
  mbedtls_md_hmac(sha256Info(), gKey, sizeof(gKey), reinterpret_cast<const uint8_t*>(text.c_str()),
                  text.length(), out);
  return toHex(out, kMacHexLen / 2);
}

bool verify(const String& text, const String& macHex) {
  if (!gPaired) return false;
  String lower = macHex;
  lower.toLowerCase();
  return ctEqual(mac(text), lower);
}

bool verifySigned(const String& line, String& text) {
  const int sp = line.lastIndexOf(' ');
  if (sp < 0) return false;
  text = line.substring(0, sp);
  return verify(text, line.substring(sp + 1));
}

String sha256Hex(const uint8_t* data, size_t len) {
  Sha256 h;
  h.update(data, len);
  return h.hex();
}

Sha256::Sha256() {
  auto* ctx = new mbedtls_md_context_t;
  mbedtls_md_init(ctx);
  mbedtls_md_setup(ctx, sha256Info(), 0);
  mbedtls_md_starts(ctx);
  ctx_ = ctx;
}

Sha256::~Sha256() {
  auto* ctx = static_cast<mbedtls_md_context_t*>(ctx_);
  mbedtls_md_free(ctx);
  delete ctx;
}

void Sha256::update(const uint8_t* data, size_t len) {
  mbedtls_md_update(static_cast<mbedtls_md_context_t*>(ctx_), data, len);
}

String Sha256::hex() {
  uint8_t out[32];
  mbedtls_md_finish(static_cast<mbedtls_md_context_t*>(ctx_), out);
  return toHex(out, sizeof(out));
}

String randomHex(size_t bytes) {
  uint8_t buf[32];
  bytes = min(bytes, sizeof(buf));
  esp_fill_random(buf, bytes);  // hardware RNG; true random while the radio is on
  return toHex(buf, bytes);
}

String issueChallenge() {
  const uint32_t now = millis();
  // Reuse an expired slot, else the oldest.
  Challenge* slot = &gChallenges[0];
  for (Challenge& c : gChallenges) {
    if (c.value.length() == 0 || now - c.issuedAt > kChallengeTtlMs) {
      slot = &c;
      break;
    }
    if (c.issuedAt < slot->issuedAt) slot = &c;
  }
  slot->value = randomHex(16);
  slot->issuedAt = now;
  return slot->value;
}

bool consumeChallenge(const String& challenge) {
  const uint32_t now = millis();
  for (Challenge& c : gChallenges) {
    if (c.value.length() && now - c.issuedAt <= kChallengeTtlMs && ctEqual(c.value, challenge)) {
      c.value = "";
      return true;
    }
  }
  return false;
}

}  // namespace Auth

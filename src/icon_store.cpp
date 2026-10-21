#include "icon_store.h"

#include <LittleFS.h>
#include <sys/stat.h>

#include "deck_config.h"

namespace {

File gFile;
int gPos = -1;
size_t gWritten = 0;

constexpr const char* kTmpPath = "/icons/upload.tmp";

String pathFor(int pos) { return "/icons/" + String(pos) + ".bin"; }

bool validPos(int pos) { return pos >= 0 && pos < Deck::kButtons; }

// LittleFS.exists() logs an error for missing files on this core; stat() is silent.
bool fileExists(const String& path) {
  struct stat st;
  return stat(("/littlefs" + path).c_str(), &st) == 0;
}

}  // namespace

namespace IconStore {

bool begin() {
  if (!LittleFS.begin(true)) {
    Serial.println("[icons] LittleFS mount failed");
    return false;
  }
  if (!fileExists("/icons")) LittleFS.mkdir("/icons");
  return true;
}

bool exists(int pos) { return validPos(pos) && fileExists(pathFor(pos)); }

bool remove(int pos) {
  if (!validPos(pos)) return false;
  if (!fileExists(pathFor(pos))) return true;
  return LittleFS.remove(pathFor(pos));
}

bool load(int pos, uint8_t* buf) {
  if (!exists(pos)) return false;
  File f = LittleFS.open(pathFor(pos), "r");
  if (!f) return false;
  const size_t n = f.read(buf, Deck::kIconBytes);
  f.close();
  return n == Deck::kIconBytes;
}

bool beginWrite(int pos) {
  abortWrite();
  if (!validPos(pos)) return false;
  gFile = LittleFS.open(kTmpPath, "w");
  if (!gFile) return false;
  gPos = pos;
  gWritten = 0;
  return true;
}

bool write(const uint8_t* data, size_t len) {
  if (gPos < 0) return false;
  if (gWritten + len > Deck::kIconBytes) {
    abortWrite();
    return false;
  }
  const size_t n = gFile.write(data, len);
  gWritten += n;
  return n == len;
}

bool endWrite() {
  if (gPos < 0) return false;
  gFile.close();
  const int pos = gPos;
  gPos = -1;
  if (gWritten != Deck::kIconBytes) {
    LittleFS.remove(kTmpPath);
    return false;
  }
  if (fileExists(pathFor(pos))) LittleFS.remove(pathFor(pos));
  return LittleFS.rename(kTmpPath, pathFor(pos));
}

void abortWrite() {
  if (gPos >= 0) {
    gFile.close();
    LittleFS.remove(kTmpPath);
  }
  gPos = -1;
  gWritten = 0;
}

int writingPos() { return gPos; }

}  // namespace IconStore

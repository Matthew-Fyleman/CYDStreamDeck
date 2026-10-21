#pragma once

#include <Arduino.h>

// Icons are stored in LittleFS as raw big-endian RGB565, Deck::kImageSize
// square, one file per grid position: /icons/<pos>.bin
namespace IconStore {

bool begin();
bool exists(int pos);
bool remove(int pos);

// Reads the icon into buf (must hold Deck::kIconBytes). Returns false if absent.
bool load(int pos, uint8_t* buf);

// Incremental writer used by both the HTTP and serial upload paths. Data is
// written to a temp file and only renamed into place once complete.
bool beginWrite(int pos);
bool write(const uint8_t* data, size_t len);
bool endWrite();   // validates size, commits
void abortWrite();
int writingPos();  // -1 when idle

}  // namespace IconStore

#include "deck_ui.h"

#include <WiFi.h>
#include <time.h>

#include "deck_config.h"
#include "icon_store.h"
#include "link.h"
#include "settings.h"
#include "touch_map.h"

namespace {

TFT_eSPI* gTft = nullptr;
XPT2046_Touchscreen* gTouch = nullptr;
DeckUI::PressHandler gOnPress = nullptr;
uint8_t* gIconBuf = nullptr;

constexpr uint16_t kBg = TFT_BLACK;
constexpr uint16_t kTileBg = 0x2945;      // dark slate
constexpr uint16_t kTileText = 0xC618;    // light grey
constexpr uint16_t kTileHint = 0x632C;    // dim grey
constexpr uint16_t kStatusBg = 0x10A2;    // near-black blue
constexpr uint16_t kStatusText = TFT_WHITE;
constexpr uint16_t kLinkOk = 0x07E8;
constexpr uint16_t kLinkOff = 0x528A;

// Bevel palettes: highlight edge, shadow edge, bevel face. The shadow must
// stay clearly distinct from the black background, otherwise the bottom/right
// border looks thinner than the top/left one and the image appears off-centre.
enum class Face { Normal, Pressed, Acked };
struct BevelColors {
  uint16_t light, dark, face;
};
constexpr BevelColors kBevelNormal = {0xA534, 0x3186, 0x5AEE};   // slate, raised
constexpr BevelColors kBevelPressed = {0xFFFF, 0x6B4D, 0xBDF7};  // bright, sunken
constexpr BevelColors kBevelAcked = {0x9FF3, 0x0300, 0x0600};    // green, sunken
constexpr uint16_t kRecessLine = 0x0841;  // uniform outline around the image
constexpr int16_t kOuterRadius = 12;
constexpr int16_t kEdge = 2;  // width of the highlight/shadow edges

// Touch state.
bool gTouchDown = false;
uint8_t gReleaseCount = 0;
uint32_t gDownAt = 0;
int gDownTarget = -2;  // -1 = status row, 0..11 = icon, -2 = none

// Press flash.
int gFlashPos = -1;
bool gFlashAcked = false;
uint32_t gFlashAt = 0;

// Status row.
uint32_t gIpUntil = 0;
String gLastStatusText;
bool gLastLinkOk = false;
uint32_t gLastStatusCheck = 0;
bool gHoldCountdown = false;

void iconRect(int pos, int16_t& x, int16_t& y) {
  const int row = pos / Deck::kCols;
  const int col = pos % Deck::kCols;
  x = Deck::kGridX + col * (Deck::kIconSize + Deck::kGap);
  y = Deck::kGridY + row * (Deck::kIconSize + Deck::kGap);
}

// Returns icon index under the point, -1 for the status row, -2 for nothing.
int hitTest(int16_t px, int16_t py) {
  if (py >= Deck::kStatusY) return -1;
  for (int i = 0; i < Deck::kButtons; ++i) {
    int16_t x, y;
    iconRect(i, x, y);
    if (px >= x && px < x + Deck::kIconSize && py >= y && py < y + Deck::kIconSize) return i;
  }
  return -2;
}

// Draws the label (or "row,col") into the recess at ix, iy.
void drawPlaceholder(int pos, int16_t ix, int16_t iy) {
  TFT_eSPI& tft = *gTft;
  const int16_t cx = ix + Deck::kImageSize / 2;
  const int16_t cy = iy + Deck::kImageSize / 2;
  tft.fillRect(ix, iy, Deck::kImageSize, Deck::kImageSize, kTileBg);
  tft.setTextDatum(MC_DATUM);
  const String& label = Settings::get().labels[pos];
  if (label.length() == 0) {
    tft.setTextColor(kTileHint, kTileBg);
    tft.drawString(String(pos / Deck::kCols + 1) + "," + String(pos % Deck::kCols + 1), cx, cy, 2);
    return;
  }
  tft.setTextColor(kTileText, kTileBg);
  if (tft.textWidth(label, 2) <= Deck::kImageSize - 6) {
    tft.drawString(label, cx, cy, 2);
    return;
  }
  // Two lines, split at the space nearest the middle.
  const int len = label.length();
  int split = -1;
  for (int i = 0; i < len; ++i) {
    if (label[i] != ' ') continue;
    if (split < 0 || abs(i - len / 2) < abs(split - len / 2)) split = i;
  }
  const bool atSpace = split >= 0;
  if (!atSpace) split = len / 2;
  tft.drawString(label.substring(0, split), cx, cy - 9, 2);
  tft.drawString(label.substring(atSpace ? split + 1 : split), cx, cy + 9, 2);
}

// Rounded pseudo-3D bevel filling exactly the icon cell, so the gaps between
// icons are untouched. Raised when idle (light top-left, shadow bottom-right),
// sunken while pressed. The image recess gets a uniform dark outline so the
// visible border is the same width on all four sides.
void drawBevel(int16_t x, int16_t y, Face face) {
  TFT_eSPI& tft = *gTft;
  const BevelColors& c = face == Face::Normal ? kBevelNormal
                         : face == Face::Pressed ? kBevelPressed : kBevelAcked;
  const bool raised = face == Face::Normal;
  const uint16_t topLeft = raised ? c.light : c.dark;
  const uint16_t bottomRight = raised ? c.dark : c.light;
  const int16_t s = Deck::kIconSize;

  tft.fillSmoothRoundRect(x, y, s, s, kOuterRadius, bottomRight, kBg);
  tft.fillSmoothRoundRect(x, y, s - kEdge - 1, s - kEdge - 1, kOuterRadius, topLeft, bottomRight);
  tft.fillSmoothRoundRect(x + kEdge, y + kEdge, s - 2 * kEdge, s - 2 * kEdge, kOuterRadius - kEdge,
                          c.face, topLeft);

  tft.drawRect(x + Deck::kBevel - 1, y + Deck::kBevel - 1, Deck::kImageSize + 2, Deck::kImageSize + 2,
               kRecessLine);
}

void drawIcon(int pos, Face face) {
  if (pos < 0 || pos >= Deck::kButtons) return;
  int16_t x, y;
  iconRect(pos, x, y);
  drawBevel(x, y, face);
  const int16_t ix = x + Deck::kBevel;
  const int16_t iy = y + Deck::kBevel;
  if (gIconBuf && IconStore::load(pos, gIconBuf)) {
    gTft->pushImage(ix, iy, Deck::kImageSize, Deck::kImageSize, reinterpret_cast<uint16_t*>(gIconBuf));
  } else {
    drawPlaceholder(pos, ix, iy);
  }
}

String formatNow(const String& fmt) {
  const time_t now = time(nullptr);
  if (now < 1700000000) return "";  // not synced yet
  struct tm tmNow;
  localtime_r(&now, &tmNow);
  char buf[48];
  strftime(buf, sizeof(buf), fmt.c_str(), &tmNow);
  return buf;
}

String statusText() {
  const uint32_t now = millis();
  if (gHoldCountdown) {
    const uint32_t held = now - gDownAt;
    const uint32_t left =
        held >= Deck::kWifiResetHoldMs ? 0 : (Deck::kWifiResetHoldMs - held + 999) / 1000;
    return "Keep holding to reset WiFi: " + String(left) + "s";
  }
  if (gIpUntil && now < gIpUntil) {
    const String ip = WiFi.isConnected() ? WiFi.localIP().toString() : String("not connected");
    return "IP " + ip + "   " + Settings::deviceId();
  }
  gIpUntil = 0;
  const String t = formatNow(Settings::get().timeFmt);
  if (t.length() == 0) return WiFi.isConnected() ? "Syncing time..." : "Connecting to WiFi...";
  return t + "    " + formatNow(Settings::get().dateFmt);
}

void drawStatus(bool force) {
  const String text = statusText();
  const bool linkOk = Link::pcConnected();
  if (!force && text == gLastStatusText && linkOk == gLastLinkOk) return;
  gLastStatusText = text;
  gLastLinkOk = linkOk;

  TFT_eSPI& tft = *gTft;
  tft.fillRect(0, Deck::kStatusY, Deck::kScreenW, Deck::kStatusH, kStatusBg);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(kStatusText, kStatusBg);
  tft.drawString(text, Deck::kScreenW / 2, Deck::kStatusY + Deck::kStatusH / 2, 2);
  // PC link indicator at the right-hand end of the row.
  tft.fillCircle(Deck::kScreenW - 12, Deck::kStatusY + Deck::kStatusH / 2, 4,
                 linkOk ? kLinkOk : kLinkOff);
}

void handleTouch() {
  int16_t x = 0, y = 0;
  const bool pressed = DeckUI::readTouch(x, y);
  const uint32_t now = millis();

  if (pressed) {
    gReleaseCount = 0;
    if (!gTouchDown) {
      gTouchDown = true;
      gDownAt = now;
      gDownTarget = hitTest(x, y);
      if (gDownTarget >= 0) {
        if (gFlashPos >= 0 && gFlashPos != gDownTarget) drawIcon(gFlashPos, Face::Normal);
        gFlashPos = gDownTarget;
        gFlashAt = now;
        gFlashAcked = false;
        drawIcon(gDownTarget, Face::Pressed);
        if (gOnPress) gOnPress(gDownTarget);
      } else if (gDownTarget == -1) {
        gIpUntil = now + Deck::kIpShowMs;
        drawStatus(true);
      }
    } else if (gDownTarget == -1) {
      const uint32_t held = now - gDownAt;
      if (held > 3000) gHoldCountdown = true;
      if (held >= Deck::kWifiResetHoldMs) {
        DeckUI::showMessage("WiFi reset",
                            "Stored WiFi credentials cleared.\nRestarting into setup mode...");
        Settings::clearWifi();
        delay(1500);
        ESP.restart();
      }
    }
    return;
  }

  // Require a few consecutive "not pressed" samples to debounce release.
  if (gTouchDown && ++gReleaseCount >= 3) {
    gTouchDown = false;
    gDownTarget = -2;
    if (gHoldCountdown) {
      gHoldCountdown = false;
      drawStatus(true);
    }
  }
}

}  // namespace

namespace DeckUI {

void begin(TFT_eSPI& tft, XPT2046_Touchscreen& touch, PressHandler onPress) {
  gTft = &tft;
  gTouch = &touch;
  gOnPress = onPress;
  if (!gIconBuf) gIconBuf = static_cast<uint8_t*>(malloc(Deck::kIconBytes));
  tft.setSwapBytes(false);  // icon files are stored big-endian (display byte order)
}

bool readTouch(int16_t& x, int16_t& y) {
  if (!gTouch->tirqTouched() || !gTouch->touched()) return false;
  const TS_Point raw = gTouch->getPoint();
  const TouchCal::Sample s = TouchCal::mapRaw(raw.x, raw.y, raw.z, true);
  x = s.x;
  y = s.y;
  return true;
}

void redrawIcon(int pos) {
  Face face = Face::Normal;
  if (pos == gFlashPos) face = gFlashAcked ? Face::Acked : Face::Pressed;
  drawIcon(pos, face);
}

void redrawStatus() { drawStatus(true); }

void redrawAll() {
  gTft->fillScreen(kBg);
  for (int i = 0; i < Deck::kButtons; ++i) redrawIcon(i);
  drawStatus(true);
}

void onPressAcked(int pos) {
  if (pos != gFlashPos || gFlashAcked) return;
  gFlashAcked = true;
  drawIcon(pos, Face::Acked);
}

void showMessage(const char* title, const String& body) {
  TFT_eSPI& tft = *gTft;
  tft.fillScreen(kBg);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, kBg);
  tft.drawString(title, 16, 16, 4);
  tft.setTextColor(TFT_WHITE, kBg);
  int16_t y = 60;
  int start = 0;
  while (start <= static_cast<int>(body.length())) {
    int nl = body.indexOf('\n', start);
    if (nl < 0) nl = body.length();
    tft.drawString(body.substring(start, nl), 16, y, 2);
    y += 22;
    start = nl + 1;
  }
}

bool confirm(const char* title, const String& body, uint32_t timeoutMs) {
  TFT_eSPI& tft = *gTft;
  showMessage(title, body);
  constexpr int16_t kBtnW = 160, kBtnH = 56, kBtnY = 240;
  constexpr int16_t kNoX = 60, kYesX = Deck::kScreenW - 60 - kBtnW;
  tft.setTextDatum(MC_DATUM);
  tft.fillSmoothRoundRect(kNoX, kBtnY, kBtnW, kBtnH, 10, 0x8000, kBg);
  tft.setTextColor(TFT_WHITE, 0x8000);
  tft.drawString("No", kNoX + kBtnW / 2, kBtnY + kBtnH / 2, 4);
  tft.fillSmoothRoundRect(kYesX, kBtnY, kBtnW, kBtnH, 10, 0x0400, kBg);
  tft.setTextColor(TFT_WHITE, 0x0400);
  tft.drawString("Yes", kYesX + kBtnW / 2, kBtnY + kBtnH / 2, 4);

  auto inBtn = [](int16_t x, int16_t y, int16_t bx) {
    return x >= bx && x < bx + kBtnW && y >= kBtnY && y < kBtnY + kBtnH;
  };
  const uint32_t start = millis();
  int choice = -1;  // 0 = No, 1 = Yes
  while (choice < 0 && millis() - start < timeoutMs) {
    int16_t x, y;
    if (readTouch(x, y)) {
      if (inBtn(x, y, kYesX)) choice = 1;
      else if (inBtn(x, y, kNoX)) choice = 0;
    }
    delay(10);
  }
  // Wait for the finger to lift so the tap is not seen again as a grid press.
  uint8_t released = 0;
  const uint32_t liftStart = millis();
  while (released < 5 && millis() - liftStart < 3000) {
    int16_t x, y;
    released = readTouch(x, y) ? 0 : released + 1;
    delay(10);
  }
  gTouchDown = false;
  gDownTarget = -2;
  return choice == 1;
}

void loop() {
  handleTouch();
  const uint32_t now = millis();
  if (gFlashPos >= 0 && now - gFlashAt >= Deck::kPressFlashMs) {
    const int pos = gFlashPos;
    gFlashPos = -1;
    drawIcon(pos, Face::Normal);
  }
  if (now - gLastStatusCheck >= 250) {
    gLastStatusCheck = now;
    drawStatus(false);
  }
}

}  // namespace DeckUI

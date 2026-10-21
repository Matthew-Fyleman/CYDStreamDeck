#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// Icon grid, status row and touch handling.
namespace DeckUI {

using PressHandler = void (*)(int pos);

void begin(TFT_eSPI& tft, XPT2046_Touchscreen& touch, PressHandler onPress);
void loop();

void redrawAll();
void redrawIcon(int pos);
void redrawStatus();

// Called by the link layer when the PC acknowledged a press.
void onPressAcked(int pos);

// Full-screen text page (used by setup portal and boot messages).
void showMessage(const char* title, const String& body);

// Raw touch sampling, mapped to landscape pixels.
bool readTouch(int16_t& x, int16_t& y);

}  // namespace DeckUI

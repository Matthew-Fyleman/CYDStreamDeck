#pragma once

#include <cstdint>

// Compile-time constants shared by all firmware modules.
namespace Deck {

constexpr const char* kFirmwareVersion = "1.0.0";

// Display geometry (landscape, setRotation(1)).
constexpr int16_t kScreenW = 480;
constexpr int16_t kScreenH = 320;

// Icon grid: 3 rows x 4 columns of square icons.
constexpr int kRows = 3;
constexpr int kCols = 4;
constexpr int kButtons = kRows * kCols;
constexpr int16_t kIconSize = 96;   // pixels, square
constexpr int16_t kGap = 4;         // spacing between icons (<= 5 px per spec)
constexpr int16_t kGridW = kCols * kIconSize + (kCols - 1) * kGap;  // 396
constexpr int16_t kGridH = kRows * kIconSize + (kRows - 1) * kGap;  // 296
constexpr int16_t kGridX = (kScreenW - kGridW) / 2;                 // 42
constexpr int16_t kGridY = 2;

// Status (time/date) row at the bottom, 20 px high, below the grid.
constexpr int16_t kStatusH = 20;
constexpr int16_t kStatusY = kScreenH - kStatusH;  // 300
static_assert(kGridY + kGridH <= kStatusY, "grid overlaps status row");

constexpr uint32_t kIconBytes = static_cast<uint32_t>(kIconSize) * kIconSize * 2;  // RGB565

// Networking.
constexpr uint16_t kUdpPort = 47800;
constexpr uint16_t kHttpPort = 80;
constexpr const char* kProtoTag = "CYDSD";

// Timings (ms).
constexpr uint32_t kPressFlashMs = 300;
constexpr uint32_t kIpShowMs = 10000;
constexpr uint32_t kWifiResetHoldMs = 10000;
constexpr uint32_t kSubscriberTtlMs = 60000;

}  // namespace Deck

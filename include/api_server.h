#pragma once

// HTTP configuration API, available once the device is on the local WiFi.
//   GET  /                       human-readable status page
//   GET  /api/info               device info JSON
//   POST /api/settings           JSON {tz, time_fmt, date_fmt, name, labels[12]}
//   POST /api/icon?pos=N         multipart upload of a raw RGB565 icon file
//   POST /api/icon/delete?pos=N  remove an icon
//   POST /api/wifi               JSON {ssid, pass}; device restarts
//   POST /api/press?pos=N        simulate a press (testing)
namespace ApiServer {

using ChangeHandler = void (*)(int pos);
using PressHandler = void (*)(int pos);

void begin(ChangeHandler onChange, PressHandler onPress);
void loop();

}  // namespace ApiServer

#pragma once

// HTTP configuration API, available once the device is on the local WiFi.
//   GET  /                       human-readable status page
//   GET  /api/info               device info JSON
//   POST /api/settings           JSON {tz, time_fmt, date_fmt, name, labels[12]}
//   POST /api/icon?pos=N         multipart upload of a raw RGB565 icon file
//   POST /api/icon/delete?pos=N  remove an icon
//   POST /api/wifi               JSON {ssid, pass}; confirmed on the touchscreen, then restarts
//   POST /api/press?pos=N        simulate a press (only built with -D CYDSD_DEBUG_API)
//   GET  /api/challenge          {"challenge": hex}; single use, valid 60 s
// Every request must have a Host header naming the device. POSTs must also
// carry "X-CYDSD: 1" and no Origin header, so web pages cannot use the API.
// Once paired, every /api/ request except /api/challenge must also carry
// X-CYDSD-Auth (see authorized() in api_server.cpp), else 401.
namespace ApiServer {

using ChangeHandler = void (*)(int pos);
using PressHandler = void (*)(int pos);

void begin(ChangeHandler onChange, PressHandler onPress);
void loop();

}  // namespace ApiServer

#pragma once

#include <Arduino.h>

// PC link: UDP over WiFi plus a line protocol over the USB serial port.
// Every message starts with "CYDSD " so the PC can ignore debug output.
//
// PC -> device (UDP and serial):
//   CYDSD DISCOVER                 -> CYDSD HERE <id> <ip> <fw> <name>
//   CYDSD SUBSCRIBE <id|*>         -> CYDSD SUBSCRIBED <id>   (UDP: sender gets presses)
//   CYDSD ACK <seq>
// PC -> device (serial only):
//   CYDSD HELLO                    -> CYDSD ID <id> <fw> <ip>
//   CYDSD PING                     -> CYDSD PONG
//   CYDSD INFO                     -> CYDSD INFO <json>
//   CYDSD SET <json>               -> CYDSD OK | CYDSD ERR <msg>
//   CYDSD WIFI <json>              -> CYDSD OK, then restart
//   CYDSD ICON <pos>               -> CYDSD OK   (begin icon upload)
//   CYDSD DATA <base64>            -> CYDSD OK   (icon bytes)
//   CYDSD END                      -> CYDSD OK | CYDSD ERR <msg>
//   CYDSD DELICON <pos>            -> CYDSD OK
//   CYDSD PAIR <base64 key>        -> CYDSD OK   (32-byte key; turns on authentication)
//   CYDSD UNPAIR                   -> CYDSD OK
// Device -> PC:
//   CYDSD PRESS <id> <pos> <seq>   (UDP to every subscriber, and on serial)
//
// Once paired, everything on UDP except DISCOVER must be signed (see auth.h):
//   CYDSD SUBSCRIBE <id> <nonce> <ts> <mac>
//     -> CYDSD SUBSCRIBED <id> <nonce> <ts> <boot> <mac>
//   CYDSD PRESS <id> <pos> <seq> <nonce> <boot> <mac>   (one per subscriber)
//   CYDSD ACK <seq> <boot> <nonce> <mac>
// Serial stays unsigned: USB already implies physical access.
namespace Link {

// pos is the changed icon, or -1 when settings changed.
using ChangeHandler = void (*)(int pos);

void begin(ChangeHandler onChange);
void startUdp();  // call once WiFi is connected
void loop();      // full processing (serial + UDP + retries)
void loopSerial();

void sendPress(int pos);
bool pcConnected();

String infoJson();

}  // namespace Link

# CYD Stream Deck: Functional Specification (FSD)

| | |
|---|---|
| Version | 1.1 |
| Date | 2026-10-21 |
| Firmware version | 1.1.0 |
| Repository | https://github.com/Matthew-Fyleman/CYDStreamDeck |

## 1. Purpose

The CYD Stream Deck turns a 4-inch ESP32 "Cheap Yellow Display" (CYD) into a 12-button touch launcher for a Windows 11 PC. Touching an icon on the CYD makes the PC run the actions configured for that icon. The actions can launch a program (with or without arguments), open a URL in the default browser, open a file or folder, or wait between steps.

The system has three parts:

| Component | Location | Runs on |
|---|---|---|
| Firmware | `src/`, `include/`, `platformio.ini` | ESP32 in the CYD |
| Daemon | `pc/cydsd_daemon.py` | Windows 11, in the background |
| Configuration application | `pc/cydsd_config.py` | Windows 11, only when configuring |
| Shared library | `pc/cydsd_common.py` | Used by the daemon and the configuration application |

## 2. Hardware

The hardware details below come from the `Clocky` project (`..\Clocky`), which runs on the same unit.

| Item | Value |
|---|---|
| Board | LCDWiki/Hosyond 4.0" ESP32-32E (E32R40T), ESP32-D0WD, 4 MB flash |
| Display | ST7796S, 320×480 native, used in landscape at **480×320** |
| Touch | XPT2046 resistive touch controller |
| SPI bus (shared) | SCK 14, MOSI 13, MISO 12 |
| Display | CS 15, DC 2, RST tied to EN (not driven), backlight 27 (active high) |
| Touch | CS 33, IRQ 36 (active low, input only) |
| Display init | Manufacturer ST7796 init sequence, `setRotation(1)`, then MADCTL = `0x28` |
| Touch calibration | raw 250–3850 on both axes, both axes inverted, no swap (`include/touch_map.h`) |
| USB serial | CH340-class bridge, COM4 on the development PC, 115200 baud |

## 3. Firmware

### 3.1 Boot sequence

1. Initialise GPIO, the shared SPI bus, the display and the touch controller.
2. Load settings from NVS and mount LittleFS (the icon store).
3. **If no WiFi credentials are stored**, start the setup portal (§3.2).
4. **Otherwise**, connect to the stored network in station mode with auto-reconnect, draw the button grid, and start the UDP link (§5.1), the HTTP API (§5.3) and NTP time sync once connected.

### 3.2 WiFi setup portal (first boot)

* The device opens an open access point named `CYD-StreamDeck-XXXXXX`, where XXXXXX is the last 3 bytes of the MAC address. A DNS server answers every name with the device's address, and unknown URLs redirect to the setup page, so phones and PCs open the page automatically as a captive portal. The page is also at `http://192.168.4.1`.
* The screen shows step-by-step instructions.
* The page has:
  * a list of nearby networks from a WiFi scan, sorted by signal strength
  * a field for the SSID, for hidden networks
  * a password field with a "show password" option
  * a link to scan again
* On submit, the device tries the credentials (AP+STA mode, 20 s timeout) and reports progress on the page and on the screen.
  * **If the credentials are verified**, they are saved to NVS and the device restarts after 5 s.
  * **If they fail**, the page shows the reason ("network not found" or "check the password"), and the user can try again.
* While the portal is running, WiFi can also be set over USB by the configuration application (the `WIFI` serial command).

### 3.3 Credential storage

The credentials are stored in a dedicated NVS namespace (`wifi`). No interface (HTTP, UDP or serial) ever reports the password back. The PC never stores the credentials either; the configuration application only sends them.

> **Note:** NVS is not encrypted unless ESP32 flash encryption is enabled. Flash encryption burns eFuses irreversibly and makes reflashing over USB harder, so it is out of scope. Someone with physical access to the device could read the flash.

### 3.4 Screen layout (landscape 480×320)

```
x: 42                                                      438
   +--------+ +--------+ +--------+ +--------+    y=2
   |  0     | |  1     | |  2     | |  3     |    96×96 cells
   +--------+ +--------+ +--------+ +--------+    4 px gaps
   |  4     | |  5     | |  6     | |  7     |
   +--------+ +--------+ +--------+ +--------+
   |  8     | |  9     | | 10     | | 11     |    ...y=298
   +--------+ +--------+ +--------+ +--------+
[        17:45    Wed 21 Oct 2026                    ●]  y=300..319 (20 px)
```

* The grid has 3 rows × 4 columns of **96×96 px** square cells, with a **4 px** gap horizontally and vertically. The grid is centred horizontally.
* Each cell is drawn as a **rounded pseudo-3D button**. The bevel sits entirely inside the 96×96 cell, so the 4 px gaps stay clear background. The bevel has:
  * an anti-aliased outer edge with a 12 px corner radius
  * a 2 px light edge on the top and left, and a 2 px shadow edge on the bottom and right (a raised look)
  * a slate face
  * a uniform 1 px dark outline around the image recess, so the border looks the same width on all four sides
* The icon image sits in an **84×84 px** recess inside the 6 px bevel.
* Each position has an index from 0 to 11, counted row by row (`pos = row*4 + col`).
* An icon with no image is drawn as a rounded tile showing its label, wrapped onto up to two lines, or its "row,col" if it has no label.
* The **status row** is 20 px high and sits below the grid without overlapping it. It shows the time (hours and minutes) and the date, using the configured time zone and formats. Before NTP sync it shows "Connecting to WiFi..." or "Syncing time...".
* A dot at the right-hand end of the status row shows the PC link: green when a daemon has subscribed in the last 60 s or pinged over USB in the last 30 s, grey otherwise.

### 3.5 Touch behaviour

| Touch | Result |
|---|---|
| Touch inside an icon | The press is sent to the PC immediately, on touch-down. The bevel flips to **sunken** (light on the bottom and right) with a bright face, and the face turns **green** when the PC acknowledges the press. The button returns to its raised state after 300 ms. The highlight is never sticky. |
| Touch in a gap or margin | Ignored |
| Tap in the status row | Shows `IP a.b.c.d   CYDSD-XXXXXX` for 10 s |
| Hold the status row for 10 s | After 3 s a countdown appears. At 10 s the stored WiFi credentials are erased and the device restarts into the setup portal. This is the recovery path if the WiFi changes. |

A new press is only accepted after the finger has been lifted, so holding does not auto-repeat.

### 3.6 Settings held on the device (NVS namespace `deck`)

| Key | Meaning | Default |
|---|---|---|
| `tz` | POSIX TZ string, which includes the DST rules | `GMT0BST,M3.5.0/1,M10.5.0` (UK) |
| `tfmt` | strftime time format | `%H:%M` |
| `dfmt` | strftime date format | `%a %d %b %Y` |
| `name` | Friendly name | `CYD Stream Deck` |
| `l0`–`l11` | Button labels (max 24 chars) | empty |

Icons are stored in LittleFS as `/icons/<pos>.bin`: raw RGB565, big-endian, 84×84 (14,112 bytes). Uploads go to a temporary file first and only replace the icon once the size is correct.

## 4. Windows software

### 4.1 Requirements

* Python 3.10 or later (tested with 3.14). `pip install -r pc/requirements.txt` installs `pyserial` and `Pillow`. Tkinter comes with the standard Python installer.

### 4.2 Configuration folder

Everything is kept in `%APPDATA%\CYDStreamDeck\`:

| File | Purpose |
|---|---|
| `config.json` | Device, display and button/action configuration |
| `icons\` | Copies of the chosen image files (`NN_<original name>`) |
| `daemon.log` | Daemon log, rotated at 512 kB |
| `pause_serial` | Transient flag that tells the daemon to release the COM port while the configuration application uses it |

Example `config.json`:

```json
{
  "version": 1,
  "device":  { "id": "CYDSD-503D20", "ip": "192.168.1.198", "transport": "wifi", "serial": true },
  "display": { "name": "CYD Stream Deck", "tz_name": "Europe/London",
               "tz": "GMT0BST,M3.5.0/1,M10.5.0", "time_fmt": "%H:%M", "date_fmt": "%a %d %b %Y" },
  "buttons": [
    { "label": "Notepad", "image": "00_notepad.png",
      "actions": [ { "type": "run", "target": "notepad.exe", "args": "", "cwd": "" } ] },
    { "label": "News", "image": "",
      "actions": [ { "type": "url", "target": "https://www.bbc.co.uk/news" } ] }
  ]
}
```

The file holds 12 button entries, one per grid position (only two are shown).

Action types:

| `type` | Fields | Behaviour |
|---|---|---|
| `run` | `target`, `args`, `cwd` | Runs through Windows ShellExecute (`os.startfile`). This accepts `.exe`, `.lnk`, `.bat` and registered App Paths names such as `chrome.exe`. Environment variables such as `%USERPROFILE%` are expanded. |
| `url` | `target` | Opens the URL in the default browser |
| `open` | `target` | Opens a file or folder with its associated application |
| `delay` | `seconds` (0–30) | Waits before the next action |

The actions for a button run in order, in a worker thread. A failing action is logged and the remaining actions still run.

### 4.3 Daemon (`cydsd_daemon.py`)

* **WiFi (primary transport):** the daemon binds UDP port 47801. Every 15 s it sends `CYDSD SUBSCRIBE <device id>` to the deck's address. It also sends it to the local broadcast addresses until it knows the deck's address, or when the deck has not replied for 45 s. The device sends presses to every subscriber it has heard from in the last 60 s. The daemon acknowledges each press, and the device retries up to 3 times, 250 ms apart, until it gets an acknowledgement.
* **Trusted address:** the deck's address is `device.ip` from `config.json`. If that is empty, it is the address of the first deck that answers a `SUBSCRIBE`. Datagrams from any other address are dropped without an ACK, and each such address is logged once. If the deck's address changes, run **Find device** in the configuration application, or better, give the deck a DHCP reservation.
* **Rate limit:** at most 2 presses per second are accepted, with bursts of up to 4. A position whose actions are still queued or running is not queued again. Actions run on a pool of 2 worker threads.
* **USB serial (secondary transport, optional, `device.serial`):** the daemon automatically finds the right COM port by sending `CYDSD HELLO` to each port and waiting for a matching `CYDSD ID <id>` reply.
  * Ports that don't answer, such as other CYDs running other firmware, are remembered and only probed again when the list of COM ports changes.
  * Ports are opened with DTR/RTS de-asserted, so probing does not reset ESP32 boards.
  * If a device ID is configured, only that deck is accepted, so several decks or CYDs can be attached at once.
* Presses arriving over both transports, and repeats caused by retries, are de-duplicated by (device id, sequence number).
* The daemon reloads `config.json` automatically when the file changes, so it never needs restarting after configuration.
* **Single instance:** if UDP port 47801 is already in use, a second copy logs an error and exits.
* **Footprint:** the daemon blocks in `select()` with a 1 s timeout. Measured idle cost was 0.03 CPU-seconds per minute (~0.05 % of one core) and ~33 MB of memory.
* **Command line:**
  * `pythonw cydsd_daemon.py` runs it in the background.
  * `--console` also logs to stdout.
  * `--test N` runs position N's actions once and exits.
* **Start at sign-in:** the configuration application can add or remove a hidden launcher, `CYD Stream Deck daemon.vbs`, in the user's Startup folder.

### 4.4 Configuration application (`cydsd_config.py`)

A Tkinter window with the following sections:

* **Device:**
  * Device ID and IP address fields.
  * Upload transport: WiFi or USB serial.
  * **Find device**: UDP discovery, falling back to probing the COM ports.
  * **Set WiFi...**: sends new credentials to the deck over WiFi or USB, and the deck restarts. Over WiFi, someone must tap **Yes** on the deck's screen within 30 s to accept the change.
  * A checkbox that controls whether the daemon also listens on USB.
* **Buttons:** a 3×4 grid mirroring the deck, with thumbnails and labels. Selecting a cell lets you edit:
  * the **label**
  * the **image**: the chosen file (PNG, JPEG, BMP, GIF, ICO or WebP) is copied into the configuration folder, then scaled and letterboxed to 84×84 on upload
  * the **actions**: add a program, URL, file/folder or wait; edit; remove; reorder; and **Test now**, which runs the actions locally
* **Time and date row:**
  * a time zone list of 54 common zones with their POSIX rules, so DST is automatic, or a custom POSIX TZ string
  * 12 or 24 hour time
  * one of 9 common date formats, each shown with today's date as an example
* **Save:** writes `config.json`.
* **Save and upload to deck:**
  * sends the display settings and labels
  * uploads all 12 icons
  * deletes icons from the deck for positions that no longer have an image

  It works over WiFi (HTTP) or USB. Over USB, the daemon is asked to release the COM port for the duration.
* **Start daemon now** and **Start daemon when I sign in to Windows**.

The daemon reads the saved file, so the configuration application does not need to be running during normal use.

## 5. Protocols

### 5.1 UDP (device port 47800, daemon port 47801)

Messages are ASCII, one per datagram, and every message starts with `CYDSD `.

| Direction | Message | Reply |
|---|---|---|
| PC → device | `CYDSD DISCOVER` | `CYDSD HERE <id> <ip> <fw> <name>` |
| PC → device | `CYDSD SUBSCRIBE <id or *>` | `CYDSD SUBSCRIBED <id>`. The sender is registered for presses (max 4 subscribers, 60 s TTL). |
| device → PC | `CYDSD PRESS <id> <pos> <seq>` | PC replies `CYDSD ACK <seq>` |

### 5.2 USB serial (115200 8N1, newline-terminated lines)

Debug output never starts with `CYDSD `, so the PC ignores it. The serial link supports the UDP messages above plus the following:

| Command | Reply |
|---|---|
| `CYDSD HELLO` | `CYDSD ID <id> <fw> <ip>` |
| `CYDSD PING` | `CYDSD PONG` |
| `CYDSD INFO` | `CYDSD INFO <json>` (the same JSON as `/api/info`) |
| `CYDSD SET <json>` | `CYDSD OK` or `CYDSD ERR <msg>`. The JSON has the same fields as `/api/settings`. |
| `CYDSD WIFI {"ssid":..,"pass":..}` | `CYDSD OK`, then the device restarts. No on-screen confirmation is needed, because USB already implies physical access. |
| `CYDSD ICON <pos>` / `CYDSD DATA <base64 ≤768 bytes>` … / `CYDSD END` | `CYDSD OK` after each line, which provides flow control |
| `CYDSD DELICON <pos>` | `CYDSD OK` |

### 5.3 HTTP API (port 80, station mode)

Request rules (anything else gets `403 {"ok":false,"error":"forbidden"}`):

* The `Host` header must be the device's IP address, `CYDSD-XXXXXX` or `CYDSD-XXXXXX.local`. This defeats DNS rebinding.
* Every `POST` must carry `X-CYDSD: 1` and **no** `Origin` header. Browsers always send `Origin` on cross-site POSTs and cannot add a custom header without a CORS preflight, which the device never approves. Web pages therefore cannot drive the API.
* The JSON endpoints require `Content-Type: application/json` (otherwise 400).
* No CORS headers are sent.

| Method and path | Body | Notes |
|---|---|---|
| `GET /` | – | Status page |
| `GET /api/info` | – | id, name, fw, ip, ssid, rssi, icon_size, rows, cols, tz, formats, time, pc_connected, icons[12], labels[12] |
| `POST /api/settings` | JSON `{tz, time_fmt, date_fmt, name, labels[12]}` | All fields are optional. The values are validated, saved, and the screen is redrawn. |
| `POST /api/icon?pos=N` | multipart file (14,112 bytes, 84×84 RGB565 BE) | |
| `POST /api/icon/delete?pos=N` | – | |
| `POST /api/wifi` | JSON `{ssid, pass}` | The deck asks "Change WiFi?" on screen. **Yes** within 30 s saves the credentials and restarts; **No** or a timeout returns `403 declined on the deck`. The request blocks while the question is shown. |
| `POST /api/press?pos=N` | – | Simulates a touch on position N. **Only in builds with `-D CYDSD_DEBUG_API`**; otherwise 404. |

## 6. Security considerations

The full review and improvement plan is in [Security-Review.md](Security-Review.md). In summary, the threat model is:

| Party | Trusted? | Protection |
|---|---|---|
| Web pages in a browser on the LAN | **No** | HTTP request rules in §5.3: `Host` check, `X-CYDSD` header, no `Origin`, no CORS |
| Other devices on the home LAN | **No**, but only partly protected until message authentication is added | The daemon only accepts datagrams from the deck's address and rate-limits presses. WiFi changes need a tap on the deck. Spoofing the deck's IP address is still possible on the LAN. |
| Someone with physical access (USB, the touchscreen, the flash chip) | Yes | – |

* A press can only trigger actions that the PC owner has configured. The device never sends commands or paths to the PC, only a grid position, so a rogue device or spoofed packet can at most run one of the user's own configured actions.
* Settings, icons and labels can still be changed by a LAN host that sends the right headers. The UDP subscription can also be abused by LAN hosts (Security-Review F5).
* The setup access point is open. It exists only until credentials are verified, and it is shown on the device screen while active.
* Nothing needs to be reachable from the internet. **Do not port-forward** ports 80, 47800 or 47801 on the router. Give the deck a **DHCP reservation**, so the daemon's trusted address stays correct.
* **Windows Firewall:** decline the program-wide "allow Python" prompt, or delete the rule it created, and add a scoped rule instead (run as administrator, adjusting the IP and path):

  ```powershell
  New-NetFirewallRule -DisplayName "CYD Stream Deck daemon" -Direction Inbound -Action Allow `
    -Protocol UDP -LocalPort 47801 -RemoteAddress 192.168.1.198 -Profile Private `
    -Program "$env:LOCALAPPDATA\Programs\Python\Python314\pythonw.exe"
  ```

## 7. Building, flashing and running

```powershell
# Firmware (PlatformIO; upload_port is COM4 in platformio.ini)
pio run -t upload

# PC software
cd pc
pip install -r requirements.txt
python cydsd_config.py          # configure, then "Save and upload to deck"
pythonw cydsd_daemon.py         # or tick "Start daemon when I sign in to Windows"
```

## 8. Verification performed (2026-10-21)

| Test | Result |
|---|---|
| Firmware compile (`pio run`) | Passed, no warnings (TFT_eSPI's TOUCH_CS notice is suppressed because touch is handled by the XPT2046 library). RAM 14.9 %, flash 44.2 % of the 2 MB app partition. |
| Upload to the CYD on COM4 | Passed |
| WiFi setup portal | Device joined the home network (192.168.1.198) through the portal |
| Serial auto-detect (`find_serial_devices`) | Found `CYDSD-503D20` on COM4 in 0.1 s |
| Serial: settings, icon upload, delete | Passed (2.8 s per 96×96 icon, before the bevel change) |
| UDP discovery (broadcast and unicast) | Passed |
| HTTP: settings, icon upload, delete, info | Passed (1.6 s per 96×96 icon, before the bevel change; 84×84 re-tested after it) |
| Daemon end-to-end, WiFi only: press → ACK → action executed | Passed, 3 out of 3 presses |
| Daemon with WiFi and USB together: duplicates removed | Passed |
| Daemon idle footprint | 0.03 CPU-s/min, 33 MB |
| Configuration GUI start-up, edit, save, full upload | Passed |
| Physical touch on the panel | Not tested here. Presses were injected through `/api/press`, which runs the same code path as a touch. The touch calibration is carried over unchanged from Clocky. |

### 8.1 Security hardening, firmware 1.1.0 (2026-10-21)

| Test | Result |
|---|---|
| Firmware compile and upload | Passed. RAM 14.9 %, flash 44.3 %. |
| `GET /api/info` with a valid `Host`; no `Access-Control-Allow-Origin` header | Passed (200) |
| `GET /api/info` with `Host: attacker.example` | Passed (403) |
| `POST` without `X-CYDSD`, or with `Origin` (including `Origin: null`) | Passed (403) |
| `POST /api/settings` with a `text/plain` body | Passed (400) |
| `POST /api/press` in a release build | Passed (404) |
| Configuration application client: info and settings upload | Passed |
| `POST /api/wifi` with nobody touching the screen | Passed: 403 "declined on the deck" after 31 s, and the deck stayed on the network |
| Daemon: forged `PRESS` from an address other than the deck's | Passed: no ACK, no action, one log line |
| Daemon: 50 presses in a burst | Passed: 4 ran, the rest were dropped |
| Daemon: a position pressed again while its actions are still running | Passed: not queued twice |

# CYDStreamDeck

A CYD-based touch stream deck. A 4" ESP32 "Cheap Yellow Display" shows a 3×4 grid of icons. Touch one and your Windows 11 PC launches the program, URL or file you configured for it.

* **Firmware** (`src/`, `include/`, `platformio.ini`): PlatformIO / Arduino for the ESP32-32E 4" CYD (ST7796S + XPT2046). It provides:
  * a captive-portal WiFi setup
  * the icon grid with press feedback
  * a time/date row (tap it to show the IP address, hold it for 10 s to reset WiFi)
  * the UDP, serial and HTTP interfaces
* **Daemon** (`pc/cydsd_daemon.py`): a lightweight background listener (WiFi/UDP, plus optional USB serial with auto-detection) that runs the configured actions.
* **Configuration app** (`pc/cydsd_config.py`): a Tkinter GUI for icons, actions, time zone, time/date formats and WiFi, with upload to the deck over WiFi or USB.

The full functional specification is in [docs/FSD.md](docs/FSD.md), and the network security review is in [docs/Security-Review.md](docs/Security-Review.md).

## Network safety

* Everything stays on your home network, so **don't port-forward** anything to the deck or the PC.
* Give the deck a **DHCP reservation** on your router. The daemon only accepts presses from the deck's configured IP address.
* Keep the deck on your main (or IoT) network, not a guest network with client isolation.
* For Windows Firewall, prefer the scoped rule in FSD §6 over the program-wide "allow Python" prompt.

## Quick start

```powershell
pio run -t upload                 # flash the CYD (COM4)
# Join the "CYD-StreamDeck-XXXXXX" WiFi from a phone and enter your WiFi details.

cd pc
pip install -r requirements.txt
python cydsd_config.py            # Find device, set up buttons, "Save and upload to deck",
                                  # then tick "Start daemon when I sign in to Windows"
```

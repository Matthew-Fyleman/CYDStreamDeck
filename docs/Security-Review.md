# CYD Stream Deck: Network Security Review

| | |
|---|---|
| Date | 2026-10-21 |
| Firmware reviewed | 1.0.0 (`src/`, `include/`) |
| PC software reviewed | `pc/cydsd_common.py`, `pc/cydsd_daemon.py`, `pc/cydsd_config.py` |
| Scope | Two-way communication between the Windows PC and the CYD over a home broadband network (WiFi/LAN), plus the first-boot setup portal. USB serial is covered briefly. |

## 1. Summary

The overall architecture is sound. **Nothing crosses the internet.** The deck and the PC talk directly to each other on the home LAN, and the only traffic that leaves the house is the deck's NTP time requests. There is no cloud service, no port forwarding, no UPnP and no OTA update channel. Above all, **the deck only sends a grid position, never a command**, so the PC's `config.json` is the only thing that decides what runs. That is the right design, and it should be kept.

The weak point is the assumption in FSD §6 that the home LAN is trusted. In practice a home network also carries guests' phones, smart TVs, cheap IoT devices and other PCs that may be compromised. It is also reachable **from web pages open in a browser on the LAN**. The firmware and daemon currently authenticate nothing. That allows the following:

| # | Finding | Severity | Who can exploit it |
|---|---|---|---|
| F1 | Any web page you visit can drive the deck's HTTP API (CSRF), made worse by `Access-Control-Allow-Origin: *` | **High** | Any website, no LAN access needed |
| F2 | `POST /api/wifi` lets anyone repoint the deck at another network, or knock it offline | **High** | Any LAN host, or a website via F1 |
| F3 | Spoofed UDP `PRESS` packets make the daemon run configured actions, with no rate limit | **Medium–High** | Any LAN host |
| F4 | `POST /api/press` remotely triggers presses | **Medium** | Any LAN host, or a website via F1 |
| F5 | Anyone can `SUBSCRIBE`, which lets them watch presses and evict the real daemon | **Medium** | Any LAN host |
| F6 | The home WiFi password crosses the LAN in plaintext over HTTP | **Medium** | Any LAN host that can sniff traffic |
| F7 | The setup access point is open and never times out | **Medium** | Anyone within WiFi range during setup |
| F8 | The Windows Firewall rule is broader than needed and may cover public networks | **Medium** | Hosts on other networks the PC joins |
| F9 | ACK and NTP spoofing | Low | Any LAN host |
| F10 | Settings and credentials in unencrypted flash | Low (accepted) | Physical access only |

Sections 4 and 5 below give a prioritised improvement plan. Quick wins that need no protocol change come first, then a small shared-secret (HMAC) scheme that closes the remaining LAN-level holes.

## 2. How the system uses the network

```
            Internet
               │   (only NTP from the deck: pool.ntp.org, time.google.com, time.nist.gov)
      ┌────────┴────────┐
      │ Broadband router│  NAT + firewall; no port forwards are needed or wanted
      └────────┬────────┘
     Home LAN / WiFi (WPA2/WPA3)
      ┌────────┴───────────────────────────────┐
      │                                        │
 ┌────┴─────┐  UDP 47800 ◄── DISCOVER/SUBSCRIBE/ACK ──  ┌────┴─────┐
 │   CYD    │  ──── HERE/SUBSCRIBED/PRESS ──► UDP 47801  │   PC     │
 │ (ESP32)  │  TCP 80 ◄── HTTP config API ────────────── │ daemon + │
 └──────────┘                                            │ config   │
     ▲ USB serial (115200, same line protocol + config)  └──────────┘
```

| Channel | Direction | Authentication | Confidentiality |
|---|---|---|---|
| UDP 47800 (device) | PC → device: `DISCOVER`, `SUBSCRIBE`, `ACK` | None | Plaintext (WiFi link layer only) |
| UDP 47801 (daemon) | Device → PC: `PRESS`, `HERE`, `SUBSCRIBED` | None. The device ID is broadcast, so it is not a secret. | Plaintext |
| HTTP 80 (device) | PC → device: settings, icons, **WiFi credentials**, press | None | Plaintext |
| Setup AP | Phone/PC → device: WiFi credentials | None, **open network** | **None** |
| USB serial | Both ways | None (physical access) | n/a |
| NTP | Device → internet | None | Plaintext |

## 3. Detailed findings

### F1. Cross-site request forgery against the HTTP API (High)

`sendJson()` adds `Access-Control-Allow-Origin: *` to every API response (`src/api_server.cpp:21`). The API has no authentication, and all its state-changing endpoints are `POST`s that a browser can send as **simple requests**, which need no CORS preflight:

* `POST /api/press?pos=N`: the data is in the query string, so a hidden `<form>` or `fetch(url, {method: "POST", mode: "no-cors"})` is enough.
* `POST /api/settings` and `POST /api/wifi`: the handlers read `arg("plain")` whatever the `Content-Type`, so a `text/plain` body (also a simple request) is accepted.

As a result, a malicious advert or web page open in **any** browser on the LAN can trigger configured actions on the PC, rewrite labels and settings, or change the deck's WiFi network. It can do this without being on the LAN itself. The deck's address is easy to guess (`192.168.1.x` for a typical home router, and the hostname is `CYDSD-XXXXXX`). Because of the wildcard CORS header, the page can also **read** `/api/info`, which reveals the device ID, SSID and button labels. That helps the page fingerprint the network and confirm it has found the deck.

Recent Chromium-based browsers ask the user before a public site can reach private addresses ("Local Network Access"). Other browsers and older versions may not, and users tend to click "Allow", so the device should not rely on that prompt.

DNS rebinding works the same way: a hostile domain resolves first to its own server and then to `192.168.1.198`, which makes the attack same-origin. The device does not check the `Host` header, so it cannot detect this.

### F2. Unauthenticated WiFi credential change (High)

`POST /api/wifi` (`src/api_server.cpp:124`) and the serial `WIFI` command overwrite the stored credentials and restart the deck. An attacker who can reach the API (directly or through F1) can:

* **cause a denial of service**: send a bogus SSID, and the deck drops off the network until someone holds the status row for 10 s and runs setup again
* **take the deck over**: point it at an access point the attacker controls. They can then feed it settings and icons, watch its traffic, and fake its presses.

### F3. Spoofed presses to the daemon (Medium–High)

`udp_loop()` binds `("", 47801)` on **every** interface (`pc/cydsd_daemon.py:218`). It accepts a `PRESS` from **any source address**, as long as the device ID matches the configured one. The device ID is not a secret, because the deck announces it in every `HERE` and `SUBSCRIBED` reply. So any LAN host can send:

```
CYDSD PRESS CYDSD-503D20 3 999999
```

and the daemon runs button 3's actions. FSD §6 notes correctly that an attacker cannot choose **what** runs. The risk still matters for three reasons:

* A button can be bound to a script, a `.bat` file, a shell command line with arguments, or a URL with side effects. Over time, users tend to add more powerful buttons.
* There is **no rate limit**. De-duplication is keyed on the attacker-chosen sequence number, and every accepted press starts a new thread (`pc/cydsd_daemon.py:136`). A flood of datagrams can therefore launch thousands of processes or browser tabs and make the PC unusable.
* The daemon sends an `ACK` to any sender (`pc/cydsd_daemon.py:256`). This is a small reflection primitive.

The same applies to any network the PC joins if the firewall allows it (see F8). A laptop on hotel or café WiFi would expose the daemon to strangers.

### F4. `/api/press` remote trigger (Medium)

`POST /api/press` exists for testing (FSD §5.3). It injects a press through the normal code path, so the deck itself then sends a genuine `PRESS` to the daemon. This stays a remote trigger even after F3 is fixed, because the deck becomes the attacker's proxy. Through F1, any website can use it.

### F5. Subscription hijack and eavesdropping (Medium)

Any host can send `SUBSCRIBE *`, and the deck then sends every press to it (`src/link.cpp:161`). There are only four slots, and when they are full the **oldest** subscriber is replaced (`src/link.cpp:80`). An attacker who re-subscribes from four ports every few seconds can therefore:

* push the real daemon out, so presses silently stop working
* learn when and which buttons are pressed, which shows when the user is at the desk and what they are doing

The source address of a UDP `SUBSCRIBE` can also be forged, so the deck can be told to send presses to an arbitrary third host.

### F6. WiFi password in plaintext on the LAN (Medium)

**Set WiFi...** over WiFi sends `{"ssid":..,"pass":..}` in a plain HTTP request. WPA2-Personal encryption protects that request from outsiders, but not from other clients on the same network. Anyone who knows the PSK and captured the four-way handshake can decrypt other clients' traffic, and a compromised device on the LAN can perform ARP spoofing. The password is the network's master key, so it deserves better protection than the rest of the configuration. (FSD §3.3 is right that the device never *reports* the password back. The weakness is only in transit.)

### F7. Open setup access point (Medium)

`WiFi.softAP(gApName.c_str())` (`src/portal.cpp:179`) creates an **open** network. During setup:

* The home WiFi password is typed into a plaintext HTTP form over an unencrypted radio link. Anyone within range with a monitor-mode adapter can capture it.
* Anyone nearby can join the AP first and point the deck at their own network.
* The portal never times out, so a deck that was left unconfigured, or reset by the 10-second hold, broadcasts an open AP indefinitely.

The exposure window is short and the AP name appears on screen, so the risk is moderate. It is still the only place the home WiFi password travels completely unencrypted.

### F8. Windows Firewall scope (Medium)

FSD §6 tells the user to accept the first-run prompt, which creates **program-wide** rules for `python.exe` / `pythonw.exe`. Those rules allow inbound traffic on **any port** for **any Python script**, from **any remote address**. If the user ticks "Public" as well, or the PC is a laptop that roams, the daemon (and every other Python program) becomes reachable on untrusted networks.

### F9. ACK and NTP spoofing (Low)

* Anyone can send `ACK <seq>`, which turns the button green even though the PC never received the press. This misleads the user but does no harm.
* NTP is unauthenticated, so a spoofed time only affects the clock on the screen. If time is later used for replay protection (see §5), this becomes more important.

### F10. Data at rest (Low, accepted)

FSD §3.3 already records that NVS is not encrypted and that flash encryption is out of scope. This is a reasonable trade-off for a desk gadget, but anyone with physical access can read the WiFi password and, after §5, the pairing key. Recovery is to change the WiFi password and re-pair.

### Things that are already right

* **Position-only protocol.** The deck cannot tell the PC what to execute. Keep this property whatever else changes.
* **No inbound internet exposure.** The design needs no port forwarding or UPnP, and the ESP32 firmware does not use UPnP.
* **No OTA.** The `no_ota.csv` partition table means a network attacker cannot reflash the firmware. If OTA is ever added, it must use signed images (ESP-IDF Secure Boot v2 or signature verification in the app).
* **Input bounds.** The UDP buffer is fixed at 256 bytes, serial lines are capped at 1500, settings are length-checked, and the portal HTML is escaped. No memory-safety problems were found in the network parsers.
* **Credentials never read back.** No interface returns the WiFi password.
* **Config file is local.** `config.json`, the real trust root, lives in `%APPDATA%`, which only the user can write to by default.

## 4. Improvements: quick wins (no protocol change)

These can be done in an afternoon. They remove the internet-reachable attack (F1) and most of the LAN-level abuse.

### 4.1 Harden the HTTP API against browsers (fixes F1, reduces F2 and F4)

1. **Remove `Access-Control-Allow-Origin: *`.** The configuration application uses `urllib`, not a browser, so it does not need CORS.
2. **Require a custom request header** on every `POST`, for example `X-CYDSD: 1`. Browsers cannot add custom headers to a cross-origin request without a CORS preflight. The device does not answer the preflight, so a hostile page's request never reaches the handler. Add the header in `DeviceClient._http()`.
3. **Reject requests that carry an `Origin` header.** Browsers send one on cross-origin `POST`s, and `urllib` never does.
4. **Check the `Host` header** against the device's IP address and `CYDSD-XXXXXX` hostname to defeat DNS rebinding.
5. **Require `Content-Type: application/json`** on the JSON endpoints.

```cpp
// api_server.cpp: call at the top of every POST handler.
bool requestAllowed() {
  if (gServer.hasHeader("Origin")) return false;                // browser cross-site request
  if (gServer.header("X-CYDSD") != "1") return false;           // forces a CORS preflight
  const String host = gServer.header("Host");
  return host == WiFi.localIP().toString() ||
         host.equalsIgnoreCase(Settings::deviceId()) ||
         host.equalsIgnoreCase(Settings::deviceId() + ".local");
}
// In begin(): gServer.collectHeaders(kHeaders, 3) for "Origin", "X-CYDSD", "Host".
```

### 4.2 Remove or gate `/api/press` (fixes F4)

Compile it only into debug builds (`#ifdef CYDSD_DEBUG_API`), or keep it but make it require the pairing key from §5. The serial transport already covers testing on the bench.

### 4.3 Confirm WiFi changes on the device (fixes F2)

Before `saveWifi()`, show *"Join network '\<ssid\>'? [Yes] [No]"* on the touchscreen, and only apply the change if someone taps **Yes** within 30 s. This turns a remote takeover into one that needs someone physically at the desk, and it costs very little code because `DeckUI` already has a message screen. Do the same for the serial `WIFI` command, or exempt it on the grounds that USB already implies physical access.

### 4.4 Tighten the daemon (reduces F3 and F5)

* **Check the source address.** Accept `PRESS` only from the device's known IP (`device_ip` learned from `SUBSCRIBED`, or `device.ip` from the config), and drop everything else silently.
* **Don't ACK packets that will be rejected.** Move the `sendto(... ACK ...)` call inside the `accepts()` / source-IP check.
* **Rate-limit and bound execution.** For example, allow at most 2 presses per second and only one action sequence per position at a time. Use a small worker pool (`concurrent.futures.ThreadPoolExecutor(max_workers=2)`) instead of one new thread per press.
* **Bind to the LAN interface.** Bind to the PC's LAN address on the deck's subnet rather than `""`, or refuse to run on networks Windows classifies as Public.
* **Send SUBSCRIBE by unicast** to the known device IP once it is known, and fall back to broadcast only for discovery. That stops the daemon announcing itself on every network the laptop joins.

### 4.5 Give the deck a fixed address

Set a **DHCP reservation** for the deck on the router. Source-IP filtering (4.4) and firewall scoping (4.6) both depend on it, and discovery becomes less necessary.

### 4.6 Replace the program-wide firewall prompt with a scoped rule (fixes F8)

Decline the first-run prompt, or delete the broad `python.exe` / `pythonw.exe` rules it created, then add one narrow rule. The configuration application could offer to do this (it needs elevation once):

```powershell
# Run as administrator. Adjust the device IP and the pythonw.exe path.
New-NetFirewallRule -DisplayName "CYD Stream Deck daemon" `
  -Direction Inbound -Action Allow -Protocol UDP -LocalPort 47801 `
  -RemoteAddress 192.168.1.198 -Profile Private `
  -Program "C:\Python314\pythonw.exe"
```

### 4.7 Router and home network hygiene

* **Never port-forward** 80, 47800 or 47801 to the deck or the PC. If remote use is ever wanted, use a VPN into the home network (WireGuard on the router, or Tailscale), not exposed ports.
* **Disable UPnP** on the router if nothing needs it, so no other device can open ports on the deck's behalf.
* Use **WPA3-Personal** (or WPA2-AES at minimum) with a strong passphrase, and keep the router firmware up to date.
* Keep guests on the **guest network**. Most F1–F6 attacks need LAN access, so fewer devices on the main LAN means less exposure.
* **Optional IoT segmentation.** If the router supports VLANs or an IoT network, the deck can live there with three firewall rules: PC → deck UDP 47800 and TCP 80, and deck → PC UDP 47801. Broadcast discovery does not cross segments, so this needs the DHCP reservation (4.5) and unicast. Plain client isolation, as used on most guest networks, would break the link completely.
* **Optional egress block.** The deck only needs NTP (UDP 123). Blocking all other outbound internet traffic from it at the router limits the damage if the firmware or a library is ever compromised. Even better, point it at the router's own NTP server if the router has one.

## 5. Improvements: authenticated protocol (recommended for v1.1)

The quick wins stop browsers and casual spoofing. A host on the LAN that can sniff and forge packets could still impersonate the deck or the daemon. A **shared secret with HMAC** closes that gap at very low cost: both mbedTLS on the ESP32 and Python's standard `hmac` module support it, and it adds no new dependencies.

### 5.1 Pairing

* The configuration application generates a random 32-byte key with `secrets.token_bytes(32)`.
* **Primary path: pair over USB serial.** Physical access is the trust anchor, and the key never crosses the network. Add a new `CYDSD PAIR <base64 key>` command.
* **Optional WiFi path.** The user opens a pairing window on the deck (for example by holding the status row for 3 s). The deck shows a 6-digit code, and the PC must send the key together with that code within 60 s. The key does cross the LAN once in this case. That is an acceptable trade-off for most homes, but USB pairing is preferable.
* On the device, store the key in its own NVS namespace (`pair`) and never report it back. The 10-second WiFi-reset hold should also clear it.
* On the PC, **don't keep the key in plaintext in `config.json`**. Protect it with Windows DPAPI (`CryptProtectData` via `ctypes`, no extra dependency) or the Windows Credential Manager (the `keyring` package).

### 5.2 Message authentication and replay protection

Append a truncated HMAC-SHA256 tag (128 bits, hex) to every message, and use a nonce chosen by the daemon so old messages cannot be replayed:

```
PC → device   CYDSD SUBSCRIBE <id> <daemon_nonce> <mac>
device → PC   CYDSD SUBSCRIBED <id> <daemon_nonce> <mac>
device → PC   CYDSD PRESS <id> <pos> <seq> <daemon_nonce> <mac>
PC → device   CYDSD ACK <seq> <daemon_nonce> <mac>

mac = HMAC-SHA256(key, everything before " <mac>")[:16 bytes]
```

* The daemon picks a fresh random `daemon_nonce` each time it starts. The device stores it per subscriber and binds every `PRESS` to it. Presses captured from an earlier session will not verify.
* Within a session, the daemon accepts only strictly increasing `seq` values per nonce. That replaces de-duplication on the attacker-chosen sequence number.
* The device only adds a subscriber when the `SUBSCRIBE` MAC verifies. That fixes F5. A replayed `SUBSCRIBE` can still add a slot, but presses sent to it carry a MAC for a nonce the attacker cannot use. To limit even that, add an NTP timestamp to `SUBSCRIBE` and reject anything older than about 2 minutes.
* `DISCOVER` / `HERE` can stay unauthenticated so the configuration application can still find an unpaired deck. `HERE` should then report `paired=yes/no`.
* Compare tags in constant time: `hmac.compare_digest` in Python, and a constant-time loop on the ESP32 rather than `memcmp`.

```cpp
#include <mbedtls/md.h>
bool macOf(const String& msg, uint8_t out[32]) {
  return mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                         gKey, sizeof(gKey),
                         reinterpret_cast<const uint8_t*>(msg.c_str()), msg.length(), out) == 0;
}
```

```python
import hmac, hashlib
def mac(key: bytes, msg: str) -> str:
    return hmac.new(key, msg.encode(), hashlib.sha256).hexdigest()[:32]
```

### 5.3 Authenticating the HTTP API

Use the same key on every request. Add an `X-CYDSD-Auth: <timestamp>:<hmac(key, method + path + timestamp + sha256(body))>` header, and reject requests where the MAC does not verify or the timestamp is more than about 60 s from the device clock. This also covers the custom-header requirement in §4.1. Icon uploads need the body hash computed in a streaming way, or the MAC can cover only the method, path and timestamp, because icons are not sensitive.

### 5.4 Protecting the WiFi password in transit (fixes F6)

With a shared key available, either:

* **encrypt the password**: send `{"ssid": .., "pass_enc": base64(nonce ‖ AES-256-GCM(key_wifi, pass))}`, where `key_wifi` is derived from the pairing key with HKDF so the two keys are separate. mbedTLS provides `mbedtls_gcm_*` and Python's `cryptography` package provides AES-GCM, which adds one dependency. Or:
* **only allow WiFi changes over USB**, plus the on-device confirmation from §4.3. This is the simplest option, and it is enough for most users.

### 5.5 Why not TLS?

The ESP32 can serve HTTPS (`esp_https_server`), but a self-signed certificate gives no protection unless the PC pins it, and pinning needs the same kind of pairing step anyway. TLS also adds about 40 KB of RAM per connection, slows icon uploads, and does nothing for the UDP press path. HMAC with a pre-shared key gives the integrity guarantees that matter here at a fraction of the cost. If TLS is added later, pin the device certificate's fingerprint during pairing.

## 6. Improvements: setup portal (fixes F7)

1. **Protect the AP with WPA2.** Generate a random 8–10 character passphrase at each portal start (`esp_random()`, which is a true RNG while the radio is on), call `WiFi.softAP(name, pass)`, and show the passphrase on screen. Showing it as a WiFi QR code too (`WIFI:T:WPA;S:<ssid>;P:<pass>;;`) makes this just as easy for the user as an open AP.
2. **Time out the portal**, for example after 10 minutes without a client. Then either stop the AP and show "Tap to restart setup", or go back to retrying the stored network if one exists.
3. **Allow only one client.** Use `WiFi.softAP(name, pass, channel, hidden, 1)`.
4. **Offer to pair during setup.** On the success page, the portal can hand over the pairing code from §5.1, so the user doesn't need a separate step.

## 7. Suggested implementation order

| Step | Change | Fixes | Effort |
|---|---|---|---|
| 1 | Remove wildcard CORS. Require the `X-CYDSD` header, no `Origin`, and a valid `Host`. | F1 | Small |
| 2 | Gate `/api/press` behind a debug build flag. | F4 | Trivial |
| 3 | Confirm WiFi changes on the touchscreen. | F2 | Small |
| 4 | Daemon: filter by source IP, ACK only accepted presses, rate-limit, use a worker pool. | F3, F5 (partly) | Small |
| 5 | DHCP reservation, scoped firewall rule, router checklist (§4.5–4.7). Document these in the README and FSD. | F8 | User setup |
| 6 | WPA2 setup AP with an on-screen passphrase and QR code, plus a portal timeout. | F7 | Small–medium |
| 7 | Pairing key and HMAC on UDP and HTTP, with the key in DPAPI on the PC. | F3, F5, F9 | Medium |
| 8 | WiFi password over USB only, or AES-GCM encrypted. | F6 | Small once step 7 is done |

## 8. Documentation updates

* **FSD §6** should change from "intended for a trusted home LAN" to an explicit threat model:
  * Websites in the user's browser are untrusted, which covers F1.
  * Other LAN devices are untrusted until pairing (§5) is implemented.
  * Physical access is trusted.
* **FSD §6** should replace the advice to "allow Python on Private networks" with the scoped rule from §4.6.
* **The README** should add a short "Network safety" note: don't port-forward, keep the deck on the main or IoT network rather than the guest network, and reserve its IP address.

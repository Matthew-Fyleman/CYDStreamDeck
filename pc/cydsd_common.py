"""Shared code for the CYD Stream Deck daemon and configuration application.

Holds the configuration file format, the device protocol (UDP + USB serial),
device discovery and icon conversion.
"""

from __future__ import annotations

import base64
import copy
import ipaddress
import json
import os
import socket
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

APP_DIR = Path(os.environ.get("APPDATA", Path.home())) / "CYDStreamDeck"
CONFIG_PATH = APP_DIR / "config.json"
ICONS_DIR = APP_DIR / "icons"
LOG_PATH = APP_DIR / "daemon.log"
# While this file exists the daemon releases the serial port (used by the
# configuration app to upload over USB).
PAUSE_SERIAL_FLAG = APP_DIR / "pause_serial"

# ---------------------------------------------------------------------------
# Device constants (must match include/deck_config.h)
# ---------------------------------------------------------------------------

ROWS = 3
COLS = 4
BUTTONS = ROWS * COLS
ICON_SIZE = 84          # image inside the 96 px bevelled cell on the deck
ICON_BYTES = ICON_SIZE * ICON_SIZE * 2
DEVICE_UDP_PORT = 47800
DAEMON_UDP_PORT = 47801
TAG = "CYDSD"
BAUD = 115200

# ---------------------------------------------------------------------------
# Display options offered by the configuration app
# ---------------------------------------------------------------------------

# (IANA-style name, POSIX TZ string understood by the ESP32 C library)
TIMEZONES: list[tuple[str, str]] = [
    ("UTC", "UTC0"),
    ("Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"),
    ("Europe/Dublin", "GMT0IST,M3.5.0/1,M10.5.0"),
    ("Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"),
    ("Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3"),
    ("Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"),
    ("Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"),
    ("Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4"),
    ("Europe/Istanbul", "<+03>-3"),
    ("Europe/Moscow", "MSK-3"),
    ("Atlantic/Reykjavik", "GMT0"),
    ("Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24"),
    ("Africa/Lagos", "WAT-1"),
    ("Africa/Johannesburg", "SAST-2"),
    ("Africa/Nairobi", "EAT-3"),
    ("Asia/Dubai", "<+04>-4"),
    ("Asia/Karachi", "PKT-5"),
    ("Asia/Kolkata", "IST-5:30"),
    ("Asia/Dhaka", "<+06>-6"),
    ("Asia/Bangkok", "<+07>-7"),
    ("Asia/Singapore", "<+08>-8"),
    ("Asia/Hong_Kong", "HKT-8"),
    ("Asia/Shanghai", "CST-8"),
    ("Asia/Tokyo", "JST-9"),
    ("Asia/Seoul", "KST-9"),
    ("Australia/Perth", "AWST-8"),
    ("Australia/Darwin", "ACST-9:30"),
    ("Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"),
    ("Australia/Brisbane", "AEST-10"),
    ("Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"),
    ("Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"),
    ("Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"),
    ("Pacific/Honolulu", "HST10"),
    ("America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"),
    ("America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"),
    ("America/Vancouver", "PST8PDT,M3.2.0,M11.1.0"),
    ("America/Phoenix", "MST7"),
    ("America/Denver", "MST7MDT,M3.2.0,M11.1.0"),
    ("America/Chicago", "CST6CDT,M3.2.0,M11.1.0"),
    ("America/Mexico_City", "CST6"),
    ("America/New_York", "EST5EDT,M3.2.0,M11.1.0"),
    ("America/Toronto", "EST5EDT,M3.2.0,M11.1.0"),
    ("America/Halifax", "AST4ADT,M3.2.0,M11.1.0"),
    ("America/St_Johns", "NST3:30NDT,M3.2.0,M11.1.0"),
    ("America/Bogota", "<-05>5"),
    ("America/Santiago", "<-04>4<-03>,M9.1.6/24,M4.1.6/24"),
    ("America/Sao_Paulo", "<-03>3"),
    ("America/Argentina/Buenos_Aires", "<-03>3"),
]
CUSTOM_TZ = "Custom (POSIX TZ string)"

TIME_FORMATS: list[tuple[str, str]] = [
    ("24 hour (17:45)", "%H:%M"),
    ("12 hour (05:45 PM)", "%I:%M %p"),
]

DATE_FORMATS: list[str] = [
    "%a %d %b %Y",   # Wed 21 Oct 2026
    "%d/%m/%Y",      # 21/10/2026
    "%m/%d/%Y",      # 10/21/2026
    "%Y-%m-%d",      # 2026-10-21
    "%d.%m.%Y",      # 21.10.2026
    "%d %b %Y",      # 21 Oct 2026
    "%b %d, %Y",     # Oct 21, 2026
    "%a %b %d %Y",   # Wed Oct 21 2026
    "%A %d %B %Y",   # Wednesday 21 October 2026
]

ACTION_TYPES = ("run", "url", "open", "delay")

# ---------------------------------------------------------------------------
# Configuration file
# ---------------------------------------------------------------------------


def default_config() -> dict:
    return {
        "version": 1,
        "device": {
            "id": "",          # e.g. CYDSD-503D20; empty = accept any device
            "ip": "",          # last known address (discovery fills this in)
            "transport": "wifi",   # how the config app uploads: wifi | serial
            "serial": True,    # daemon also listens over USB serial
        },
        "display": {
            "name": "CYD Stream Deck",
            "tz_name": "Europe/London",
            "tz": "GMT0BST,M3.5.0/1,M10.5.0",
            "time_fmt": "%H:%M",
            "date_fmt": "%a %d %b %Y",
        },
        "buttons": [{"label": "", "image": "", "actions": []} for _ in range(BUTTONS)],
    }


def _merge(base: dict, override: dict) -> dict:
    out = copy.deepcopy(base)
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(out.get(key), dict):
            out[key] = _merge(out[key], value)
        else:
            out[key] = value
    return out


def ensure_dirs() -> None:
    ICONS_DIR.mkdir(parents=True, exist_ok=True)


def load_config(path: Path = CONFIG_PATH) -> dict:
    cfg = default_config()
    if path.exists():
        with open(path, "r", encoding="utf-8-sig") as f:  # tolerate BOM from hand edits
            cfg = _merge(cfg, json.load(f))
    buttons = list(cfg.get("buttons", []))[:BUTTONS]
    while len(buttons) < BUTTONS:
        buttons.append({"label": "", "image": "", "actions": []})
    cfg["buttons"] = [_merge({"label": "", "image": "", "actions": []}, b) for b in buttons]
    return cfg


def save_config(cfg: dict, path: Path = CONFIG_PATH) -> None:
    ensure_dirs()
    tmp = path.with_suffix(".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)
    os.replace(tmp, path)


def icon_path(image: str) -> Path | None:
    """Resolve a button's image entry (relative to ICONS_DIR or absolute)."""
    if not image:
        return None
    p = Path(image)
    return p if p.is_absolute() else ICONS_DIR / p


def describe_action(action: dict) -> str:
    kind = action.get("type")
    if kind == "run":
        args = action.get("args", "")
        return f"Run: {action.get('target', '')}" + (f" {args}" if args else "")
    if kind == "url":
        return f"Open URL: {action.get('target', '')}"
    if kind == "open":
        return f"Open: {action.get('target', '')}"
    if kind == "delay":
        return f"Wait {action.get('seconds', 1)} s"
    return f"Unknown: {action}"


# ---------------------------------------------------------------------------
# Icons
# ---------------------------------------------------------------------------


def image_to_rgb565(path: Path) -> bytes:
    """Scale an image to fit ICON_SIZE square (letterboxed on black) and
    return it as big-endian RGB565, the byte order the display expects."""
    from PIL import Image

    with Image.open(path) as src:
        img = src.convert("RGBA")
    img.thumbnail((ICON_SIZE, ICON_SIZE), Image.LANCZOS)
    canvas = Image.new("RGBA", (ICON_SIZE, ICON_SIZE), (0, 0, 0, 255))
    canvas.alpha_composite(img, ((ICON_SIZE - img.width) // 2, (ICON_SIZE - img.height) // 2))
    out = bytearray(ICON_BYTES)
    i = 0
    for r, g, b, _a in canvas.getdata():
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[i] = v >> 8
        out[i + 1] = v & 0xFF
        i += 2
    return bytes(out)


# ---------------------------------------------------------------------------
# Network discovery
# ---------------------------------------------------------------------------


def local_broadcast_addresses() -> list[str]:
    """Best-effort list of broadcast addresses for local IPv4 interfaces
    (assumes /24 networks, plus the limited broadcast address)."""
    addrs = {"255.255.255.255"}
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if not ip.startswith("127."):
                net = ipaddress.ip_network(f"{ip}/24", strict=False)
                addrs.add(str(net.broadcast_address))
    except OSError:
        pass
    return sorted(addrs)


def parse_here(text: str) -> dict | None:
    parts = text.strip().split(" ", 5)
    if len(parts) < 5 or parts[0] != TAG or parts[1] != "HERE":
        return None
    return {"id": parts[2], "ip": parts[3], "fw": parts[4], "name": parts[5] if len(parts) > 5 else ""}


def udp_discover(timeout: float = 1.5, extra_ips: list[str] | None = None) -> list[dict]:
    """Broadcast a DISCOVER and collect HERE replies."""
    found: dict[str, dict] = {}
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.bind(("", 0))
        msg = f"{TAG} DISCOVER".encode()
        for addr in local_broadcast_addresses() + list(extra_ips or []):
            try:
                sock.sendto(msg, (addr, DEVICE_UDP_PORT))
            except OSError:
                pass
        end = time.monotonic() + timeout
        while (left := end - time.monotonic()) > 0:
            sock.settimeout(left)
            try:
                data, _ = sock.recvfrom(512)
            except (socket.timeout, OSError):
                break
            dev = parse_here(data.decode(errors="replace"))
            if dev:
                found[dev["id"]] = dev
    finally:
        sock.close()
    return list(found.values())


# ---------------------------------------------------------------------------
# USB serial
# ---------------------------------------------------------------------------


def open_serial(port: str, timeout: float = 0.5):
    """Open a port without toggling DTR/RTS, which would reset the ESP32."""
    import serial

    s = serial.Serial()
    s.port = port
    s.baudrate = BAUD
    s.timeout = timeout
    s.write_timeout = 2
    s.dtr = False
    s.rts = False
    s.open()
    return s


def probe_serial_port(port: str, timeout: float = 1.5) -> dict | None:
    """Ask a port whether a CYD Stream Deck is on the other end."""
    try:
        s = open_serial(port, timeout=0.2)
    except Exception:
        return None
    try:
        s.reset_input_buffer()
        s.write(f"\n{TAG} HELLO\n".encode())
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            line = s.readline().decode(errors="replace").strip()
            if line.startswith(f"{TAG} ID "):
                parts = line.split()
                return {"port": port, "id": parts[2], "fw": parts[3] if len(parts) > 3 else "",
                        "ip": parts[4] if len(parts) > 4 else ""}
    except Exception:
        return None
    finally:
        s.close()
    return None


def list_serial_ports() -> list[str]:
    from serial.tools import list_ports

    return sorted(p.device for p in list_ports.comports())


def find_serial_devices(device_id: str = "", skip: set[str] | None = None) -> list[dict]:
    """Probe every COM port and return the CYD Stream Decks found. Ports that
    are busy or belong to other devices (including other CYDs running
    different firmware) simply don't answer."""
    found = []
    for port in list_serial_ports():
        if skip and port in skip:
            continue
        dev = probe_serial_port(port)
        if dev and (not device_id or dev["id"].upper() == device_id.upper()):
            found.append(dev)
    return found


class SerialSession:
    """Request/response helper for configuration commands over USB."""

    def __init__(self, port: str):
        self.ser = open_serial(port, timeout=0.5)
        time.sleep(0.1)
        self.ser.reset_input_buffer()

    def close(self) -> None:
        self.ser.close()

    def command(self, cmd: str, timeout: float = 3.0, expect: str = "OK") -> str:
        self.ser.write(f"{TAG} {cmd}\n".encode())
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            line = self.ser.readline().decode(errors="replace").strip()
            if not line.startswith(TAG + " "):
                continue
            body = line[len(TAG) + 1:]
            if body.startswith(expect):
                return body
            if body.startswith("ERR"):
                raise RuntimeError(body)
        raise TimeoutError(f"No reply to {cmd.split(' ')[0]}")


# ---------------------------------------------------------------------------
# Device client (configuration upload) over WiFi (HTTP) or USB serial
# ---------------------------------------------------------------------------


class DeviceClient:
    def __init__(self, transport: str, ip: str = "", port: str = ""):
        self.transport = transport
        self.ip = ip
        self.port = port
        self.session: SerialSession | None = None

    # -- context management: pauses the daemon's serial use if needed --
    def __enter__(self):
        if self.transport == "serial":
            ensure_dirs()
            PAUSE_SERIAL_FLAG.touch()
            time.sleep(1.5)  # give the daemon time to release the port
            try:
                self.session = SerialSession(self.port)
            except Exception:
                PAUSE_SERIAL_FLAG.unlink(missing_ok=True)
                raise
        return self

    def __exit__(self, *exc):
        if self.session:
            self.session.close()
            self.session = None
        if self.transport == "serial":
            PAUSE_SERIAL_FLAG.unlink(missing_ok=True)

    # -- HTTP helpers --
    def _http(self, path: str, body: bytes | None = None, content_type: str = "application/json",
              timeout: float = 10) -> dict:
        req = urllib.request.Request(f"http://{self.ip}{path}", data=body, method="POST" if body is not None else "GET")
        # The deck rejects POSTs without this header, which a web page cannot add
        # to a cross-site request.
        req.add_header("X-CYDSD", "1")
        if body is not None:
            req.add_header("Content-Type", content_type)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return json.loads(resp.read().decode() or "{}")
        except urllib.error.HTTPError as e:
            try:
                detail = json.loads(e.read().decode()).get("error", "")
            except Exception:
                detail = ""
            raise RuntimeError(f"HTTP {e.code} {detail}".strip()) from None

    # -- operations --
    def info(self) -> dict:
        if self.transport == "serial":
            return json.loads(self.session.command("INFO", expect="INFO")[5:])
        return self._http("/api/info")

    def set_settings(self, settings: dict) -> None:
        payload = json.dumps(settings, separators=(",", ":"))
        if self.transport == "serial":
            self.session.command("SET " + payload)
        else:
            self._http("/api/settings", payload.encode())

    def upload_icon(self, pos: int, data: bytes) -> None:
        assert len(data) == ICON_BYTES
        if self.transport == "serial":
            self.session.command(f"ICON {pos}")
            for i in range(0, len(data), 768):
                self.session.command("DATA " + base64.b64encode(data[i:i + 768]).decode())
            self.session.command("END")
            return
        boundary = uuid.uuid4().hex
        body = (
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"icon\"; filename=\"{pos}.bin\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\n"
        ).encode() + data + f"\r\n--{boundary}--\r\n".encode()
        self._http(f"/api/icon?pos={pos}", body, f"multipart/form-data; boundary={boundary}")

    def delete_icon(self, pos: int) -> None:
        if self.transport == "serial":
            self.session.command(f"DELICON {pos}")
        else:
            self._http(f"/api/icon/delete?pos={pos}", b"")

    def set_wifi(self, ssid: str, password: str) -> None:
        payload = json.dumps({"ssid": ssid, "pass": password})
        if self.transport == "serial":
            self.session.command("WIFI " + payload)
        else:
            # Waits while someone accepts or declines the change on the touchscreen.
            self._http("/api/wifi", payload.encode(), timeout=45)


def device_settings_payload(cfg: dict) -> dict:
    disp = cfg["display"]
    return {
        "name": disp.get("name", "CYD Stream Deck"),
        "tz": disp["tz"],
        "time_fmt": disp["time_fmt"],
        "date_fmt": disp["date_fmt"],
        "labels": [b.get("label", "") for b in cfg["buttons"]],
    }

"""CYD Stream Deck daemon.

Listens for icon presses from the CYD over WiFi (UDP) and, optionally, USB
serial, and runs the actions configured for the pressed grid position.

Designed to be idle almost all of the time: it blocks on a socket with a
one-second timeout and only wakes for heartbeats and presses.

Usage:
    pythonw cydsd_daemon.py            normal background operation
    python  cydsd_daemon.py --console  log to the console as well
    python  cydsd_daemon.py --test N   run the actions for position N and exit
"""

from __future__ import annotations

import argparse
import logging
import logging.handlers
import os
import select
import socket
import sys
import threading
import time
import webbrowser

import cydsd_common as common

HEARTBEAT_S = 15          # SUBSCRIBE interval (device expires us after 60 s)
SERIAL_PING_S = 10
SERIAL_RESCAN_S = 10
DEDUPE_S = 5.0

log = logging.getLogger("cydsd")


# ---------------------------------------------------------------------------
# Configuration (reloaded automatically when the file changes)
# ---------------------------------------------------------------------------


class ConfigWatcher:
    def __init__(self):
        self._mtime = None
        self.cfg = common.default_config()
        self.check()

    def check(self) -> bool:
        try:
            mtime = common.CONFIG_PATH.stat().st_mtime
        except FileNotFoundError:
            mtime = None
        if mtime == self._mtime:
            return False
        self._mtime = mtime
        try:
            self.cfg = common.load_config()
            log.info("Configuration loaded (%s)", common.CONFIG_PATH)
        except Exception as e:  # keep the previous config on a bad file
            log.error("Could not load configuration: %s", e)
        return True

    @property
    def device_id(self) -> str:
        return self.cfg["device"].get("id", "").strip()

    def accepts(self, dev_id: str) -> bool:
        return not self.device_id or dev_id.upper() == self.device_id.upper()


# ---------------------------------------------------------------------------
# Actions
# ---------------------------------------------------------------------------


def run_action(action: dict) -> None:
    kind = action.get("type")
    target = os.path.expandvars(str(action.get("target", "")).strip())
    if kind == "run":
        args = action.get("args", "") or None
        cwd = action.get("cwd", "") or os.path.dirname(target) or None
        if cwd and not os.path.isdir(cwd):
            cwd = None
        # ShellExecute: handles .exe, .lnk, .bat, App Paths names and documents.
        os.startfile(target, "open", args, cwd)
    elif kind == "url":
        webbrowser.open(target)
    elif kind == "open":
        os.startfile(target)
    elif kind == "delay":
        time.sleep(min(max(float(action.get("seconds", 1)), 0.0), 30.0))
    else:
        raise ValueError(f"unknown action type {kind!r}")


def run_actions(pos: int, cfg: dict) -> None:
    if not 0 <= pos < common.BUTTONS:
        log.warning("Ignoring press for invalid position %s", pos)
        return
    button = cfg["buttons"][pos]
    actions = button.get("actions", [])
    log.info("Position %d (row %d, col %d) '%s': %d action(s)", pos, pos // common.COLS + 1,
             pos % common.COLS + 1, button.get("label", ""), len(actions))
    for action in actions:
        try:
            log.info("  %s", common.describe_action(action))
            run_action(action)
        except Exception as e:
            log.error("  action failed: %s", e)


class Dispatcher:
    """De-duplicates presses arriving over both transports / retries and runs
    actions off the network thread."""

    def __init__(self, watcher: ConfigWatcher):
        self.watcher = watcher
        self._seen: dict[tuple[str, str], float] = {}
        self._lock = threading.Lock()

    def press(self, dev_id: str, pos: str, seq: str, via: str) -> None:
        now = time.monotonic()
        with self._lock:
            self._seen = {k: t for k, t in self._seen.items() if now - t < DEDUPE_S}
            key = (dev_id.upper(), seq)
            if key in self._seen:
                return
            self._seen[key] = now
        self.watcher.check()
        log.info("Press from %s via %s: pos=%s seq=%s", dev_id, via, pos, seq)
        try:
            p = int(pos)
        except ValueError:
            return
        threading.Thread(target=run_actions, args=(p, self.watcher.cfg), daemon=True).start()


# ---------------------------------------------------------------------------
# Serial transport (runs in its own thread)
# ---------------------------------------------------------------------------


class SerialListener(threading.Thread):
    def __init__(self, watcher: ConfigWatcher, dispatcher: Dispatcher, stop: threading.Event):
        super().__init__(daemon=True, name="serial")
        self.watcher = watcher
        self.dispatcher = dispatcher
        self.stop_event = stop
        self._probed_ports: set[str] = set()  # ports with no deck on them

    def run(self) -> None:
        last_ports: list[str] = []
        while not self.stop_event.is_set():
            if not self.watcher.cfg["device"].get("serial", True) or common.PAUSE_SERIAL_FLAG.exists():
                self.stop_event.wait(1)
                continue
            try:
                ports = common.list_serial_ports()
            except Exception:
                ports = []
            if ports != last_ports:  # only re-probe when the port list changes
                self._probed_ports.clear()
                last_ports = ports
            found = None
            for port in ports:
                if port in self._probed_ports:
                    continue
                dev = common.probe_serial_port(port)
                if dev and self.watcher.accepts(dev["id"]):
                    found = dev
                    break
                self._probed_ports.add(port)
            if found:
                log.info("Serial: found %s on %s", found["id"], found["port"])
                self._listen(found["port"])
                self._probed_ports.discard(found["port"])
            self.stop_event.wait(SERIAL_RESCAN_S)

    def _listen(self, port: str) -> None:
        try:
            ser = common.open_serial(port, timeout=1.0)
        except Exception as e:
            log.warning("Serial: cannot open %s: %s", port, e)
            return
        last_ping = 0.0
        try:
            while not self.stop_event.is_set():
                if common.PAUSE_SERIAL_FLAG.exists():
                    log.info("Serial: released %s for the configuration app", port)
                    return
                now = time.monotonic()
                if now - last_ping >= SERIAL_PING_S:
                    ser.write(f"{common.TAG} PING\n".encode())
                    last_ping = now
                line = ser.readline().decode(errors="replace").strip()
                parts = line.split()
                if len(parts) == 5 and parts[0] == common.TAG and parts[1] == "PRESS":
                    _, _, dev_id, pos, seq = parts
                    ser.write(f"{common.TAG} ACK {seq}\n".encode())
                    if self.watcher.accepts(dev_id):
                        self.dispatcher.press(dev_id, pos, seq, "USB")
        except Exception as e:
            log.info("Serial: %s closed (%s)", port, e)
        finally:
            ser.close()


# ---------------------------------------------------------------------------
# UDP transport (main thread)
# ---------------------------------------------------------------------------


def udp_loop(watcher: ConfigWatcher, dispatcher: Dispatcher, stop: threading.Event) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        sock.bind(("", common.DAEMON_UDP_PORT))
    except OSError:
        log.error("UDP port %d is in use - is the daemon already running?", common.DAEMON_UDP_PORT)
        sys.exit(1)
    log.info("Listening for the deck on UDP port %d", common.DAEMON_UDP_PORT)

    last_heartbeat = 0.0
    device_ip = ""
    subscribed = False
    while not stop.is_set():
        now = time.monotonic()
        if now - last_heartbeat >= HEARTBEAT_S:
            last_heartbeat = now
            watcher.check()
            target = watcher.device_id or "*"
            msg = f"{common.TAG} SUBSCRIBE {target}".encode()
            dests = set(common.local_broadcast_addresses())
            for ip in (device_ip, watcher.cfg["device"].get("ip", "")):
                if ip:
                    dests.add(ip)
            for addr in dests:
                try:
                    sock.sendto(msg, (addr, common.DEVICE_UDP_PORT))
                except OSError:
                    pass

        ready, _, _ = select.select([sock], [], [], 1.0)
        if not ready:
            continue
        try:
            data, addr = sock.recvfrom(512)
        except OSError:
            continue
        parts = data.decode(errors="replace").strip().split()
        if len(parts) < 2 or parts[0] != common.TAG:
            continue
        if parts[1] == "PRESS" and len(parts) == 5:
            _, _, dev_id, pos, seq = parts
            sock.sendto(f"{common.TAG} ACK {seq}".encode(), addr)
            if watcher.accepts(dev_id):
                dispatcher.press(dev_id, pos, seq, f"WiFi {addr[0]}")
        elif parts[1] == "SUBSCRIBED" and len(parts) >= 3 and watcher.accepts(parts[2]):
            if not subscribed or device_ip != addr[0]:
                log.info("Subscribed to %s at %s", parts[2], addr[0])
            subscribed = True
            device_ip = addr[0]


# ---------------------------------------------------------------------------


def setup_logging(console: bool) -> None:
    common.ensure_dirs()
    handlers: list[logging.Handler] = [
        logging.handlers.RotatingFileHandler(common.LOG_PATH, maxBytes=512_000, backupCount=1, encoding="utf-8")
    ]
    if console and sys.stdout:
        handlers.append(logging.StreamHandler(sys.stdout))
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s", handlers=handlers)


def main() -> None:
    ap = argparse.ArgumentParser(description="CYD Stream Deck daemon")
    ap.add_argument("--console", action="store_true", help="also log to the console")
    ap.add_argument("--test", type=int, metavar="POS", help="run the actions for grid position POS and exit")
    args = ap.parse_args()

    setup_logging(args.console or args.test is not None)
    watcher = ConfigWatcher()
    if args.test is not None:
        run_actions(args.test, watcher.cfg)
        return

    log.info("CYD Stream Deck daemon starting (device id filter: %s)", watcher.device_id or "any")
    stop = threading.Event()
    dispatcher = Dispatcher(watcher)
    SerialListener(watcher, dispatcher, stop).start()
    try:
        udp_loop(watcher, dispatcher, stop)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        log.info("Daemon stopped")


if __name__ == "__main__":
    main()

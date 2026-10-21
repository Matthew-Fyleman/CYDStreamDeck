"""CYD Stream Deck configuration application (Tkinter).

Edits the configuration stored in %APPDATA%\\CYDStreamDeck, and uploads icons,
labels and display settings (and optionally WiFi credentials) to the deck over
WiFi or USB serial.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
import winreg
from pathlib import Path
from tkinter import filedialog, messagebox, simpledialog, ttk

import cydsd_common as common

HERE = Path(__file__).resolve().parent
DAEMON_SCRIPT = HERE / "cydsd_daemon.py"
STARTUP_DIR = Path(os.environ.get("APPDATA", "")) / "Microsoft" / "Windows" / "Start Menu" / "Programs" / "Startup"
LEGACY_LAUNCHER = STARTUP_DIR / "CYD Stream Deck daemon.vbs"
# Autostart uses a per-user Run registry value rather than a generated script:
# antivirus (e.g. Bitdefender) quarantines Python that writes .vbs launchers.
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
RUN_VALUE = "CYD Stream Deck daemon"
THUMB = 72


def pythonw() -> str:
    exe = Path(sys.executable)
    candidate = exe.with_name("pythonw.exe")
    return str(candidate if candidate.exists() else exe)


def autostart_enabled() -> bool:
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            winreg.QueryValueEx(key, RUN_VALUE)
        return True
    except OSError:
        return False


def set_autostart(enabled: bool) -> None:
    LEGACY_LAUNCHER.unlink(missing_ok=True)
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as key:
        if enabled:
            winreg.SetValueEx(key, RUN_VALUE, 0, winreg.REG_SZ, f'"{pythonw()}" "{DAEMON_SCRIPT}"')
        else:
            try:
                winreg.DeleteValue(key, RUN_VALUE)
            except FileNotFoundError:
                pass


# ---------------------------------------------------------------------------
# Dialogs
# ---------------------------------------------------------------------------


class ActionDialog(simpledialog.Dialog):
    def __init__(self, parent, kind: str, action: dict | None = None):
        self.kind = kind
        self.action = dict(action or {"type": kind})
        self.result: dict | None = None
        titles = {"run": "Run a program", "url": "Open a URL", "open": "Open a file or folder", "delay": "Wait"}
        super().__init__(parent, titles[kind])

    def body(self, master):
        self.vars: dict[str, tk.StringVar] = {}
        row = 0

        def field(label, key, browse=None, default=""):
            nonlocal row
            ttk.Label(master, text=label).grid(row=row, column=0, sticky="w", padx=4, pady=4)
            var = tk.StringVar(value=str(self.action.get(key, default)))
            ent = ttk.Entry(master, textvariable=var, width=60)
            ent.grid(row=row, column=1, sticky="we", padx=4, pady=4)
            if browse:
                ttk.Button(master, text="Browse...", command=lambda: browse(var)).grid(row=row, column=2, padx=4)
            self.vars[key] = var
            row += 1
            return ent

        if self.kind == "run":
            first = field("Program:", "target", self._browse_program)
            field("Arguments:", "args")
            field("Start in (optional):", "cwd", self._browse_folder)
        elif self.kind == "url":
            first = field("URL:", "target", default="https://")
        elif self.kind == "open":
            first = field("File or folder:", "target", self._browse_file)
            ttk.Button(master, text="Choose folder...", command=lambda: self._browse_folder(self.vars["target"])).grid(
                row=row, column=1, sticky="w", padx=4)
        else:
            first = field("Seconds (0-30):", "seconds", default="1")
        return first

    def _browse_program(self, var):
        path = filedialog.askopenfilename(parent=self, filetypes=[
            ("Programs and shortcuts", "*.exe *.lnk *.bat *.cmd *.com"), ("All files", "*.*")])
        if path:
            var.set(os.path.normpath(path))

    def _browse_file(self, var):
        path = filedialog.askopenfilename(parent=self)
        if path:
            var.set(os.path.normpath(path))

    def _browse_folder(self, var):
        path = filedialog.askdirectory(parent=self)
        if path:
            var.set(os.path.normpath(path))

    def validate(self):
        if self.kind == "delay":
            try:
                v = float(self.vars["seconds"].get())
                if not 0 <= v <= 30:
                    raise ValueError
            except ValueError:
                messagebox.showerror("Invalid", "Enter a number of seconds between 0 and 30.", parent=self)
                return False
            return True
        target = self.vars["target"].get().strip()
        if not target or target == "https://":
            messagebox.showerror("Invalid", "Please enter a target.", parent=self)
            return False
        if self.kind == "url" and "://" not in target:
            self.vars["target"].set("https://" + target)
        return True

    def apply(self):
        result = {"type": self.kind}
        for key, var in self.vars.items():
            val = var.get().strip()
            result[key] = float(val) if key == "seconds" else val
        self.result = result


class WifiDialog(simpledialog.Dialog):
    def body(self, master):
        ttk.Label(master, text="Network name (SSID):").grid(row=0, column=0, sticky="w", padx=4, pady=4)
        self.ssid = ttk.Entry(master, width=36)
        self.ssid.grid(row=0, column=1, padx=4, pady=4)
        ttk.Label(master, text="Password:").grid(row=1, column=0, sticky="w", padx=4, pady=4)
        self.pw = ttk.Entry(master, width=36, show="•")
        self.pw.grid(row=1, column=1, padx=4, pady=4)
        self.show = tk.BooleanVar()
        ttk.Checkbutton(master, text="Show password", variable=self.show,
                        command=lambda: self.pw.config(show="" if self.show.get() else "•")).grid(
            row=2, column=1, sticky="w")
        ttk.Label(master, text="The deck restarts and joins this network. Credentials are\n"
                               "stored only on the deck, never on this PC.",
                  foreground="#666").grid(row=3, column=0, columnspan=2, sticky="w", padx=4, pady=8)
        return self.ssid

    def validate(self):
        if not self.ssid.get().strip():
            messagebox.showerror("Invalid", "Enter the network name.", parent=self)
            return False
        return True

    def apply(self):
        self.result = (self.ssid.get().strip(), self.pw.get())


# ---------------------------------------------------------------------------
# Main window
# ---------------------------------------------------------------------------


class ConfigApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("CYD Stream Deck configuration")
        self.minsize(980, 640)
        common.ensure_dirs()
        try:
            self.cfg = common.load_config()
        except Exception as e:
            messagebox.showerror("Configuration", f"Could not read {common.CONFIG_PATH}:\n{e}\n\nStarting with defaults.")
            self.cfg = common.default_config()
        self.selected = 0
        self.thumbs: list = [None] * common.BUTTONS
        self.dirty = False
        self.serial_port = ""
        self._build()
        self._load_display_vars()
        self.refresh_grid()
        self.select(0)
        self.protocol("WM_DELETE_WINDOW", self.on_close)

    # ---------------- layout ----------------
    def _build(self):
        style = ttk.Style(self)
        if "vista" in style.theme_names():
            style.theme_use("vista")
        style.configure("Sel.TButton", foreground="#0a58ca")

        # Device
        dev = ttk.LabelFrame(self, text="Device")
        dev.pack(fill="x", padx=10, pady=(10, 4))
        self.dev_id = tk.StringVar(value=self.cfg["device"].get("id", ""))
        self.dev_ip = tk.StringVar(value=self.cfg["device"].get("ip", ""))
        self.transport = tk.StringVar(value=self.cfg["device"].get("transport", "wifi"))
        self.daemon_serial = tk.BooleanVar(value=self.cfg["device"].get("serial", True))
        ttk.Label(dev, text="Device ID:").grid(row=0, column=0, padx=4, pady=4, sticky="w")
        ttk.Entry(dev, textvariable=self.dev_id, width=16).grid(row=0, column=1, padx=4, sticky="w")
        ttk.Label(dev, text="IP address:").grid(row=0, column=2, padx=4, sticky="w")
        ttk.Entry(dev, textvariable=self.dev_ip, width=16).grid(row=0, column=3, padx=4, sticky="w")
        ttk.Label(dev, text="Upload via:").grid(row=0, column=4, padx=(12, 4), sticky="w")
        ttk.Radiobutton(dev, text="WiFi", value="wifi", variable=self.transport).grid(row=0, column=5)
        ttk.Radiobutton(dev, text="USB serial", value="serial", variable=self.transport).grid(row=0, column=6)
        ttk.Button(dev, text="Find device", command=self.find_device).grid(row=0, column=7, padx=(12, 4))
        ttk.Button(dev, text="Set WiFi...", command=self.set_wifi).grid(row=0, column=8, padx=4)
        ttk.Checkbutton(dev, text="Daemon also listens on USB serial", variable=self.daemon_serial,
                        command=self.mark_dirty).grid(row=1, column=0, columnspan=4, sticky="w", padx=4, pady=(0, 4))
        self.pair_status = tk.StringVar()
        ttk.Label(dev, textvariable=self.pair_status).grid(row=1, column=4, columnspan=3, sticky="w", padx=(12, 4))
        ttk.Button(dev, text="Pair over USB...", command=self.pair).grid(row=1, column=7, padx=(12, 4), pady=(0, 4))
        for v in (self.dev_id, self.dev_ip, self.transport):
            v.trace_add("write", lambda *_: self.mark_dirty())
        self.dev_id.trace_add("write", lambda *_: self.update_pair_status())
        self.update_pair_status()

        mid = ttk.Frame(self)
        mid.pack(fill="both", expand=True, padx=10, pady=4)

        # Grid
        grid = ttk.LabelFrame(mid, text="Buttons (as laid out on the deck)")
        grid.pack(side="left", fill="y")
        self.cells: list[ttk.Button] = []
        for i in range(common.BUTTONS):
            b = ttk.Button(grid, compound="top", width=12, command=lambda i=i: self.select(i))
            b.grid(row=i // common.COLS, column=i % common.COLS, padx=3, pady=3)
            self.cells.append(b)

        # Editor
        ed = ttk.LabelFrame(mid, text="Selected button")
        ed.pack(side="left", fill="both", expand=True, padx=(10, 0))
        self.ed_title = ttk.Label(ed, font=("Segoe UI", 11, "bold"))
        self.ed_title.grid(row=0, column=0, columnspan=4, sticky="w", padx=6, pady=(6, 2))
        ttk.Label(ed, text="Label:").grid(row=1, column=0, sticky="w", padx=6, pady=4)
        self.label_var = tk.StringVar()
        lab = ttk.Entry(ed, textvariable=self.label_var, width=30)
        lab.grid(row=1, column=1, columnspan=3, sticky="w", padx=6)
        self.label_var.trace_add("write", self.on_label)
        ttk.Label(ed, text="(shown when there is no image, max 24 chars)", foreground="#666").grid(
            row=2, column=1, columnspan=3, sticky="w", padx=6)

        ttk.Label(ed, text="Image:").grid(row=3, column=0, sticky="nw", padx=6, pady=4)
        self.preview = ttk.Label(ed, relief="solid")
        self.preview.grid(row=3, column=1, sticky="w", padx=6, pady=4)
        imgbtns = ttk.Frame(ed)
        imgbtns.grid(row=3, column=2, columnspan=2, sticky="w")
        ttk.Button(imgbtns, text="Choose image...", command=self.choose_image).pack(anchor="w", pady=2)
        ttk.Button(imgbtns, text="Remove image", command=self.clear_image).pack(anchor="w", pady=2)
        self.image_name = ttk.Label(imgbtns, foreground="#666")
        self.image_name.pack(anchor="w", pady=2)

        ttk.Label(ed, text="Actions (run in order):").grid(row=4, column=0, columnspan=2, sticky="w", padx=6, pady=(10, 2))
        self.actions = tk.Listbox(ed, height=8, activestyle="none")
        self.actions.grid(row=5, column=0, columnspan=3, sticky="nsew", padx=6)
        self.actions.bind("<Double-Button-1>", lambda e: self.edit_action())
        ed.rowconfigure(5, weight=1)
        ed.columnconfigure(2, weight=1)
        abtn = ttk.Frame(ed)
        abtn.grid(row=5, column=3, sticky="n", padx=6)
        add = ttk.Menubutton(abtn, text="Add action ▾")
        menu = tk.Menu(add, tearoff=False)
        menu.add_command(label="Run a program...", command=lambda: self.add_action("run"))
        menu.add_command(label="Open a URL...", command=lambda: self.add_action("url"))
        menu.add_command(label="Open a file or folder...", command=lambda: self.add_action("open"))
        menu.add_command(label="Wait...", command=lambda: self.add_action("delay"))
        add["menu"] = menu
        add.pack(fill="x", pady=2)
        for text, cmd in (("Edit...", self.edit_action), ("Remove", self.remove_action),
                          ("Move up", lambda: self.move_action(-1)), ("Move down", lambda: self.move_action(1)),
                          ("Test now", self.test_actions)):
            ttk.Button(abtn, text=text, command=cmd).pack(fill="x", pady=2)

        # Display settings
        disp = ttk.LabelFrame(self, text="Time and date row")
        disp.pack(fill="x", padx=10, pady=4)
        self.tz_name = tk.StringVar()
        self.tz_custom = tk.StringVar()
        self.time_fmt = tk.StringVar()
        self.date_fmt = tk.StringVar()
        ttk.Label(disp, text="Time zone:").grid(row=0, column=0, padx=4, pady=4, sticky="w")
        tz = ttk.Combobox(disp, textvariable=self.tz_name, state="readonly", width=30,
                          values=[n for n, _ in common.TIMEZONES] + [common.CUSTOM_TZ])
        tz.grid(row=0, column=1, padx=4, sticky="w")
        ttk.Label(disp, text="POSIX TZ:").grid(row=0, column=2, padx=4, sticky="w")
        self.tz_entry = ttk.Entry(disp, textvariable=self.tz_custom, width=34)
        self.tz_entry.grid(row=0, column=3, padx=4, sticky="w")
        ttk.Label(disp, text="(daylight saving is handled automatically)", foreground="#666").grid(
            row=0, column=4, padx=4, sticky="w")
        ttk.Label(disp, text="Time format:").grid(row=1, column=0, padx=4, pady=4, sticky="w")
        ttk.Combobox(disp, textvariable=self.time_fmt, state="readonly", width=30,
                     values=[n for n, _ in common.TIME_FORMATS]).grid(row=1, column=1, padx=4, sticky="w")
        ttk.Label(disp, text="Date format:").grid(row=1, column=2, padx=4, sticky="w")
        self.date_combo = ttk.Combobox(disp, textvariable=self.date_fmt, state="readonly", width=32,
                                       values=[time.strftime(f) for f in common.DATE_FORMATS])
        self.date_combo.grid(row=1, column=3, padx=4, sticky="w")
        self.tz_name.trace_add("write", self.on_tz)
        for v in (self.tz_custom, self.time_fmt, self.date_fmt):
            v.trace_add("write", lambda *_: self.mark_dirty())

        # Bottom bar
        bottom = ttk.Frame(self)
        bottom.pack(fill="x", padx=10, pady=(4, 10))
        ttk.Button(bottom, text="Save", command=self.save).pack(side="left")
        ttk.Button(bottom, text="Save and upload to deck", command=self.upload).pack(side="left", padx=6)
        self.autostart = tk.BooleanVar(value=autostart_enabled())
        ttk.Checkbutton(bottom, text="Start daemon when I sign in to Windows", variable=self.autostart,
                        command=self.toggle_autostart).pack(side="right")
        ttk.Button(bottom, text="Start daemon now", command=self.start_daemon).pack(side="right", padx=6)
        ttk.Button(bottom, text="Open config folder", command=lambda: os.startfile(common.APP_DIR)).pack(
            side="right", padx=6)
        self.status = ttk.Label(self, text=f"Configuration folder: {common.APP_DIR}", anchor="w",
                                relief="sunken", padding=(6, 2))
        self.status.pack(fill="x", side="bottom")

    # ---------------- helpers ----------------
    def set_status(self, text: str):
        self.status.config(text=text)

    def mark_dirty(self, *_):
        self.dirty = True

    def button(self, i=None) -> dict:
        return self.cfg["buttons"][self.selected if i is None else i]

    def _load_display_vars(self):
        d = self.cfg["display"]
        names = [n for n, _ in common.TIMEZONES]
        self.tz_custom.set(d.get("tz", ""))
        self.tz_name.set(d.get("tz_name") if d.get("tz_name") in names else common.CUSTOM_TZ)
        tf = {f: n for n, f in common.TIME_FORMATS}
        self.time_fmt.set(tf.get(d.get("time_fmt"), common.TIME_FORMATS[0][0]))
        df = d.get("date_fmt", common.DATE_FORMATS[0])
        self.date_fmt.set(time.strftime(df if df in common.DATE_FORMATS else common.DATE_FORMATS[0]))
        self.dirty = False

    def on_tz(self, *_):
        name = self.tz_name.get()
        lookup = dict(common.TIMEZONES)
        if name in lookup:
            self.tz_custom.set(lookup[name])
            self.tz_entry.config(state="readonly")
        else:
            self.tz_entry.config(state="normal")
        self.mark_dirty()

    def collect(self):
        """Copy UI state into self.cfg."""
        dev = self.cfg["device"]
        dev["id"] = self.dev_id.get().strip()
        dev["ip"] = self.dev_ip.get().strip()
        dev["transport"] = self.transport.get()
        dev["serial"] = bool(self.daemon_serial.get())
        d = self.cfg["display"]
        d["tz_name"] = self.tz_name.get()
        d["tz"] = self.tz_custom.get().strip()
        d["time_fmt"] = dict(common.TIME_FORMATS).get(self.time_fmt.get(), "%H:%M")
        examples = [time.strftime(f) for f in common.DATE_FORMATS]
        try:
            d["date_fmt"] = common.DATE_FORMATS[examples.index(self.date_fmt.get())]
        except ValueError:
            pass

    def thumbnail(self, i):
        from PIL import Image, ImageTk

        p = common.icon_path(self.button(i).get("image", ""))
        if not p or not p.exists():
            return None
        try:
            with Image.open(p) as im:
                im = im.convert("RGBA")
                im.thumbnail((THUMB, THUMB))
                bg = Image.new("RGBA", (THUMB, THUMB), (0, 0, 0, 255))
                bg.alpha_composite(im, ((THUMB - im.width) // 2, (THUMB - im.height) // 2))
                return ImageTk.PhotoImage(bg)
        except Exception:
            return None

    def blank_thumb(self):
        if not hasattr(self, "_blank"):
            self._blank = tk.PhotoImage(width=THUMB, height=THUMB)
            self._blank.put("#2a3242", to=(0, 0, THUMB, THUMB))
        return self._blank

    def refresh_cell(self, i):
        self.thumbs[i] = self.thumbnail(i)
        b = self.button(i)
        text = b.get("label") or f"{i // common.COLS + 1},{i % common.COLS + 1}"
        self.cells[i].config(image=self.thumbs[i] or self.blank_thumb(), text=text[:14],
                             style="Sel.TButton" if i == self.selected else "TButton")

    def refresh_grid(self):
        for i in range(common.BUTTONS):
            self.refresh_cell(i)

    def select(self, i):
        old = self.selected
        self.selected = i
        self.refresh_cell(old)
        self.refresh_cell(i)
        b = self.button()
        self.ed_title.config(text=f"Row {i // common.COLS + 1}, column {i % common.COLS + 1}  (position {i})")
        self._loading = True
        self.label_var.set(b.get("label", ""))
        self._loading = False
        self.preview.config(image=self.thumbs[i] or self.blank_thumb())
        self.image_name.config(text=b.get("image") or "No image")
        self.refresh_actions()

    def refresh_actions(self):
        self.actions.delete(0, "end")
        for a in self.button().get("actions", []):
            self.actions.insert("end", common.describe_action(a))

    # ---------------- editing ----------------
    def on_label(self, *_):
        if getattr(self, "_loading", False):
            return
        self.button()["label"] = self.label_var.get()[:24]
        self.refresh_cell(self.selected)
        self.mark_dirty()

    def choose_image(self):
        path = filedialog.askopenfilename(title="Choose an icon image", filetypes=[
            ("Images", "*.png *.jpg *.jpeg *.bmp *.gif *.ico *.webp"), ("All files", "*.*")])
        if not path:
            return
        try:
            common.image_to_rgb565(Path(path))  # validate it can be converted
        except Exception as e:
            messagebox.showerror("Image", f"Could not read that image:\n{e}")
            return
        src = Path(path)
        dest = common.ICONS_DIR / f"{self.selected:02d}_{src.name}"
        if src.resolve() != dest.resolve():
            for old in common.ICONS_DIR.glob(f"{self.selected:02d}_*"):
                old.unlink(missing_ok=True)
            shutil.copy2(src, dest)
        self.button()["image"] = dest.name
        self.select(self.selected)
        self.mark_dirty()
        self.set_status(f"Image copied to {dest}. Images are scaled to {common.ICON_SIZE}x{common.ICON_SIZE} on upload.")

    def clear_image(self):
        self.button()["image"] = ""
        self.select(self.selected)
        self.mark_dirty()

    def selected_action(self):
        sel = self.actions.curselection()
        return sel[0] if sel else None

    def add_action(self, kind):
        dlg = ActionDialog(self, kind)
        if dlg.result:
            self.button().setdefault("actions", []).append(dlg.result)
            self.refresh_actions()
            self.refresh_cell(self.selected)
            self.mark_dirty()

    def edit_action(self):
        idx = self.selected_action()
        if idx is None:
            return
        action = self.button()["actions"][idx]
        dlg = ActionDialog(self, action.get("type", "run"), action)
        if dlg.result:
            self.button()["actions"][idx] = dlg.result
            self.refresh_actions()
            self.mark_dirty()

    def remove_action(self):
        idx = self.selected_action()
        if idx is not None:
            del self.button()["actions"][idx]
            self.refresh_actions()
            self.refresh_cell(self.selected)
            self.mark_dirty()

    def move_action(self, delta):
        idx = self.selected_action()
        acts = self.button().get("actions", [])
        if idx is None or not 0 <= idx + delta < len(acts):
            return
        acts[idx], acts[idx + delta] = acts[idx + delta], acts[idx]
        self.refresh_actions()
        self.actions.selection_set(idx + delta)
        self.mark_dirty()

    def test_actions(self):
        import cydsd_daemon

        acts = list(self.button().get("actions", []))
        if not acts:
            self.set_status("This button has no actions.")
            return

        def worker():
            for a in acts:
                try:
                    cydsd_daemon.run_action(a)
                except Exception as e:
                    self.after(0, messagebox.showerror, "Action failed", f"{common.describe_action(a)}\n\n{e}")
                    return

        threading.Thread(target=worker, daemon=True).start()

    # ---------------- persistence ----------------
    def save(self) -> bool:
        self.collect()
        if not self.cfg["display"]["tz"]:
            messagebox.showerror("Time zone", "Enter a POSIX TZ string for the custom time zone.")
            return False
        try:
            common.save_config(self.cfg)
        except Exception as e:
            messagebox.showerror("Save", f"Could not save the configuration:\n{e}")
            return False
        self.dirty = False
        self.set_status(f"Saved {common.CONFIG_PATH}")
        return True

    def on_close(self):
        if self.dirty:
            ans = messagebox.askyesnocancel("Unsaved changes", "Save changes before closing?")
            if ans is None or (ans and not self.save()):
                return
        self.destroy()

    # ---------------- device ----------------
    def run_bg(self, label, fn, done=None):
        """Run fn in a worker thread; report errors in a dialog."""
        self.set_status(label + "...")
        self.config(cursor="watch")

        def worker():
            try:
                result = fn()
            except Exception as e:
                err = e
                self.after(0, lambda: (self.config(cursor=""), self.set_status(f"{label} failed: {err}"),
                                       messagebox.showerror(label, str(err))))
                return
            self.after(0, lambda: (self.config(cursor=""), done(result) if done else self.set_status(label + ": done")))

        threading.Thread(target=worker, daemon=True).start()

    def resolve_serial_port(self) -> str:
        """Find the COM port of our deck (the daemon is paused while probing)."""
        common.ensure_dirs()
        common.PAUSE_SERIAL_FLAG.touch()
        try:
            time.sleep(1.5)
            devs = common.find_serial_devices(self.cfg["device"].get("id", ""))
        finally:
            common.PAUSE_SERIAL_FLAG.unlink(missing_ok=True)
        if not devs:
            raise RuntimeError("No CYD Stream Deck found on any COM port.")
        return devs[0]["port"]

    def client(self) -> common.DeviceClient:
        dev = self.cfg["device"]
        if dev["transport"] == "serial":
            if not self.serial_port:
                self.serial_port = self.resolve_serial_port()
            return common.DeviceClient("serial", port=self.serial_port)
        if not dev.get("ip"):
            raise RuntimeError("No device IP address. Use 'Find device' or switch to USB serial.")
        return common.DeviceClient("wifi", ip=dev["ip"], key=common.load_key(dev.get("id", "")))

    def update_pair_status(self):
        paired = common.load_key(self.dev_id.get().strip()) is not None
        self.pair_status.set("Paired: WiFi is authenticated" if paired else "Not paired: WiFi is unauthenticated")

    def pair(self):
        self.collect()
        if not messagebox.askokcancel(
                "Pair over USB",
                "Pair this PC with the deck? The deck must be plugged in by USB.\n\n"
                "Afterwards the deck only accepts signed WiFi messages, so only paired PCs can "
                "configure it or receive its presses over WiFi. USB keeps working without pairing.\n\n"
                "Pairing again replaces the key, which unpairs any other PC. Holding the time row "
                "on the deck for 10 s (WiFi reset) also clears the pairing."):
            return

        def work():
            if not self.serial_port:
                self.serial_port = self.resolve_serial_port()
            key = common.new_key()
            with common.DeviceClient("serial", port=self.serial_port) as dev:
                dev_id = dev.info()["id"]
                dev.pair(key)
            common.save_key(dev_id, key)
            return dev_id

        def done(dev_id):
            if not self.dev_id.get().strip():
                self.dev_id.set(dev_id)
                self.save()
            self.update_pair_status()
            self.set_status(f"Paired with {dev_id}. The daemon picks up the key automatically.")

        self.run_bg("Pairing over USB", work, done)

    def find_device(self):
        self.collect()
        known_ip = self.cfg["device"].get("ip", "")

        def work():
            devs = common.udp_discover(extra_ips=[known_ip] if known_ip else None)
            for d in devs:
                d["via"] = "WiFi"
            if not devs:
                common.PAUSE_SERIAL_FLAG.touch()
                try:
                    time.sleep(1.5)
                    for d in common.find_serial_devices():
                        d["via"] = f"USB {d['port']}"
                        devs.append(d)
                finally:
                    common.PAUSE_SERIAL_FLAG.unlink(missing_ok=True)
            return devs

        def done(devs):
            if not devs:
                self.set_status("No deck found.")
                messagebox.showwarning("Find device", "No CYD Stream Deck found on the network or on USB.\n\n"
                                       "Check it is powered, and that it shows the time (it is on WiFi) "
                                       "or is plugged in by USB.")
                return
            want = self.dev_id.get().strip().upper()
            d = next((x for x in devs if x["id"].upper() == want), devs[0])
            if len(devs) > 1 and not want:
                listing = "\n".join(f"{x['id']}  {x['ip']}  ({x['via']})" for x in devs)
                messagebox.showinfo("Find device", f"Several decks found; using the first:\n\n{listing}\n\n"
                                    "Enter a Device ID to choose another.")
            self.dev_id.set(d["id"])
            if d.get("ip") and d["ip"] != "0.0.0.0":
                self.dev_ip.set(d["ip"])
            if d["via"].startswith("USB"):
                self.serial_port = d["port"]
                if not d.get("ip") or d["ip"] == "0.0.0.0":
                    self.transport.set("serial")
            self.set_status(f"Found {d['id']} at {d.get('ip') or '-'} via {d['via']}")

        self.run_bg("Searching for the deck", work, done)

    def upload(self):
        if not self.save():
            return
        cfg = self.cfg

        def work():
            images = []
            for i, b in enumerate(cfg["buttons"]):
                p = common.icon_path(b.get("image", ""))
                images.append(common.image_to_rgb565(p) if p and p.exists() else None)
            with self.client() as dev:
                info = dev.info()
                if info.get("icon_size") != common.ICON_SIZE:
                    raise RuntimeError(f"Unexpected device icon size {info.get('icon_size')}")
                dev.set_settings(common.device_settings_payload(cfg))
                for i, data in enumerate(images):
                    self.after(0, self.set_status, f"Uploading icon {i + 1} of {common.BUTTONS}...")
                    if data:
                        dev.upload_icon(i, data)
                    elif info.get("icons", [False] * common.BUTTONS)[i]:
                        dev.delete_icon(i)
                return info

        def done(info):
            self.set_status(f"Uploaded to {info.get('id')} ({info.get('ip') or self.cfg['device']['transport']}).")
            if not self.dev_id.get():
                self.dev_id.set(info.get("id", ""))
                self.save()

        self.run_bg("Uploading to the deck", work, done)

    def set_wifi(self):
        self.collect()
        dlg = WifiDialog(self, "Set deck WiFi")
        if not dlg.result:
            return
        ssid, pw = dlg.result

        def work():
            with self.client() as dev:
                dev.set_wifi(ssid, pw)

        if self.cfg["device"]["transport"] != "serial":
            messagebox.showinfo("Set deck WiFi", "Tap Yes on the deck's screen within 30 seconds "
                                "to accept the new network.")
        self.run_bg("Sending WiFi credentials (accept on the deck)", work,
                    lambda _: self.set_status("WiFi credentials sent; the deck is restarting. "
                                              "Use 'Find device' in ~20 s to get its new address."))

    # ---------------- daemon ----------------
    def start_daemon(self):
        subprocess.Popen([pythonw(), str(DAEMON_SCRIPT)], cwd=str(HERE),
                         creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP,
                         close_fds=True)
        self.set_status(f"Daemon started (if not already running). Log: {common.LOG_PATH}")

    def toggle_autostart(self):
        try:
            set_autostart(self.autostart.get())
            if self.autostart.get():
                self.set_status("Daemon will start when you sign in to Windows.")
            else:
                self.set_status("Daemon will no longer start at sign-in.")
        except Exception as e:
            messagebox.showerror("Start at sign-in", str(e))


def main():
    try:  # crisp rendering on scaled displays
        import ctypes

        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass
    app = ConfigApp()
    app.mainloop()


if __name__ == "__main__":
    main()

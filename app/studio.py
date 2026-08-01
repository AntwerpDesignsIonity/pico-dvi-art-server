"""Pico DVI Art Studio - the desktop application that runs the whole appliance.

This is a native desktop app (Tkinter), not a web page: no browser, no npm, no
Electron download, no dev server. Tkinter ships with Python, so START.bat can
launch it on a clean machine with nothing but the repo checked out.

It owns everything the appliance needs:

  * the art server, running in-process on a background thread - there is no
    separate console to babysit and no port for anyone to type in,
  * every setting in pc_server/config.py, grouped and validated,
  * a live preview of exactly the pixels the Pico is being sent,
  * device control: build firmware, flash it, push an OTA update.

Nothing here asks the user to pick a port, an IP or a file path.
"""

from __future__ import annotations

import io
import json
import queue
import socket
import subprocess
import sys
import threading
import time
import tkinter as tk
from dataclasses import fields
from pathlib import Path
from tkinter import messagebox, ttk

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "pc_server"))

import numpy as np  # noqa: E402

from config import DVI_MODES, Config  # noqa: E402
from server import ArtServer, local_addresses  # noqa: E402

PYTHON = sys.executable
POLL_MS = 120
PREVIEW_ZOOM = 2

# (field name, label, widget kind, options)
SETTING_GROUPS: list[tuple[str, list[tuple[str, str, str, object]]]] = [
    ("Display", [
        ("dvi_mode", "Panel mode", "choice", sorted(DVI_MODES)),
        ("fps", "Target frames/second", "float", None),
        ("byte_order", "Byte order", "choice", ["little", "big"]),
    ]),
    ("Art", [
        ("source", "Art source", "choice", ["shader", "ai", "hybrid"]),
        ("speed", "Animation speed", "float", None),
        ("seed", "Seed (blank = random)", "text", None),
        ("border_thickness", "Border thickness (px)", "int", None),
        ("border_intensity", "Border intensity", "float", None),
    ]),
    ("Clock / HUD", [
        ("hud", "Show HUD", "bool", None),
        ("clock_24h", "24-hour clock", "bool", None),
        ("show_seconds", "Show seconds", "bool", None),
        ("date_format", "Date format", "text", None),
        ("timezone", "Timezone (blank = local)", "text", None),
        ("hud_margin", "HUD margin (px)", "int", None),
        ("hud_scale_clock", "Clock scale", "int", None),
        ("hud_scale_small", "Label scale", "int", None),
    ]),
    ("Temperature", [
        ("temp_source", "Source", "choice", ["weather", "cpu", "static", "none"]),
        ("temp_static_c", "Static value (C)", "float", None),
        ("latitude", "Latitude", "float", None),
        ("longitude", "Longitude", "float", None),
        ("temp_refresh_s", "Refresh (s)", "float", None),
        ("temp_label_server", "Server label", "text", None),
        ("temp_label_local", "Device label", "text", None),
    ]),
    ("AI images", [
        ("ai_enabled", "Enable AI source", "bool", None),
        ("ai_provider", "Provider", "choice", ["openai", "folder"]),
        ("ai_model", "Model", "text", None),
        ("ai_size", "Requested size", "text", None),
        ("ai_interval_s", "New image every (s)", "float", None),
        ("ai_fade_s", "Cross-fade (s)", "float", None),
        ("ai_folder", "Local folder", "text", None),
        ("ai_api_key_env", "API key env var", "text", None),
    ]),
    ("Network", [
        ("host", "Listen address", "text", None),
        ("port", "Port", "int", None),
    ]),
]


class LogTee(io.TextIOBase):
    """Mirror everything the server prints into the GUI log pane."""

    def __init__(self, sink: queue.Queue, passthrough) -> None:
        self.sink = sink
        self.passthrough = passthrough

    def write(self, text: str) -> int:
        if text:
            self.sink.put(text)
            if self.passthrough is not None:
                try:
                    self.passthrough.write(text)
                except (ValueError, OSError):
                    pass
        return len(text)

    def flush(self) -> None:
        if self.passthrough is not None:
            try:
                self.passthrough.flush()
            except (ValueError, OSError):
                pass


class Studio(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("Pico DVI Art Studio")
        self.geometry("1180x780")
        self.minsize(980, 640)

        self.cfg = Config.load()
        self.server: ArtServer | None = None
        self.server_thread: threading.Thread | None = None
        self.logq: queue.Queue = queue.Queue()
        self.vars: dict[str, tk.Variable] = {}
        self._preview_img: tk.PhotoImage | None = None
        self._busy = False

        sys.stdout = LogTee(self.logq, sys.__stdout__)
        sys.stderr = LogTee(self.logq, sys.__stderr__)

        self._build_ui()
        self._load_into_widgets()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(POLL_MS, self._tick)
        self.log(f"Ready. Repo: {REPO}")
        self.start_server()

    # -- construction ----------------------------------------------------
    def _build_ui(self) -> None:
        bar = ttk.Frame(self, padding=(10, 8))
        bar.pack(side="top", fill="x")

        self.btn_server = ttk.Button(bar, text="Stop server", command=self.toggle_server)
        self.btn_server.pack(side="left")
        ttk.Button(bar, text="Save settings", command=self.save_settings).pack(
            side="left", padx=(8, 0)
        )
        ttk.Separator(bar, orient="vertical").pack(side="left", fill="y", padx=10)
        self.btn_build = ttk.Button(
            bar, text="Build + flash firmware", command=self.build_and_flash
        )
        self.btn_build.pack(side="left")
        self.btn_ota = ttk.Button(bar, text="Push OTA", command=self.push_ota)
        self.btn_ota.pack(side="left", padx=(8, 0))

        self.status = ttk.Label(bar, text="", anchor="e")
        self.status.pack(side="right", fill="x", expand=True)

        body = ttk.PanedWindow(self, orient="horizontal")
        body.pack(fill="both", expand=True, padx=10, pady=(0, 10))

        book = ttk.Notebook(body)
        for title, spec in SETTING_GROUPS:
            book.add(self._settings_page(book, spec), text=title)
        body.add(book, weight=3)

        right = ttk.Frame(body)
        body.add(right, weight=4)

        pv = ttk.LabelFrame(right, text="Live preview - exactly what the panel receives")
        pv.pack(fill="x")
        self.preview = tk.Label(pv, background="#0b0b0f")
        self.preview.pack(padx=8, pady=8)

        dev = ttk.LabelFrame(right, text="Device")
        dev.pack(fill="x", pady=(10, 0))
        self.device_label = ttk.Label(dev, text="waiting...", justify="left")
        self.device_label.pack(anchor="w", padx=8, pady=8)

        logf = ttk.LabelFrame(right, text="Log")
        logf.pack(fill="both", expand=True, pady=(10, 0))
        self.logbox = tk.Text(
            logf, height=10, wrap="none", background="#11131a",
            foreground="#c8d0e0", insertbackground="#c8d0e0", relief="flat",
        )
        self.logbox.pack(side="left", fill="both", expand=True, padx=(8, 0), pady=8)
        sb = ttk.Scrollbar(logf, orient="vertical", command=self.logbox.yview)
        sb.pack(side="right", fill="y", pady=8, padx=(0, 8))
        self.logbox.configure(yscrollcommand=sb.set, state="disabled")

    def _settings_page(self, parent, spec) -> ttk.Frame:
        page = ttk.Frame(parent, padding=12)
        page.columnconfigure(1, weight=1)
        for row, (name, label, kind, options) in enumerate(spec):
            ttk.Label(page, text=label).grid(
                row=row, column=0, sticky="w", pady=4, padx=(0, 10)
            )
            if kind == "bool":
                var: tk.Variable = tk.BooleanVar()
                ttk.Checkbutton(page, variable=var).grid(row=row, column=1, sticky="w")
            elif kind == "choice":
                var = tk.StringVar()
                ttk.Combobox(
                    page, textvariable=var, values=list(options), state="readonly"
                ).grid(row=row, column=1, sticky="ew")
            else:
                var = tk.StringVar()
                ttk.Entry(page, textvariable=var).grid(row=row, column=1, sticky="ew")
            self.vars[name] = var
        return page

    # -- settings --------------------------------------------------------
    def _load_into_widgets(self) -> None:
        for name, var in self.vars.items():
            value = getattr(self.cfg, name, "")
            if isinstance(var, tk.BooleanVar):
                var.set(bool(value))
            else:
                var.set("" if value is None else str(value))

    def _collect(self) -> dict:
        known = {f.name for f in fields(Config)}
        return {n: v.get() for n, v in self.vars.items() if n in known}

    def save_settings(self) -> bool:
        candidate = Config.load()
        try:
            candidate.update(self._collect())
            candidate.validate()
        except (ValueError, TypeError) as exc:
            messagebox.showerror("Invalid setting", str(exc))
            return False

        payload = candidate.as_dict()
        payload.pop("extra", None)
        (REPO / "pc_server" / "config.json").write_text(
            json.dumps(payload, indent=2), encoding="utf-8"
        )
        self.cfg = candidate
        self._load_into_widgets()
        self.log(
            f"Settings saved. Panel {self.cfg.dvi_mode} "
            f"-> {self.cfg.width}x{self.cfg.height} framebuffer."
        )
        if self.server is not None:
            self.log("Restarting server so the new settings take effect.")
            self.stop_server()
            self.start_server()
        return True

    # -- server ----------------------------------------------------------
    def toggle_server(self) -> None:
        if self.server is None:
            self.start_server()
        else:
            self.stop_server()

    def start_server(self) -> None:
        if self.server is not None:
            return
        try:
            self.cfg = Config.load()
        except ValueError as exc:
            messagebox.showerror("Invalid configuration", str(exc))
            return
        self.server = ArtServer(self.cfg)
        self.server_thread = threading.Thread(
            target=self._serve, name="art-server", daemon=True
        )
        self.server_thread.start()
        self.btn_server.configure(text="Stop server")

    def _serve(self) -> None:
        srv = self.server
        try:
            srv.serve_forever()
        except OSError as exc:
            self.logq.put(f"[!] server stopped: {exc}\n")
        finally:
            if self.server is srv:
                self.server = None

    def stop_server(self) -> None:
        srv = self.server
        if srv is None:
            return
        srv._stop.set()
        # Unblock the accept() loop by connecting to ourselves.
        try:
            with socket.create_connection(("127.0.0.1", self.cfg.port), timeout=1):
                pass
        except OSError:
            pass
        if self.server_thread is not None:
            self.server_thread.join(timeout=4)
        self.server = None
        self.btn_server.configure(text="Start server")
        self.log("Server stopped.")

    # -- device ----------------------------------------------------------
    def build_and_flash(self) -> None:
        if self._busy:
            return
        if not self.save_settings():
            return
        args = [PYTHON, str(REPO / "tools" / "build_firmware.py"), "--flash",
                "--mode", self.cfg.dvi_mode]
        self._run_tool(args, "firmware build + flash")

    def push_ota(self) -> None:
        srv = self.server
        if srv is None:
            messagebox.showinfo("Not running", "Start the server first.")
            return
        sent = srv.broadcast_command({"action": "ota"})
        self.log(f"OTA requested on {sent} device(s).")

    def _run_tool(self, args: list[str], what: str) -> None:
        self._busy = True
        self.btn_build.configure(state="disabled")
        self.log(f"--- {what} ---")

        def worker() -> None:
            try:
                proc = subprocess.Popen(
                    args, cwd=str(REPO), stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, text=True, bufsize=1,
                )
                for line in proc.stdout:
                    self.logq.put(line)
                code = proc.wait()
                self.logq.put(f"--- {what} finished (exit {code}) ---\n")
            except OSError as exc:
                self.logq.put(f"[!] {what} failed: {exc}\n")
            finally:
                self._busy = False
                self.btn_build.configure(state="normal")

        threading.Thread(target=worker, daemon=True).start()

    # -- periodic --------------------------------------------------------
    def _tick(self) -> None:
        self._drain_log()
        self._refresh_preview()
        self._refresh_status()
        self.after(POLL_MS, self._tick)

    def _drain_log(self) -> None:
        chunks = []
        while True:
            try:
                chunks.append(self.logq.get_nowait())
            except queue.Empty:
                break
        if not chunks:
            return
        self.logbox.configure(state="normal")
        self.logbox.insert("end", "".join(chunks))
        # Keep the pane bounded so a long run cannot eat all the memory.
        if int(self.logbox.index("end-1c").split(".")[0]) > 800:
            self.logbox.delete("1.0", "300.0")
        self.logbox.see("end")
        self.logbox.configure(state="disabled")

    def _refresh_preview(self) -> None:
        srv = self.server
        if srv is None:
            return
        try:
            frame = srv.render_frame()
            data = _to_ppm(frame)
        except Exception as exc:  # a bad shader must not take the UI down
            self.log(f"[!] preview failed: {exc}")
            return
        self._preview_img = tk.PhotoImage(data=data).zoom(PREVIEW_ZOOM)
        self.preview.configure(image=self._preview_img)

    def _refresh_status(self) -> None:
        srv = self.server
        if srv is None:
            self.status.configure(text="server stopped")
            self.device_label.configure(text="Server stopped.")
            self.btn_server.configure(text="Start server")
            return

        with srv._clients_lock:
            clients = list(srv._clients)
        addrs = ", ".join(local_addresses())
        self.status.configure(
            text=f"serving {self.cfg.width}x{self.cfg.height} @ {self.cfg.fps:g} fps "
                 f"on {addrs}:{self.cfg.port}  |  {len(clients)} device(s)"
        )

        if not clients:
            self.device_label.configure(
                text="No device connected.\n"
                     "The Pico shows a diagonal standby pattern until it joins\n"
                     "Wi-Fi and reaches this server."
            )
            return

        lines = []
        for session, _conn in clients:
            with session.lock:
                temp = session.local_temp_c
                fps = session.client_fps
                seen = time.time() - session.last_seen
                who = f"{session.device_id or '?'} @ {session.address[0]}"
                mcu = f"{temp:.1f}C" if temp is not None else "?"
                lines.append(f"{who}   fw={session.fw_version}   mcu={mcu}")
                lines.append(
                    f"    device fps={fps if fps is not None else '?'}   "
                    f"last heard {seen:.0f}s ago"
                )
        self.device_label.configure(text="\n".join(lines))

    # -- misc ------------------------------------------------------------
    def log(self, message: str) -> None:
        self.logq.put(message.rstrip() + "\n")

    def _on_close(self) -> None:
        self.stop_server()
        sys.stdout = sys.__stdout__
        sys.stderr = sys.__stderr__
        self.destroy()


def _to_ppm(frame: np.ndarray) -> bytes:
    """Binary PPM, which Tk's PhotoImage reads natively - no Pillow needed."""
    rgb = np.ascontiguousarray(frame[:, :, :3].astype(np.uint8))
    height, width = rgb.shape[:2]
    return b"P6\n%d %d\n255\n" % (width, height) + rgb.tobytes()


def main() -> None:
    Studio().mainloop()


if __name__ == "__main__":
    main()

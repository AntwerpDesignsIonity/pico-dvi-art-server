"""Native Pico-only desktop launcher for firmware build and flashing.

The repository no longer ships a PC preview or control server. This app keeps
the local display build settings, shows USB/BOOTSEL state, and shells out to
tools/build_firmware.py for the actual work.
"""

from __future__ import annotations

import ctypes
import queue
import shutil
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, ttk

from studio_settings import DVI_MODES, StudioSettings, load_settings, save_settings

REPO = (
    Path(sys.executable).resolve().parent
    if getattr(sys, "frozen", False)
    else Path(__file__).resolve().parent.parent
)
sys.path.insert(0, str(REPO / "tools"))

from pico_device import describe_connected_device  # noqa: E402

POLL_MS = 150
DEVICE_REFRESH_MS = 1000
UF2_PATH = REPO / "pico_firmware_c" / "build" / "pico_dvi_art_client.uf2"
_INSTANCE_MUTEX = None


def _find_system_python() -> str | None:
    if not getattr(sys, "frozen", False):
        return sys.executable
    for candidate in ("py", "python"):
        found = shutil.which(candidate)
        if found:
            return found
    return None


class Studio(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("Pico DVI Firmware Studio")
        self.geometry("1080x720")
        self.minsize(920, 620)

        try:
            self.settings = load_settings()
        except (OSError, ValueError) as exc:
            messagebox.showwarning(
                "Settings reset",
                f"Stored settings could not be loaded and were reset.\n\n{exc}",
            )
            self.settings = StudioSettings()

        self.logq = queue.Queue()
        self.vars: dict[str, tk.Variable] = {}
        self._busy = False

        self._build_ui()
        self._load_into_widgets()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(POLL_MS, self._tick)
        self.after(0, self._refresh_device_status)
        self.log("Ready. This workspace now builds and flashes Pico firmware only.")

    def _build_ui(self) -> None:
        bar = ttk.Frame(self, padding=(10, 8))
        bar.pack(side="top", fill="x")

        self.btn_save = ttk.Button(bar, text="Save settings", command=self.save_form_settings)
        self.btn_save.pack(side="left")
        self.btn_build = ttk.Button(bar, text="Build firmware", command=self.build_firmware)
        self.btn_build.pack(side="left", padx=(8, 0))
        self.btn_flash = ttk.Button(bar, text="Flash existing UF2", command=self.flash_existing)
        self.btn_flash.pack(side="left", padx=(8, 0))
        self.btn_build_flash = ttk.Button(
            bar,
            text="Build + flash",
            command=self.build_and_flash,
        )
        self.btn_build_flash.pack(side="left", padx=(8, 0))

        self.status = ttk.Label(bar, text="Idle", anchor="e")
        self.status.pack(side="right", fill="x", expand=True)

        body = ttk.Frame(self, padding=(10, 0, 10, 10))
        body.pack(fill="both", expand=True)
        body.columnconfigure(0, weight=3)
        body.columnconfigure(1, weight=5)
        body.rowconfigure(0, weight=1)

        left = ttk.Frame(body)
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 10))
        right = ttk.Frame(body)
        right.grid(row=0, column=1, sticky="nsew")
        right.rowconfigure(2, weight=1)

        config_box = ttk.LabelFrame(left, text="Firmware configuration", padding=12)
        config_box.pack(fill="x")
        config_box.columnconfigure(1, weight=1)

        ttk.Label(config_box, text="Panel mode").grid(row=0, column=0, sticky="w", pady=4)
        mode_var = tk.StringVar()
        ttk.Combobox(
            config_box,
            textvariable=mode_var,
            values=list(DVI_MODES),
            state="readonly",
        ).grid(row=0, column=1, sticky="ew", pady=4)
        self.vars["dvi_mode"] = mode_var

        ttk.Label(config_box, text="TMDS polarity").grid(row=1, column=0, sticky="w", pady=4)
        polarity_var = tk.StringVar()
        ttk.Combobox(
            config_box,
            textvariable=polarity_var,
            values=["1", "0"],
            state="readonly",
        ).grid(row=1, column=1, sticky="ew", pady=4)
        self.vars["dvi_invert_diffpairs"] = polarity_var

        help_box = ttk.LabelFrame(left, text="Purpose", padding=12)
        help_box.pack(fill="x", pady=(10, 0))
        ttk.Label(
            help_box,
            text=(
                "This repository is now Pico-only.\n\n"
                "The Pico renders the art locally in C. The desktop side is used only "
                "to build the UF2 and flash it over USB. There is no control server, "
                "desktop preview, or runtime network dependency."
            ),
            justify="left",
            wraplength=300,
        ).pack(anchor="w")

        workflow_box = ttk.LabelFrame(left, text="Workflow", padding=12)
        workflow_box.pack(fill="x", pady=(10, 0))
        ttk.Label(
            workflow_box,
            text=(
                "1. Connect the Pico by USB.\n"
                "2. Keep 640x480 / polarity 1 as the safe default.\n"
                "3. Click Build + flash.\n"
                "4. If auto-reboot into BOOTSEL fails, reconnect while holding BOOTSEL."
            ),
            justify="left",
            wraplength=300,
        ).pack(anchor="w")

        device_box = ttk.LabelFrame(right, text="Connected device", padding=12)
        device_box.grid(row=0, column=0, sticky="ew")
        self.device_label = ttk.Label(device_box, text="Detecting device...", justify="left")
        self.device_label.pack(anchor="w")

        artifact_box = ttk.LabelFrame(right, text="Firmware artifact", padding=12)
        artifact_box.grid(row=1, column=0, sticky="ew", pady=(10, 0))
        ttk.Label(
            artifact_box,
            text=f"UF2 path: {UF2_PATH}",
            justify="left",
            wraplength=620,
        ).pack(anchor="w")

        log_box = ttk.LabelFrame(right, text="Build log", padding=8)
        log_box.grid(row=2, column=0, sticky="nsew", pady=(10, 0))
        log_box.rowconfigure(0, weight=1)
        log_box.columnconfigure(0, weight=1)
        self.logbox = tk.Text(
            log_box,
            wrap="none",
            background="#11131a",
            foreground="#c8d0e0",
            insertbackground="#c8d0e0",
            relief="flat",
        )
        self.logbox.grid(row=0, column=0, sticky="nsew")
        scroll = ttk.Scrollbar(log_box, orient="vertical", command=self.logbox.yview)
        scroll.grid(row=0, column=1, sticky="ns")
        self.logbox.configure(yscrollcommand=scroll.set, state="disabled")

    def _load_into_widgets(self) -> None:
        self.vars["dvi_mode"].set(self.settings.dvi_mode)
        self.vars["dvi_invert_diffpairs"].set(str(self.settings.dvi_invert_diffpairs))

    def _current_settings(self) -> StudioSettings:
        settings = StudioSettings()
        settings.update(
            {
                "dvi_mode": self.vars["dvi_mode"].get(),
                "dvi_invert_diffpairs": self.vars["dvi_invert_diffpairs"].get(),
            }
        )
        return settings

    def save_form_settings(self) -> bool:
        try:
            settings = self._current_settings()
            save_settings(settings)
        except (OSError, ValueError) as exc:
            messagebox.showerror("Invalid setting", str(exc))
            return False
        self.settings = settings
        self.log(
            "Settings saved: "
            f"mode={self.settings.dvi_mode}, "
            f"polarity={self.settings.dvi_invert_diffpairs}."
        )
        return True

    def _tool_python(self) -> str | None:
        python = _find_system_python()
        if python is not None:
            return python
        messagebox.showerror(
            "Python not found",
            "A Python 3.9+ installation is required to run tools/build_firmware.py.",
        )
        return None

    def build_firmware(self) -> None:
        if self._busy or not self.save_form_settings():
            return
        python = self._tool_python()
        if python is None:
            return
        args = [
            python,
            str(REPO / "tools" / "build_firmware.py"),
            "--mode",
            self.settings.dvi_mode,
            "--invert-diffpairs",
            str(self.settings.dvi_invert_diffpairs),
        ]
        self._run_tool(args, "build firmware")

    def flash_existing(self) -> None:
        if self._busy or not self.save_form_settings():
            return
        python = self._tool_python()
        if python is None:
            return
        args = [
            python,
            str(REPO / "tools" / "build_firmware.py"),
            "--no-build",
            "--flash",
        ]
        self._run_tool(args, "flash existing uf2")

    def build_and_flash(self) -> None:
        if self._busy or not self.save_form_settings():
            return
        python = self._tool_python()
        if python is None:
            return
        args = [
            python,
            str(REPO / "tools" / "build_firmware.py"),
            "--flash",
            "--mode",
            self.settings.dvi_mode,
            "--invert-diffpairs",
            str(self.settings.dvi_invert_diffpairs),
        ]
        self._run_tool(args, "build and flash")

    def _run_tool(self, args: list[str], what: str) -> None:
        self._busy = True
        self._set_buttons("disabled")
        self.status.configure(text=f"Running {what}...")
        self.log(f"--- {what} ---")

        def worker() -> None:
            exit_code = 1
            try:
                proc = subprocess.Popen(
                    args,
                    cwd=str(REPO),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    bufsize=1,
                )
                assert proc.stdout is not None
                for line in proc.stdout:
                    self.logq.put(("log", line))
                exit_code = proc.wait()
            except OSError as exc:
                self.logq.put(("log", f"[!] {what} failed: {exc}\n"))
            finally:
                self.logq.put(("done", (what, exit_code)))

        threading.Thread(target=worker, name=f"tool-{what}", daemon=True).start()

    def _set_buttons(self, state: str) -> None:
        for button in (self.btn_save, self.btn_build, self.btn_flash, self.btn_build_flash):
            button.configure(state=state)

    def _tick(self) -> None:
        self._drain_log()
        self.after(POLL_MS, self._tick)

    def _drain_log(self) -> None:
        chunks: list[str] = []
        while True:
            try:
                kind, payload = self.logq.get_nowait()
            except queue.Empty:
                break
            if kind == "log":
                chunks.append(str(payload))
            elif kind == "done":
                what, exit_code = payload
                self._busy = False
                self._set_buttons("normal")
                self.status.configure(
                    text=(
                        f"Finished {what}."
                        if int(exit_code) == 0
                        else f"{what.capitalize()} failed."
                    )
                )
                chunks.append(f"--- {what} finished (exit {exit_code}) ---\n")
        if not chunks:
            return
        self.logbox.configure(state="normal")
        self.logbox.insert("end", "".join(chunks))
        if int(self.logbox.index("end-1c").split(".")[0]) > 800:
            self.logbox.delete("1.0", "300.0")
        self.logbox.see("end")
        self.logbox.configure(state="disabled")

    def _refresh_device_status(self) -> None:
        self.device_label.configure(text=describe_connected_device())
        self.after(DEVICE_REFRESH_MS, self._refresh_device_status)

    def log(self, message: str) -> None:
        line = message if message.endswith("\n") else message + "\n"
        self.logq.put(("log", line))

    def _on_close(self) -> None:
        self.destroy()


def main() -> None:
    global _INSTANCE_MUTEX
    if sys.platform == "win32":
        _INSTANCE_MUTEX = ctypes.windll.kernel32.CreateMutexW(
            None,
            False,
            "Local\\PicoDviFirmwareStudio",
        )
        if ctypes.windll.kernel32.GetLastError() == 183:
            ctypes.windll.user32.MessageBoxW(
                None,
                "Pico DVI Firmware Studio is already running.",
                "Pico DVI Firmware Studio",
                0x40,
            )
            return
    Studio().mainloop()


if __name__ == "__main__":
    main()

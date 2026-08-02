from __future__ import annotations

import sys
import time
from pathlib import Path

RPI_VID = 0x2E8A


def find_bootsel_drives() -> list[Path]:
    drives: list[Path] = []
    if sys.platform == "win32":
        candidates = [Path(f"{chr(letter)}:\\") for letter in range(ord("D"), ord("Z") + 1)]
    else:
        candidates = []
        for base in ("/media", "/run/media", "/Volumes"):
            root = Path(base)
            if root.is_dir():
                candidates.extend(p for p in root.iterdir() if p.is_dir())
    for drive in candidates:
        info = drive / "INFO_UF2.TXT"
        try:
            if info.is_file():
                drives.append(drive)
        except OSError:
            continue
    return drives


def discover_raspberry_pi_ports() -> list[str]:
    try:
        from serial.tools import list_ports
    except ImportError:
        return []
    return sorted(
        {
            info.device
            for info in list_ports.comports()
            if info.vid == RPI_VID and info.device
        }
    )


def wait_for_bootsel(timeout_s: float = 8.0) -> Path | None:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        drives = find_bootsel_drives()
        if drives:
            return drives[0]
        time.sleep(0.5)
    return None


def reboot_to_bootsel(port: str, timeout_s: float = 8.0) -> Path | None:
    try:
        import serial
    except ImportError:
        return None

    try:
        handle = serial.Serial(port, 1200)
        handle.dtr = False
        time.sleep(0.3)
        handle.close()
    except Exception:
        pass
    return wait_for_bootsel(timeout_s)


def describe_connected_device() -> str:
    drives = find_bootsel_drives()
    if drives:
        return (
            f"BOOTSEL drive detected at {drives[0]}.\n"
            "The Pico is ready to receive a UF2 now."
        )

    ports = discover_raspberry_pi_ports()
    if ports:
        joined = ", ".join(ports)
        noun = "device" if len(ports) == 1 else "devices"
        return (
            f"Raspberry Pi USB {noun} detected on {joined}.\n"
            "Build + flash can ask the board to reboot into BOOTSEL automatically."
        )

    return (
        "No Pico detected.\n"
        "Connect the board by USB, or hold BOOTSEL while plugging it in."
    )

"""Wait for a Pico to appear and flash a .uf2 onto it, then get out of the way.

Used to recover a board whose firmware has wedged its USB: the moment it is
replugged (in BOOTSEL, or briefly reachable during the boot-time console wait)
this grabs it and writes known-good firmware.

Only ever acts on Raspberry Pi USB devices (VID 2E8A).
"""

from __future__ import annotations

import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import flash_pico  # noqa: E402


def wait_and_flash(uf2: Path, timeout: float = 900.0) -> bool:
    print(f"[recover] waiting for a board to flash {uf2.name} onto", flush=True)
    deadline = time.time() + timeout
    tried_touch: set[str] = set()

    while time.time() < deadline:
        drives = flash_pico.find_bootsel_drives()
        if drives:
            drive = Path(drives[0])
            print(f"[recover] BOOTSEL drive at {drive} - flashing", flush=True)
            shutil.copy(uf2, drive / uf2.name)
            print("[recover] done", flush=True)
            return True

        # Not in BOOTSEL: a freshly booted board answers the 1200-baud touch
        # during its console wait, so try each port once as it appears.
        for board in flash_pico.discover(skip_probe=True):
            port = board.port
            if "COM" not in port.upper() and not port.startswith("/dev"):
                continue
            if port in tried_touch:
                continue
            tried_touch.add(port)
            print(f"[recover] {port} appeared - asking it into BOOTSEL", flush=True)
            if flash_pico.reboot_to_bootsel(port):
                break
        time.sleep(0.5)

    print("[recover] gave up waiting", flush=True)
    return False


if __name__ == "__main__":
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    if not target or not target.exists():
        raise SystemExit("usage: recover_pico.py <firmware.uf2>")
    raise SystemExit(0 if wait_and_flash(target) else 1)

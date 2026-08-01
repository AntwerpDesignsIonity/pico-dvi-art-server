"""One-command installer for the Pico W art client.

    python tools/flash_pico.py               # detect, ask, install
    python tools/flash_pico.py --dry-run     # show the plan, touch nothing
    python tools/flash_pico.py --list        # just identify connected boards

What it does:

1. finds every Raspberry Pi USB device (VID 0x2E8A) and works out its state:
   BOOTSEL mass-storage, a MicroPython REPL, or third-party firmware;
2. installs MicroPython from micropython.org when the board is in BOOTSEL;
3. writes `device_secrets.py` from your answers (Wi-Fi, server IP, GitHub repo),
   auto-detecting this PC's LAN address as the default;
4. copies the firmware to the board and verifies every file landed.

Safety: a board running unrecognised firmware is never written to unless you
pass --force. Erasing it would be irreversible.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

RPI_VID = 0x2E8A
REPO_ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_DIR = REPO_ROOT / "pico_firmware"
SECRETS = FIRMWARE_DIR / "device_secrets.py"

# Files copied to the board. device_secrets.py is generated, not templated.
FIRMWARE_FILES = ["boot.py", "main.py", "ota.py", "display_driver.py", "config.py", "version.json"]

UF2_INDEX = "https://micropython.org/download/{board}/"

# The RP2350 bootloader reports a generic Board-ID, so the chip family is all we
# can read from the drive. Both families are matched to their wireless build:
# this firmware needs Wi-Fi, and the wireless builds are supersets for our use.
CHIP_TO_BOARD = {
    "RP2350": "RPI_PICO2_W",
    "RP2040": "RPI_PICO_W",
}
DEFAULT_BOARD = "RPI_PICO_W"

BOOTSEL = "bootsel"
MICROPYTHON = "micropython"
FOREIGN = "foreign"


class Board:
    def __init__(self, port: str, serial: str = "", pid: int = 0):
        self.port = port
        self.serial = serial
        self.pid = pid
        self.state = FOREIGN
        self.details = ""

    def __str__(self) -> str:
        label = {
            BOOTSEL: "BOOTSEL (ready for MicroPython)",
            MICROPYTHON: "MicroPython",
            FOREIGN: "other firmware - DO NOT ERASE",
        }[self.state]
        extra = f" | {self.details}" if self.details else ""
        return f"{self.port:<10} {label}{extra}"


# --------------------------------------------------------------------- shell
def run(cmd: list[str], timeout: float = 60) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def mpremote(*args: str, timeout: float = 60) -> subprocess.CompletedProcess:
    return run([sys.executable, "-m", "mpremote", *args], timeout=timeout)


def mp(port: str, *args: str, timeout: float = 60) -> subprocess.CompletedProcess:
    """Talk to a board, preferring not to reset it.

    Plain `mpremote connect` soft-resets first, which restarts boot.py. Once our
    firmware is installed that boot blocks on Wi-Fi, so the reset locks us out of
    the REPL forever. `resume` skips the reset and interrupts the running program
    instead; we only fall back to the resetting form if that fails.
    """
    result = mpremote("connect", port, "resume", *args, timeout=timeout)
    if result.returncode == 0:
        return result
    return mpremote("connect", port, *args, timeout=timeout)


def ensure_tooling() -> None:
    missing = []
    for module, package in (("serial", "pyserial"), ("mpremote", "mpremote")):
        try:
            __import__(module)
        except ImportError:
            missing.append(package)
    if missing:
        print(f"[setup] installing {', '.join(missing)}...")
        run([sys.executable, "-m", "pip", "install", "-q", *missing], timeout=300)


# ------------------------------------------------------------------ discovery
def find_bootsel_drives() -> list[Path]:
    """A Pico in BOOTSEL mounts a drive containing INFO_UF2.TXT."""
    drives: list[Path] = []
    if sys.platform == "win32":
        candidates = [Path(f"{chr(letter)}:/") for letter in range(ord("D"), ord("Z") + 1)]
    else:
        candidates = []
        for base in ("/media", "/run/media", "/Volumes"):
            root = Path(base)
            if root.is_dir():
                candidates.extend(p for p in root.iterdir() if p.is_dir())
    for drive in candidates:
        try:
            if (drive / "INFO_UF2.TXT").is_file():
                drives.append(drive)
        except OSError:
            continue
    return drives


def probe_port(port: str) -> tuple[str, str]:
    """Return (state, details) for a serial port without modifying the board."""
    result = mp(port, "exec", "import sys;print(sys.implementation.name)", timeout=25)
    if "micropython" in (result.stdout or "").lower():
        info = mp(port, "exec", "import os;print(os.uname().release)", timeout=25)
        return MICROPYTHON, f"MicroPython {info.stdout.strip()}"

    # Not a REPL. Capture whatever it is emitting so the user can recognise it.
    noise = (result.stderr or "") + (result.stdout or "")
    match = re.search(r"b['\"](.{0,160})", noise, re.S)
    banner = (match.group(1) if match else "").replace("\\r\\n", " ").replace("\\n", " ")
    banner = re.sub(r"\s+", " ", banner).strip()
    return FOREIGN, (f"emits: {banner[:110]}" if banner else "no REPL response")


def discover(skip_probe: bool = False) -> list[Board]:
    boards: list[Board] = []

    for drive in find_bootsel_drives():
        board = Board(port=str(drive))
        board.state = BOOTSEL
        board.details = "mass storage"
        boards.append(board)

    try:
        from serial.tools import list_ports
    except ImportError:
        return boards

    for info in list_ports.comports():
        if info.vid != RPI_VID:
            continue
        board = Board(info.device, info.serial_number or "", info.pid or 0)
        if not skip_probe:
            board.state, board.details = probe_port(info.device)
        boards.append(board)
    return boards


# ------------------------------------------------------------- micropython
def board_from_drive(drive: Path) -> str:
    """Map a BOOTSEL drive to the MicroPython board name via INFO_UF2.TXT."""
    try:
        info = (drive / "INFO_UF2.TXT").read_text(errors="replace")
    except OSError:
        return DEFAULT_BOARD
    for chip, name in CHIP_TO_BOARD.items():
        if chip in info:
            return name
    return DEFAULT_BOARD


def latest_uf2_url(board: str = DEFAULT_BOARD) -> str:
    index = UF2_INDEX.format(board=board)
    with urllib.request.urlopen(index, timeout=30) as response:
        html = response.read().decode("utf-8", "replace")
    pattern = re.compile(r'href="(/resources/firmware/' + re.escape(board) + r'-[^"]+\.uf2)"')
    matches = pattern.findall(html)
    if not matches:
        raise RuntimeError(f"could not find a MicroPython .uf2 link for {board}")
    # The page lists newest first. Skip preview builds and the RISC-V variant:
    # the Arm build is the one every published RP2350 example targets.
    stable = [m for m in matches if "preview" not in m and "riscv" not in m.lower()]
    return "https://micropython.org" + (stable or matches)[0]


def reboot_to_bootsel(port: str) -> Path | None:
    """Ask a running board to enter BOOTSEL by touching the port at 1200 baud.

    Most RP2040/RP2350 USB-CDC firmware implements this Arduino-style reset. It
    fails harmlessly on firmware that does not.
    """
    print(f"[bootsel] asking {port} to enter BOOTSEL (1200-baud touch)")
    try:
        import serial

        handle = serial.Serial(port, 1200)
        handle.dtr = False
        time.sleep(0.3)
        handle.close()
    except Exception as exc:
        # The board often yanks the USB device mid-call, which surfaces as an
        # exception even though the reset succeeded. Check for the drive anyway.
        print(f"[bootsel] port closed abruptly ({type(exc).__name__}) - checking for the drive")

    for _ in range(12):
        time.sleep(1)
        drives = find_bootsel_drives()
        if drives:
            print(f"[bootsel] board is now in BOOTSEL at {drives[0]}")
            return drives[0]
    print("[bootsel] the board did not enter BOOTSEL - use the BOOTSEL button")
    return None


def install_micropython(drive: Path, dry_run: bool = False) -> None:
    board = board_from_drive(drive)
    url = latest_uf2_url(board)
    name = url.rsplit("/", 1)[-1]
    print(f"[uf2] board detected as {board}")
    print(f"[uf2] latest firmware: {name}")
    if dry_run:
        print(f"[dry-run] would copy {name} to {drive}")
        return

    cache = REPO_ROOT / ".cache"
    cache.mkdir(exist_ok=True)
    local = cache / name
    if not local.exists():
        print("[uf2] downloading...")
        with urllib.request.urlopen(url, timeout=180) as response, open(local, "wb") as out:
            shutil.copyfileobj(response, out)
    print(f"[uf2] copying to {drive} - the board reboots by itself")
    shutil.copyfile(local, drive / name)
    time.sleep(6)


def wait_for_repl(timeout: float = 45) -> Board | None:
    print("[uf2] waiting for the MicroPython REPL...")
    deadline = time.time() + timeout
    while time.time() < deadline:
        for board in discover():
            if board.state == MICROPYTHON:
                return board
        time.sleep(2)
    return None


# ----------------------------------------------------------------- secrets
def lan_address() -> str:
    try:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect(("8.8.8.8", 80))
        address = probe.getsockname()[0]
        probe.close()
        return address
    except OSError:
        return "192.168.1.10"


def ask(prompt: str, default: str, auto: bool) -> str:
    if auto:
        return default
    reply = input(f"  {prompt} [{default}]: ").strip()
    return reply or default


def refresh_server_ip() -> None:
    """Keep SERVER_IP in an existing device_secrets.py pointing at this PC."""
    if not SECRETS.exists():
        return
    text = SECRETS.read_text(encoding="utf-8")
    address = lan_address()
    updated, count = re.subn(
        r'^SERVER_IP\s*=\s*.+$', f'SERVER_IP = "{address}"', text, count=1, flags=re.M
    )
    if count and updated != text:
        SECRETS.write_text(updated, encoding="utf-8")
        print(f"[secrets] SERVER_IP updated to this PC: {address}")


def write_secrets(dry_run: bool = False, auto: bool = False) -> None:
    if SECRETS.exists():
        print(f"[secrets] {SECRETS.name} already exists - keeping it")
        if not dry_run:
            refresh_server_ip()
        return

    print("\n[secrets] creating device_secrets.py (git-ignored, never uploaded)")
    env = os.environ.get
    ssid = ask("Wi-Fi SSID", env("PICO_WIFI_SSID", ""), auto)
    password = ask("Wi-Fi password", env("PICO_WIFI_PASS", ""), auto)
    server = ask("This PC's LAN IP (the art server)", env("PICO_SERVER_IP", lan_address()), auto)
    port = ask("Server port", env("PICO_SERVER_PORT", "5001"), auto)
    device = ask("Device name", env("PICO_DEVICE_ID", "pico-dvi-01"), auto)
    user = ask("GitHub user", env("PICO_GITHUB_USER", "AntwerpDesignsIonity"), auto)
    repo = ask("GitHub repo", env("PICO_GITHUB_REPO", "pico-dvi-art-server"), auto)

    if auto and not ssid:
        print("[secrets] no Wi-Fi credentials found.")
        print("          Set PICO_WIFI_SSID and PICO_WIFI_PASS, or run")
        print("          'python tools/flash_pico.py' once to enter them.")
        return

    content = f'''# Generated by tools/flash_pico.py - GIT-IGNORED, never uploaded.
# Excluded from the OTA manifest, so updates cannot overwrite it.

WIFI_SSID = {ssid!r}
WIFI_PASS = {password!r}

SERVER_IP = {server!r}
SERVER_PORT = {int(port)}

DEVICE_ID = {device!r}

GITHUB_USER = {user!r}
GITHUB_REPO = {repo!r}
GITHUB_BRANCH = "main"
'''
    if dry_run:
        print("[dry-run] would write device_secrets.py")
        return
    SECRETS.write_text(content, encoding="utf-8")
    print(f"[secrets] wrote {SECRETS}")


# -------------------------------------------------------------------- copy
def copy_firmware(port: str, dry_run: bool = False) -> bool:
    # Order matters. boot.py is written last: the moment it exists the board can
    # reset into firmware that blocks on Wi-Fi, and anything not yet copied would
    # be stranded. device_secrets.py goes first so the board never boots with
    # placeholder credentials.
    files = [SECRETS.name, "version.json", "config.py", "ota.py",
             "display_driver.py", "main.py", "boot.py"]
    files += [f for f in FIRMWARE_FILES if f not in files]

    missing = [f for f in files if not (FIRMWARE_DIR / f).exists()]
    if missing:
        print(f"[copy] missing files: {', '.join(missing)}")
        return False

    print(f"\n[copy] sending {len(files)} files to {port}")
    if dry_run:
        for name in files:
            print(f"  [dry-run] {name} ({(FIRMWARE_DIR / name).stat().st_size} B)")
        return True

    # One invocation, one REPL entry: a per-file loop re-resets the board between
    # files and loses the REPL as soon as boot.py has landed.
    sources = [str(FIRMWARE_DIR / name) for name in files]
    result = mp(port, "fs", "cp", *sources, ":", timeout=180)
    if result.returncode != 0:
        print((result.stderr or "").strip()[-500:])
        return False
    for name in files:
        print(f"  {name:<22} {(FIRMWARE_DIR / name).stat().st_size:>7} B  ok")

    listing = mp(port, "fs", "ls", timeout=45)
    on_board = listing.stdout
    absent = [f for f in files if f not in on_board]
    if absent:
        print(f"[verify] MISSING on board: {', '.join(absent)}")
        return False
    print("[verify] all files present on the board")
    return True


# -------------------------------------------------------------------- main
def choose_target(boards: list[Board], force: bool) -> Board | None:
    usable = [b for b in boards if b.state in (BOOTSEL, MICROPYTHON)]
    if usable:
        # Prefer BOOTSEL: a board deliberately put there is the intended target.
        usable.sort(key=lambda b: 0 if b.state == BOOTSEL else 1)
        return usable[0]

    foreign = [b for b in boards if b.state == FOREIGN]
    if not foreign:
        return None

    print("\n" + "!" * 72)
    print("! Every connected Raspberry Pi board is running NON-MicroPython firmware.")
    print("! Installing MicroPython would ERASE it permanently.")
    for board in foreign:
        print(f"!   {board}")
    print("!")
    print("! If one of these really is the display board, put it in BOOTSEL mode:")
    print("!   unplug it, hold the BOOTSEL button, plug it back in, release.")
    print("! An RPI-RP2 drive appears and this installer will take over safely.")
    print("!" * 72)
    if not force:
        return None

    victim = foreign[0]
    print("\n[force] --force given: erasing the firmware on " + victim.port)
    drive = reboot_to_bootsel(victim.port)
    if drive is None:
        return None
    entered = Board(str(drive))
    entered.state = BOOTSEL
    entered.details = "mass storage"
    return entered


def board_firmware_version(port: str) -> str | None:
    """Read version.json from the board; None if absent/unreadable."""
    result = mp(
        port, "exec",
        "import json,os;print('V=' + (str(json.load(open('version.json'))['version']) "
        "if 'version.json' in os.listdir() else ''))",
        timeout=30,
    )
    if result.returncode != 0:
        return None
    match = re.search(r"V=(\S*)", result.stdout or "")
    if not match:
        return None
    return match.group(1) or None


def local_firmware_version() -> str:
    import json

    try:
        return str(json.loads((FIRMWARE_DIR / "version.json").read_text())["version"])
    except Exception:
        return ""


def already_provisioned(port: str) -> bool:
    listing = mp(port, "fs", "ls", timeout=45).stdout or ""
    if "main.py" not in listing or SECRETS.name not in listing:
        return False
    local = local_firmware_version()
    return bool(local) and board_firmware_version(port) == local


def provision_once(args: argparse.Namespace) -> int:
    print("[scan] looking for Raspberry Pi boards (USB VID 2E8A)...")
    boards = discover()
    if not boards:
        print("[scan] no board connected.")
        print("       Plug the Pico W in with a DATA usb cable, or hold BOOTSEL")
        print("       while connecting to get the RPI-RP2 drive.")
        return 0 if args.auto else 1

    print(f"[scan] {len(boards)} board(s):")
    for board in boards:
        print(f"   {board}")
    if args.list:
        return 0

    if args.port:
        target = Board(args.port)
        target.state, target.details = probe_port(args.port)
    else:
        target = choose_target(boards, args.force)
    if target is None:
        print("\n[skip] no board is ready to be provisioned. Nothing was changed.")
        return 0 if args.auto else 2

    if target.state == MICROPYTHON and not args.dry_run and already_provisioned(target.port):
        print(f"[skip] {target.port} already runs firmware v{local_firmware_version()}")
        return 0

    if target.state == BOOTSEL:
        install_micropython(Path(target.port), args.dry_run)
        if not args.dry_run:
            found = wait_for_repl()
            if found is None:
                print("[stop] the board did not come back as MicroPython.")
                return 0 if args.auto else 3
            target = found
            print(f"[uf2] board is now {target.details} on {target.port}")

    write_secrets(args.dry_run, args.auto)
    if not SECRETS.exists() and not args.dry_run:
        print("[stop] cannot provision without device_secrets.py")
        return 0 if args.auto else 5

    if not copy_firmware(target.port, args.dry_run):
        print("[stop] copy failed - the board may be half-provisioned.")
        return 0 if args.auto else 4

    if not args.dry_run and not args.no_reboot:
        print("[boot] resetting the board")
        mp(target.port, "reset", timeout=30)

    print("\n[done] the Pico will join Wi-Fi, check GitHub for updates,")
    print("       then connect to the art server.")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Install the Pico W art client")
    parser.add_argument("--list", action="store_true", help="identify boards and exit")
    parser.add_argument("--dry-run", action="store_true", help="show the plan, change nothing")
    parser.add_argument("--port", help="use this serial port instead of auto-detecting")
    parser.add_argument("--force", action="store_true", help="allow writing to unrecognised firmware")
    parser.add_argument("--no-reboot", action="store_true", help="do not reset the board at the end")
    parser.add_argument("--auto", action="store_true",
                        help="fully unattended: never prompt, never block, exit 0 when there is "
                             "nothing safe to do")
    parser.add_argument("--watch", type=float, nargs="?", const=30.0, default=None,
                        metavar="SECONDS",
                        help="keep running and provision any board that gets plugged in")
    args = parser.parse_args(argv)

    ensure_tooling()

    if args.watch is None:
        return provision_once(args)

    args.auto = True
    print(f"[watch] rescanning every {args.watch:.0f}s - Ctrl+C to stop")
    while True:
        try:
            provision_once(args)
        except KeyboardInterrupt:
            return 0
        except Exception as exc:  # a transient USB error must not kill the appliance
            print(f"[watch] {type(exc).__name__}: {exc}")
        try:
            time.sleep(args.watch)
        except KeyboardInterrupt:
            return 0


if __name__ == "__main__":
    raise SystemExit(main())

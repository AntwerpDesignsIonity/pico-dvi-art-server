"""Build and optionally flash the local-render C firmware for Pico 2 W.

The board renders the artwork locally and no longer depends on a desktop
preview or control server. This script exists purely to produce a UF2 and copy
it to a connected Pico over the standard BOOTSEL workflow.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

import pico_device

ROOT = Path(__file__).resolve().parent.parent
FIRMWARE_DIR = ROOT / "pico_firmware_c"
BUILD_DIR = FIRMWARE_DIR / "build"
UF2_NAME = "pico_dvi_art_client.uf2"

PICO_HOME = Path(os.environ.get("PICO_HOME", Path.home() / "pico"))
SDK_DIR = Path(os.environ.get("PICO_SDK_PATH", PICO_HOME / "pico-sdk"))
PICODVI_DIR = Path(os.environ.get("PICODVI_PATH", PICO_HOME / "PicoDVI"))

SDK_URL = "https://github.com/raspberrypi/pico-sdk.git"
PICODVI_URL = "https://github.com/Wren6991/PicoDVI.git"


def log(message: str) -> None:
    print(f"[build] {message}", flush=True)


def run(cmd, **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=True, **kwargs)


def find_program(names, extra_dirs=()) -> str | None:
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    for directory in extra_dirs:
        for name in names:
            candidate = Path(directory) / name
            if candidate.exists():
                return str(candidate)
    return None


def glob_first(patterns) -> str | None:
    for pattern in patterns:
        base = Path(pattern).anchor or "/"
        rest = str(Path(pattern).relative_to(base))
        try:
            matches = sorted(Path(base).glob(rest), reverse=True)
        except (OSError, ValueError):
            continue
        if matches:
            return str(matches[0])
    return None


def find_arm_toolchain() -> str | None:
    direct = shutil.which("arm-none-eabi-gcc")
    if direct:
        return str(Path(direct).parent)
    hit = glob_first(
        [
            r"C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\*\bin\arm-none-eabi-gcc.exe",
            r"C:\Program Files\Arm GNU Toolchain arm-none-eabi\*\bin\arm-none-eabi-gcc.exe",
            str(
                Path.home()
                / ".pico-sdk"
                / "toolchain"
                / "*"
                / "bin"
                / "arm-none-eabi-gcc.exe"
            ),
        ]
    )
    return str(Path(hit).parent) if hit else None


def find_cmake() -> str | None:
    return find_program(["cmake", "cmake.exe"], [r"C:\Program Files\CMake\bin"])


def find_ninja() -> str | None:
    return find_program(
        ["ninja", "ninja.exe"],
        [Path(sys.executable).parent / "Scripts", Path(sys.executable).parent],
    )


def find_vcvars() -> str | None:
    if shutil.which("cc") or shutil.which("gcc") or shutil.which("cl"):
        return None
    program_files = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = Path(program_files) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.exists():
        try:
            out = subprocess.run(
                [
                    str(vswhere),
                    "-latest",
                    "-products",
                    "*",
                    "-requires",
                    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                    "-property",
                    "installationPath",
                ],
                capture_output=True,
                text=True,
                check=True,
            ).stdout.strip().splitlines()
            if out:
                candidate = Path(out[0]) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
                if candidate.exists():
                    return str(candidate)
        except (subprocess.CalledProcessError, OSError):
            pass
    return glob_first(
        [
            r"C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat",
            r"C:\Program Files\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat",
        ]
    )


def ensure_sources() -> None:
    if not (SDK_DIR / "pico_sdk_init.cmake").exists():
        log(f"fetching the Pico SDK into {SDK_DIR}")
        SDK_DIR.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "-b", "master", SDK_URL, str(SDK_DIR)])
    if not (SDK_DIR / "lib" / "tinyusb").exists():
        log("initialising SDK submodules (this takes a few minutes)")
        run(
            [
                "git",
                "-C",
                str(SDK_DIR),
                "submodule",
                "update",
                "--init",
                "lib/tinyusb",
            ]
        )
    if not (PICODVI_DIR / "software" / "libdvi").exists():
        log(f"fetching PicoDVI into {PICODVI_DIR}")
        PICODVI_DIR.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--depth", "1", PICODVI_URL, str(PICODVI_DIR)])


def shell_through_vcvars(vcvars: str, command: list[str], cwd: Path) -> int:
    quoted = " ".join(f'"{part}"' if " " in part else part for part in command)
    line = f'call "{vcvars}" >nul 2>&1 && {quoted}'
    return subprocess.call(line, shell=True, cwd=cwd)


def build(clean: bool = False, mode: str = "640x480", invert_diffpairs: int = 1) -> Path:
    ensure_sources()

    cmake = find_cmake()
    if not cmake:
        raise SystemExit("cmake was not found - install it from https://cmake.org/download/")
    ninja = find_ninja()
    if not ninja:
        log("ninja not found, installing it with pip")
        run([sys.executable, "-m", "pip", "install", "--quiet", "ninja"])
        ninja = find_ninja()
    if not ninja:
        raise SystemExit("ninja could not be installed automatically")
    arm_bin = find_arm_toolchain()
    if not arm_bin:
        raise SystemExit(
            "arm-none-eabi-gcc was not found - install the Arm GNU toolchain from\n"
            "https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads"
        )

    env = os.environ.copy()
    env["PICO_SDK_PATH"] = str(SDK_DIR)
    env["PATH"] = os.pathsep.join(
        [arm_bin, str(Path(ninja).parent), str(Path(cmake).parent), env.get("PATH", "")]
    )
    os.environ.update(env)

    if clean and BUILD_DIR.exists():
        log("removing the previous build directory")

        def force_remove(func, path, _exc):
            os.chmod(path, 0o700)
            func(path)

        shutil.rmtree(BUILD_DIR, onerror=force_remove)

    vcvars = find_vcvars()
    if vcvars:
        log(f"host compiler: {vcvars}")
    elif os.name == "nt":
        log("WARNING: no host compiler found - pioasm/picotool may fail to build")

    wide = "ON" if mode == "800x480" else "OFF"
    stamp = BUILD_DIR / "dvi_mode.stamp"
    stamp_value = f"{mode}|invert={invert_diffpairs}"
    if stamp.exists() and stamp.read_text(encoding="utf-8").strip() != stamp_value:
        log(f"panel mode/polarity changed to {stamp_value} - reconfiguring")
        (BUILD_DIR / "build.ninja").unlink(missing_ok=True)

    if not (BUILD_DIR / "build.ninja").exists():
        log(f"configuring for {mode} (invert_diffpairs={invert_diffpairs})")
        configure = [
            cmake,
            "-S",
            str(FIRMWARE_DIR),
            "-B",
            str(BUILD_DIR),
            "-G",
            "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={ninja}",
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DPICODVI_PATH={PICODVI_DIR.as_posix()}",
            f"-DDVI_MODE_800X480={wide}",
            f"-DDVI_INVERT_DIFFPAIRS={invert_diffpairs}",
        ]
        rc = (
            shell_through_vcvars(vcvars, configure, FIRMWARE_DIR)
            if vcvars
            else subprocess.call(configure, cwd=FIRMWARE_DIR)
        )
        if rc != 0:
            raise SystemExit("cmake configure failed")

    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    stamp.write_text(stamp_value, encoding="utf-8")

    log("compiling")
    compile_cmd = [cmake, "--build", str(BUILD_DIR)]
    rc = (
        shell_through_vcvars(vcvars, compile_cmd, FIRMWARE_DIR)
        if vcvars
        else subprocess.call(compile_cmd, cwd=FIRMWARE_DIR)
    )
    if rc != 0:
        raise SystemExit("compilation failed")

    uf2 = BUILD_DIR / UF2_NAME
    if not uf2.exists():
        raise SystemExit(f"the build finished but {uf2} was not produced")
    log(f"built {uf2} ({uf2.stat().st_size // 1024} KiB)")
    return uf2


def flash(uf2: Path, timeout: float = 30.0) -> bool:
    drives = pico_device.find_bootsel_drives()
    if not drives:
        for port in pico_device.discover_raspberry_pi_ports():
            log(f"asking {port} to reboot into BOOTSEL")
            drive = pico_device.reboot_to_bootsel(port)
            if drive is not None:
                drives = [drive]
                break
    if not drives:
        log("no board in BOOTSEL - hold the BOOTSEL button while plugging it in")
        return False

    drive = Path(drives[0])
    log(f"copying {uf2.name} to {drive}")
    shutil.copy(uf2, drive / uf2.name)

    deadline = time.time() + timeout
    while time.time() < deadline:
        if not (drive / "INFO_UF2.TXT").exists():
            log("flashed - the board has rebooted")
            return True
        time.sleep(0.5)
    log("flashed (the drive is still visible, which is harmless)")
    return True


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clean", action="store_true", help="discard the build directory first")
    parser.add_argument("--flash", action="store_true", help="write the result to a connected board")
    parser.add_argument("--no-build", action="store_true", help="flash the existing .uf2 without rebuilding")
    parser.add_argument(
        "--mode",
        default="640x480",
        choices=["640x480", "800x480"],
        help="panel mode: 640x480 (320x240 buffer) or 800x480 (400x240)",
    )
    parser.add_argument(
        "--invert-diffpairs",
        type=int,
        default=1,
        choices=[0, 1],
        help="TMDS diff-pair polarity (1 matches this carrier; try 0 only if the panel never locks)",
    )
    args = parser.parse_args(argv)

    uf2 = (
        BUILD_DIR / UF2_NAME
        if args.no_build
        else build(clean=args.clean, mode=args.mode, invert_diffpairs=args.invert_diffpairs)
    )
    if args.no_build and not uf2.exists():
        raise SystemExit(f"{uf2} does not exist yet - run without --no-build first")
    if args.flash:
        return 0 if flash(uf2) else 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

# pico-dvi-art-server

This repository keeps its historical name, but it is now a **Pico-only firmware workspace**.

It builds and flashes the local-render **C firmware** for a **Raspberry Pi Pico 2 W** driving a **Waveshare PICO-DVI-LCD 10.1**. The Pico renders the generative artwork on-device; the desktop side exists only to build the UF2 and copy it over USB.

## What this repository is about

1. **Local art firmware** for the Pico 2 W and the DVI display carrier.
2. **Desktop tooling** to configure the display mode, build the firmware, and flash it.
3. **No server backend**: no desktop preview, no TCP control plane, and no runtime dependency on a PC once the UF2 has been flashed.

## Quick start

### Source launcher

Double-click **`START.bat`**.

It will:

1. find Python 3.9+,
2. install `pyserial` if needed,
3. open the native **Pico DVI Firmware Studio**.

### Manual build and flash

```powershell
python tools\build_firmware.py --flash --mode 640x480 --invert-diffpairs 1
```

Safe default:

- `640x480`
- `invert-diffpairs 1`

If the board does not reboot into BOOTSEL automatically, reconnect it while holding **BOOTSEL** and run the command again.

## Repository structure

```text
pico-dvi-art-server/
├── START.bat
├── app/
│   ├── studio.py            # native desktop build/flash launcher
│   └── studio_settings.py   # persisted local UI settings
├── pico_firmware_c/
│   ├── CMakeLists.txt
│   ├── main.c               # local-render Pico firmware
│   └── pico_sdk_import.cmake
├── tools/
│   ├── build_firmware.py    # build + flash entry point
│   └── pico_device.py       # BOOTSEL / Raspberry Pi USB detection helpers
├── tests/
│   └── test_studio_settings.py
└── .github/workflows/
    └── ci.yml
```

## Desktop application

`app\studio.py` is a small Tkinter utility for:

1. selecting the DVI mode,
2. selecting the TMDS polarity,
3. building the UF2,
4. flashing the Pico over USB.

It does not host a server and does not stream frames.

## Firmware notes

- **640x480 mode** uses a **320x240 RGB565 framebuffer** that is doubled to the output timing.
- **800x480 mode** uses a **400x240 RGB565 framebuffer**.
- The panel artwork is generated locally in `pico_firmware_c\main.c`.
- USB is the supported delivery path for firmware updates in this repository.

## Toolchain requirements

The build script will look for or fetch:

- **CMake**
- **Ninja**
- **Arm GNU Toolchain (`arm-none-eabi-gcc`)**
- **Pico SDK**
- **PicoDVI**

## Packaging the desktop launcher

If you want a standalone Windows executable, package the current source version yourself:

```powershell
pip install pyinstaller
python -m PyInstaller --onefile --windowed --name "PicoDVIFirmwareStudio" app\studio.py
```

## Summary

This is now a **formal, USB-first Pico DVI firmware repo**:

- the **Pico** is the runtime target,
- the **desktop** is only the build/flash tool,
- the old **server backend has been removed**.

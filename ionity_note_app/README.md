# Ionity Pico Note-Display

Ionity Pico Note-Display is the desktop companion for the Ionity scripture display. It
finds the device on the local network, pushes footer notes over HTTP, and can
stage firmware updates over Wi-Fi on V3+ hardware.

## What this repo contains

| Path | Purpose |
| --- | --- |
| `main.js` | Electron main process: device scan, note push, OTA upload |
| `preload.js` | Safe IPC bridge exposed to the UI |
| `index.html` | Desktop UI for finding the display and sending notes |
| `webapp/` | Static web app for browsers and GitHub Pages |
| `ionity-note.sh` | POSIX shell note-push helper |
| `assets/` | Branding and app icons |

## Features

- Auto-detects the display by hostname first, then scans the local subnet
- Pushes or clears a persistent footer note
- Shows the local PC IPs in the UI footer
- Reads device metadata from `/id` on V3+ firmware
- Uploads firmware images to the OTA endpoint on V3+ firmware
- Ships a static web app for browser access and release downloads

## Run locally

```powershell
npm install
npm start
```

## Build a Windows release

```powershell
npm run dist
```

Artifacts are written to `dist/`:

- `IonityPicoNoteDisplay-4.0.0-portable.exe`
- `IonityPicoNoteDisplay-4.0.0-setup.exe`

## Web app hosting

The `webapp/` folder is designed for GitHub Pages. It provides:

- a link to `http://ionity-scripture.local/`
- an IP entry field for devices without mDNS
- links to the latest GitHub release and source repo

## Device requirements

- V2 firmware: note board only
- V3+ firmware: note board, `/id`, OTA upload, and mDNS

## Next steps

1. Flash the V4 firmware to the Pico once over USB.
2. Open the desktop app and confirm the display is found.
3. Use the web app or desktop app to send a note.
4. Use the OTA upload button for future firmware updates.

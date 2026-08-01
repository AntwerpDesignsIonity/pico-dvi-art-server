# pico-dvi-art-server

Infinite generative art streamed over Wi-Fi to a **Raspberry Pi Pico 2 W**
driving a **Waveshare PICO-DVI-LCD 10.1**. The native desktop application controls
the art, HUD, Wi-Fi provisioning, firmware builds, flashing and OTA.

![preview frame](docs/preview.png)

* **Swirling vortex artwork** that never repeats - irrational frequency ratios plus a
  random per-run seed mean the pattern has no period.
* **Colour-swirling animated frame** around the edge with two counter-rotating comets.
* **HUD, top right:** clock, date underneath, then the **server** temperature
  (weather API or host CPU) and the **actual RP2350 chipset** temperature reported by
  the Pico itself.
* **Retro arcade cycle:** five original colorful faux-3D pixel-art scenes (falling
  blocks, maze chase, tank arena, platform runner and ice climb), with the clock,
  date and temperatures centered and no border.
* **Wi-Fi runtime transport:** live RGB565 artwork is delivered over TCP. USB is
  reserved for explicit firmware flashing and diagnostics.
* **Push OTA control:** builds the selected C firmware, releases USB cleanly,
  enters BOOTSEL and copies the UF2. USB flashing is verified; network-only OTA
  requires an established Pico TCP session.
* Optional **AI image mode** (rotating prompt template -> image API -> 400x240 RGB565).

---

## Current verified status (2026-08-01)

### Verified

* The Pico 2 W enumerates as **COM5**, USB VID `2E8A`. The attached ESP device
  uses VID `303A` and is never selected by the flashing tools.
* The safe firmware builds and flashes automatically:

  ```powershell
  python tools\build_firmware.py --mode 640x480 --invert-diffpairs 1 --clean --flash
  ```

* The firmware uses **640x480p60 DVI** with a **320x240 RGB565 framebuffer**
  doubled horizontally and vertically. A frame payload is **153,600 bytes**.
* The standalone [PicoDVIArtStudio.exe](PicoDVIArtStudio.exe) starts the native
  GUI and TCP server. Because it is a PyInstaller one-file application, first
  launch can take roughly 20-30 seconds while it extracts; the launcher and
  application child appearing as two processes is normal. Current SHA-256:
  `D6C7DC3A9D555E086A98641225135ECE0936B2C3498E7089749D1BFF5D0462C2`.
* The server has been verified listening on `192.168.124.4:5001`.
* Windows Firewall has exactly one enabled inbound rule named
  **Pico DVI Art Server**, allowing TCP port `5001` on all profiles. The app
  checks this rule whenever the server starts and requests one UAC approval if
  it must create it.
* Wi-Fi credentials are editable in the GUI's **Network** tab. Saving writes
  only to the ignored local credentials file. **Build + flash firmware** is
  required before changed credentials are present on the Pico.
* `temp_source = weather` reads current outdoor temperature from Open-Meteo
  without an API key. Clock/date come from the PC. The MCU temperature appears
  only after a connected Pico sends `TEMP` telemetry.
* All **80 automated tests** pass.

### Still requiring physical verification

* A sustained Pico-to-server TCP frame session was not observed in the final
  test window. The Pico has alternated between successful association
  (`192.168.124.7`) and AP refusal/backoff. An ARP entry proves the AP learned
  the Pico, but failed ping alone does **not** prove client isolation because
  the Pico firmware does not provide an ICMP/TCP test service.
* The panel was reported as solid red. The documented Waveshare/PicoDVI wiring
  uses `invert_diffpairs=1`; a test with `0` produced no useful firmware log and
  was reverted. The current board therefore runs the documented
  `invert_diffpairs=1` build. Confirm the panel shows the checkered
  **PICO DVI ART** standby screen; if it remains solid red, inspect the DVI
  carrier seating, cable/panel input, and power before changing networking.

---

## Run it — one file, no questions

Double-click **`PicoDVIArtStudio.exe`** for the packaged application, or
**`START.bat`** to run the source version through an installed Python.

`START.bat` finds Python, installs missing dependencies the first time, then
opens the native **Pico DVI Art Studio**. The packaged EXE already contains the
Python runtime and application dependencies. In both cases the server runs
inside the application; there is no browser or separate console server.

```
PicoDVIArtStudio.exe
  └── native GUI + art server + provisioning + build/flash/OTA controls

START.bat
  ├── finds Python 3.9+ (PATH, py launcher, or %LOCALAPPDATA%\Programs\Python)
  ├── pip install -r pc_server/requirements.txt mpremote pyserial   (first run only)
  └── app/studio.py                 ← same application, run from source
```

Select `retro`, `shader`, `ai` or `hybrid` under **Art source**. The **Network** tab
contains editable Wi-Fi SSID/password fields. Credentials are stored only in
`pico_firmware/device_secrets.py`, which is git-ignored and never uploaded. Use
**Build + flash firmware** after changing Wi-Fi settings. Only USB VID `2E8A`
(Raspberry Pi) is ever selected; attached ESP boards are ignored.

Advanced use, if you ever want it:

```
python tools/flash_pico.py --list      # identify every connected board, change nothing
python tools/flash_pico.py             # interactive setup, prompts for credentials
python tools/flash_pico.py --dry-run   # show the plan
```

### Building a standalone `.exe`

`START.bat` needs Python installed. If you'd rather hand someone a single
double-clickable file with no Python required, package the same app with
PyInstaller:

```
pip install pyinstaller
python -m PyInstaller --onefile --windowed --name "PicoDVIArtStudio" ^
    --paths pc_server --distpath . app\studio.py
```

This produces `PicoDVIArtStudio.exe` at the repo root. The current release
executable is committed to the repository so it can be downloaded and run
directly. It must stay in this folder (next to `pico_firmware_c/`, `pico_firmware/`,
`pc_server/`, `tools/`) since it uses the same relative paths as
`app/studio.py`. Building/flashing firmware from the `.exe` still needs a
system Python with CMake/Ninja/the Pico SDK installed; the streaming/HUD/OTA
GUI itself does not.

---

## Architecture

```
[ PC / local server ]  pc_server/server.py
   |  1. InfiniteArtEngine        - swirling plasma, no repeat period
   |  2. SwirlBorder              - animated colour frame
   |  3. HUD                      - clock / date / OUT temp / MCU temp
   |  4. RGB565 pack (153,600 B at 320x240)
   +--> TCP :5001 --- Wi-Fi ---> [ Pico 2 W ]  pico_firmware_c/main.c
                                     |  blits bytes into the DVI framebuffer
                                     |  sends TEMP / STAT / HELLO back up
                                     v
                            [ Pico DVI LCD 10.1 ]

[ Desktop app ] --BOOTSEL + UF2--> [ Pico 2 W C firmware ] --reboot
```

### Wire protocol

Server to Pico, little-endian:

| Packet  | Bytes                                                         |
| ------- | ------------------------------------------------------------- |
| Frame   | `AA BB CC DD` + `uint32 length` + `length` bytes RGB565        |
| Command | `AA BB CC EE` + `uint32 length` + UTF-8 JSON `{"cmd": "ota"}`  |

Pico to server, newline-terminated ASCII:

```
HELLO <fw_version> <device_id>
TEMP <celsius>            # RP2350 on-chip sensor -> the "MCU" HUD line
STAT fps=<f> drops=<n>
```

---

## Cycle times — what changes, and how fast

Nothing in the visual layer ever repeats: every animated term uses a mutually
irrational frequency (φ, √2, √3, √5, √7), so the combined signal has **no finite
period**. The numbers below are the *perceived* rhythms, not loop lengths.

| Layer | Cycle | Where to change it |
| --- | --- | --- |
| Frame refresh | **20 fps** (50 ms) | `fps` / `--fps` (server renders up to ~41 fps) |
| Vortex swirl rotation | ~18 s per turn | `t * 0.35` in `math_shaders.render` |
| Full hue wheel (whole artwork) | **~28 s** | `t * 0.021 * PHI` |
| Plasma band motion | 7–12 s | `f1`…`f4` frequencies |
| Swirl strength breathing | ~57 s | `_drift(t, 0, 0.11)` |
| Zoom breathing | ~90 s | `_drift(t, 1, 0.07)` |
| Border hue wheel | ~8.3 s | `t * 0.12` in `SwirlBorder.apply` |
| Border comet lap | ~4.5 s / ~7.7 s (counter-rotating) | `t * 0.22`, `t * 0.13` |
| Clock digits | 1 s | `show_seconds` |
| MCU temperature uplink | **5 s** | `TELEMETRY_INTERVAL_S` |
| Server temperature refresh | **10 min** | `temp_refresh_s` |
| New AI image (when enabled) | **90 s**, 3 s cross-fade | `ai_interval_s`, `ai_fade_s` |
| OTA check | **on boot + hourly** | `OTA_ON_BOOT`, `OTA_CHECK_MINUTES` |
| OTA on demand | ~2 s to reach the board (+ up to 5 min GitHub CDN lag) | `python pc_server/push_ota.py` |
| Reconnect after server loss | 3 s, infinite retry | `RECONNECT_DELAY_S` |

Because the drift terms are incommensurable, the *combination* of swirl angle, zoom,
hue and plasma phase only approximately recurs on a timescale of years — and the
random per-boot seed shifts all of them anyway, so restarting the server gives you a
genuinely different piece.

Want it calmer or wilder? `--speed 0.4` slows every visual cycle by 2.5x,
`--speed 2.0` doubles them; the clock and temperatures are unaffected.

---

## Repository layout

```
pico-dvi-art-server/
├── PicoDVIArtStudio.exe     # packaged native application
├── START.bat                # source launcher (requires Python)
├── app/
│   └── studio.py            # GUI + server owner + provisioning/flash/OTA controls
├── .github/workflows/
│   ├── release.yml          # auto-increments pico_firmware/version.json on push
│   └── ci.yml               # tests + firmware byte-compile
├── pico_firmware/           # flashed onto the Pico W
│   ├── boot.py              # rollback check -> Wi-Fi -> GitHub OTA
│   ├── ota.py               # GitHub updater with staging + rollback
│   ├── main.py              # stream receiver, telemetry, offline fallback
│   ├── display_driver.py    # panel backends + offline animation
│   ├── config.py            # tuning values (safe to publish)
│   ├── device_secrets.example.py  # template -> copy to device_secrets.py
│   └── version.json         # firmware version + file manifest
├── pc_server/               # runs on your PC / home server
│   ├── server.py            # TCP server + stream orchestrator
│   ├── math_shaders.py      # infinite swirl engine + swirling border
│   ├── hud.py               # clock / date / temperature overlay
│   ├── font5x7.py           # bitmap font
│   ├── temperature.py       # weather / CPU / static temperature source
│   ├── pixels.py            # RGB888 <-> RGB565 + framing
│   ├── ai_prompts.py        # rotating AI prompt template + image fetcher
│   ├── preview.py           # render PNGs without hardware
│   ├── fake_pico.py         # virtual Pico W for end-to-end testing
│   ├── push_ota.py          # tell every connected board to update now
│   ├── config.example.json  # copy to config.json and edit
│   └── requirements.txt
├── tools/
│   └── flash_pico.py        # USB detection + MicroPython install + provisioning
└── tests/
    ├── test_pipeline.py      # font, packing, shaders, HUD, socket round-trip
    └── test_ota.py           # OTA staging / rollback against a stubbed GitHub
```

---

## 1. Run the server

`START.bat` already does all of this. The commands below are the manual equivalent.

```powershell
pip install -r pc_server/requirements.txt

# check the artwork without any hardware -> preview/frame_XXX.png
python pc_server/preview.py --frames 6

# start streaming
python pc_server/server.py
```

The server prints its LAN addresses on start - put that IP into
`pico_firmware/config.py` as `SERVER_IP`.

Useful flags: `--fps 25`, `--source hybrid`, `--speed 0.6`, `--border 12`,
`--byte-order big`, `--temp-source cpu`, `--no-hud`, `--self-test 60`.

Configuration precedence: defaults -> `pc_server/config.json`
(copy `config.example.json`) -> `PICOART_*` environment variables -> CLI flags.

```powershell
copy pc_server\config.example.json pc_server\config.json
$env:PICOART_FPS = "25"        # env override example
```

### Verify without hardware

```powershell
python pc_server/server.py --port 5099          # terminal 1
python pc_server/fake_pico.py --port 5099 --frames 60 --save shot.png   # terminal 2
```

`fake_pico.py` speaks the real protocol, reports a fake MCU temperature and writes the
frame it received to PNG - what you see there is exactly what the panel shows.

---

## 2. Flash the Pico 2 W (automatic)

Use **Build + flash firmware** in the GUI. It saves the settings, builds the
safe C/PicoDVI firmware, asks only Raspberry Pi VID `2E8A` to enter BOOTSEL,
copies the UF2, and waits for the board to reboot.

The equivalent command is:

```powershell
python tools\build_firmware.py --mode 640x480 --invert-diffpairs 1 --clean --flash
```

The board shows a local checkered standby/status screen while Wi-Fi or the
server is unavailable and keeps retrying. USB remains for explicit
installation/recovery only; live art frames use Wi-Fi.

### Display backends

`display_driver.py` probes, in order: a `picodvi.DVI` MicroPython module, a
CircuitPython `picodvi.Framebuffer` + `framebufferio` combination, and finally a
headless stub so you can validate the network path before the panel library is
sorted. To use a different library, add a class exposing `buffer`, `show()` and
`name` to `_BACKENDS`.

If red and blue look swapped, flip the byte order on the server:
`python pc_server/server.py --byte-order big`.

---

## 3. OTA updates over GitHub

1. Push this repo to GitHub (public, or the raw URLs will 404 — which is exactly why
   `device_secrets.py` is git-ignored).
2. Set `GITHUB_USER` / `GITHUB_REPO` / `GITHUB_BRANCH` in
   `pico_firmware/device_secrets.py`.
3. Edit any firmware file and push. `release.yml` bumps `version.json` and rewrites its
   file manifest automatically.
4. The board updates on its next boot, on its scheduled check
   (`OTA_CHECK_MINUTES`, default hourly), or immediately when you run:

```powershell
python pc_server/push_ota.py                  # {"cmd": "ota"}
python pc_server/push_ota.py --reboot         # {"cmd": "reboot"}
```

The running server picks the trigger file up within 2 seconds and broadcasts the
command to every connected board.

**Propagation delay.** `raw.githubusercontent.com` is fronted by a CDN with a hard
`max-age=300`, so a freshly pushed commit takes **up to 5 minutes** to become visible
to the board — no client-side header can bypass it. If you push and immediately run
`push_ota.py`, the device may still see the old version; it will pick the new one up
on its next hourly check, or just wait five minutes and trigger again.

**Safety net.** Files are downloaded to `<name>.new` and validated (non-empty, not a
GitHub 404 page) before anything is replaced. The version is then re-checked, so a
publish landing mid-download can never mix old and new files. The old copies are kept
as `<name>.bak` and an `ota.pending` marker is written. `main.py` calls
`ota.mark_boot_ok()` after 20 successfully streamed frames, which clears the marker.
If the board never gets that far, the next boot restores the backups, marks the bad
version as `blocked` and refuses to install it again — push a higher version to
recover.

`config.py` and `device_secrets.py` are deliberately excluded from the manifest, so
OTA never overwrites your Wi-Fi credentials.

---

## 4. AI image mode (optional)

`ai_prompts.py` holds the master prompt template plus the rotating variable lists
(subject / style / palette / seed), so no two generations are alike:

```powershell
python pc_server/ai_prompts.py      # print a few sample prompts
```

To stream generated images instead of (or blended with) the shader:

```powershell
pip install pillow
$env:OPENAI_API_KEY = "sk-..."
python pc_server/server.py --source ai        # or: --source hybrid
```

`source=ai` cross-fades to each new image, `source=hybrid` keeps the swirl engine
breathing over it. Generation happens on a background thread - a failed or slow API
call never stalls the stream, the shader simply keeps running. Set
`"ai_provider": "folder"` in `config.json` to cycle through local images in
`art_cache/` instead of calling an API.

**Never commit API keys** - they are read from the environment variable named by
`ai_api_key_env`.

---

## 5. Temperatures

| HUD line | Source                                                                        |
| -------- | ----------------------------------------------------------------------------- |
| `OUT`    | server side: Open-Meteo current temperature (default), host CPU, or a constant |
| `MCU`    | the Pico's own RP2350 on-chip sensor (ADC4), sent up the stream every 5 s      |

```json
{ "temp_source": "weather", "latitude": 51.2194, "longitude": 4.4025 }
```

`temp_source` accepts `weather`, `cpu` (needs `pip install psutil`), `static` or
`none`. Labels are configurable via `temp_label_server` / `temp_label_local`.
`--` is shown until the first reading arrives, so a dead network never blanks the HUD.

---

## Tests

```powershell
python -m unittest discover -s tests -v
python pc_server/server.py --self-test 60
```

The current suite contains **80 tests** covering font metrics, RGB565 packing,
shader and retro-scene behavior, border/HUD placement, configuration, prompt
rotation, socket framing/telemetry, and OTA staging/rollback failure cases.

---

## Performance notes

* Rendering costs ~25 ms/frame at 400x240 on a modern desktop CPU (~40 fps headroom);
  the border precomputes its geometry once and only touches edge pixels.
* The safe 320x240 RGB565 mode is 153,600 bytes/frame. At 20 fps that is
  ~24.6 Mbit/s before TCP/Wi-Fi overhead. The Pico W's CYW43439 is the real
  limit; lower `fps` if the client stalls.
* TCP back-pressure paces the stream automatically: a slow board simply receives
  fewer frames instead of desynchronising.

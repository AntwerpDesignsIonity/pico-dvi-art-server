"""Wi-Fi + GitHub OTA with automatic rollback (MicroPython, Pico W).

Update flow
-----------
1. fetch `version.json` from the GitHub raw URL in config.py
2. if the remote version is higher, download every file of the manifest to
   `<name>.new` and sanity-check it (non-empty, not an HTML error page)
3. back up the current copies to `<name>.bak`, swap the new files in,
   drop an `ota.pending` marker and reset
4. `main.py` calls `mark_boot_ok()` once it is streaming; if that never
   happens the next boot restores the `.bak` files automatically

config.py is never touched by an update - device credentials stay local.
"""

import machine
import os
import time

try:
    import ujson as json
except ImportError:  # CPython during desktop linting
    import json

import config

VERSION_FILE = "version.json"
PENDING_FLAG = "ota.pending"
CHUNK = 512


# ---------------------------------------------------------------- filesystem
def exists(path):
    try:
        os.stat(path)
        return True
    except OSError:
        return False


def remove(path):
    try:
        os.remove(path)
    except OSError:
        pass


def copy_file(src, dst):
    remove(dst)
    with open(src, "rb") as fin, open(dst, "wb") as fout:
        while True:
            block = fin.read(CHUNK)
            if not block:
                break
            fout.write(block)


def replace(src, dst):
    """os.rename cannot overwrite on littlefs, so unlink the target first."""
    remove(dst)
    os.rename(src, dst)


# --------------------------------------------------------------------- wifi
def connect_wifi(verbose=True):
    import network

    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    try:
        if getattr(config, "WIFI_COUNTRY", ""):
            network.country(config.WIFI_COUNTRY)
    except Exception:
        pass
    if wlan.isconnected():
        return wlan

    if verbose:
        print("[wifi] connecting to", config.WIFI_SSID)
    wlan.connect(config.WIFI_SSID, config.WIFI_PASS)
    deadline = time.time() + config.WIFI_TIMEOUT_S
    while not wlan.isconnected() and time.time() < deadline:
        time.sleep(0.5)
    if wlan.isconnected():
        if verbose:
            print("[wifi] connected, ip =", wlan.ifconfig()[0])
    else:
        print("[wifi] FAILED to connect")
    return wlan


# ------------------------------------------------------------------ version
def local_version():
    try:
        with open(VERSION_FILE) as handle:
            return int(json.load(handle).get("version", 0))
    except (OSError, ValueError):
        return 0


def blocked_version():
    """Version that previously failed to boot; never re-installed."""
    try:
        with open(VERSION_FILE) as handle:
            return int(json.load(handle).get("blocked", -1))
    except (OSError, ValueError):
        return -1


def local_manifest():
    try:
        with open(VERSION_FILE) as handle:
            files = json.load(handle).get("files")
            if isinstance(files, list) and files:
                return files
    except (OSError, ValueError):
        pass
    return ["main.py", "ota.py", "display_driver.py", "boot.py"]


def _get(url, timeout=15):
    import urequests

    # raw.githubusercontent.com is served by Fastly with a hard max-age=300 that
    # a client cannot bypass, so a fresh push takes up to 5 minutes to become
    # visible here. The cache-buster and no-cache headers only help on the edges
    # of that window; the real safety net is the version re-check below.
    separator = "&" if "?" in url else "?"
    return urequests.get(
        url + separator + "t=" + str(time.time()),
        timeout=timeout,
        headers={"Cache-Control": "no-cache", "Pragma": "no-cache"},
    )


def _looks_like_code(text):
    if not text or len(text) < 20:
        return False
    head = text[:200].lstrip().lower()
    return not (head.startswith("<!doctype") or head.startswith("<html") or head == "404: not found")


# ---------------------------------------------------------------------- OTA
def check_and_update(verbose=True):
    """Returns True when an update was flashed (the board resets right after)."""
    try:
        response = _get(config.RAW_BASE + VERSION_FILE)
        remote = response.json()
        response.close()
    except Exception as exc:
        print("[ota] version check failed:", exc)
        return False

    remote_version = int(remote.get("version", 0))
    current = local_version()
    files = remote.get("files") or local_manifest()
    if verbose:
        print("[ota] local v%d | github v%d" % (current, remote_version))
    if remote_version <= current:
        return False
    if remote_version == blocked_version():
        print("[ota] v%d is blocked (it failed to boot) - push a higher version" % remote_version)
        return False

    print("[ota] updating to v%d: %s" % (remote_version, ", ".join(files)))
    staged = []
    try:
        for name in files:
            # Never overwrite device-local credentials or settings.
            if name in ("config.py", "device_secrets.py"):
                continue
            response = _get(config.RAW_BASE + name)
            text = response.text
            response.close()
            if not _looks_like_code(text):
                raise OSError("bad download for " + name)
            with open(name + ".new", "w") as handle:
                handle.write(text)
            staged.append(name)
            print("[ota]   staged", name, len(text), "bytes")
    except Exception as exc:
        print("[ota] download aborted:", exc)
        for name in staged:
            remove(name + ".new")
        return False

    # The CDN serves each file with its own TTL, so a publish landing mid-download
    # could mix old and new code. Re-check the version before committing anything.
    try:
        response = _get(config.RAW_BASE + VERSION_FILE)
        confirmed = int(response.json().get("version", 0))
        response.close()
    except Exception as exc:
        print("[ota] re-check failed:", exc)
        confirmed = remote_version  # network died after a clean download; proceed
    if confirmed != remote_version:
        print("[ota] version moved %d -> %d mid-download, retrying later" % (remote_version, confirmed))
        for name in staged:
            remove(name + ".new")
        return False

    for name in staged:
        if exists(name):
            copy_file(name, name + ".bak")
    for name in staged:
        replace(name + ".new", name)

    with open(VERSION_FILE, "w") as handle:
        json.dump({"version": remote_version, "files": files}, handle)
    with open(PENDING_FLAG, "w") as handle:
        handle.write(str(remote_version))

    print("[ota] flashed v%d - resetting" % remote_version)
    time.sleep(1)
    machine.reset()
    return True


def mark_boot_ok():
    """Called by main.py once streaming works: keeps the new firmware."""
    if not exists(PENDING_FLAG):
        return
    remove(PENDING_FLAG)
    for name in local_manifest():
        remove(name + ".bak")
    print("[ota] update confirmed healthy")


def rollback_if_needed():
    """Restore the previous firmware if the last update never confirmed."""
    if not exists(PENDING_FLAG):
        return False
    print("[ota] previous update did not confirm - rolling back")
    try:
        with open(PENDING_FLAG) as handle:
            failed_version = int(handle.read().strip() or "0")
    except (OSError, ValueError):
        failed_version = local_version()

    restored = 0
    for name in local_manifest():
        backup = name + ".bak"
        if exists(backup):
            replace(backup, name)
            restored += 1
    remove(PENDING_FLAG)
    try:
        with open(VERSION_FILE) as handle:
            data = json.load(handle)
        data["version"] = max(0, failed_version - 1)
        data["blocked"] = failed_version
        with open(VERSION_FILE, "w") as handle:
            json.dump(data, handle)
    except Exception:
        pass
    print("[ota] restored %d file(s); v%d blocked" % (restored, failed_version))
    if restored:
        time.sleep(1)
        machine.reset()
    return True

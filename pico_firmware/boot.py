"""Runs before main.py on every power-up.

1. roll back if the previous OTA update never confirmed itself
2. join Wi-Fi
3. ask GitHub whether newer firmware exists and flash it (resets on success)

Keep this file small: if it raises, the board never reaches main.py.
"""

import time

try:
    import config
    import ota
except Exception as exc:  # pragma: no cover - corrupted flash
    print("[boot] configuration import failed:", exc)
    config = None
    ota = None

if ota is not None:
    try:
        ota.rollback_if_needed()
    except Exception as exc:
        print("[boot] rollback failed:", exc)

    if getattr(config, "OTA_ON_BOOT", True):
        try:
            wlan = ota.connect_wifi()
            if wlan.isconnected():
                ota.check_and_update()
            else:
                print("[boot] no Wi-Fi - skipping OTA, starting display anyway")
        except Exception as exc:
            print("[boot] OTA check failed:", exc)

    print("[boot] handing over to main.py")
    time.sleep(0.2)

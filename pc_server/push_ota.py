"""Order every connected Pico W to run its GitHub OTA check right now.

    python pc_server/push_ota.py            # {"cmd": "ota"}
    python pc_server/push_ota.py --reboot   # {"cmd": "reboot"}

Writes a trigger file that the running server picks up within 2 seconds and
broadcasts to all streaming clients as a control packet.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

TRIGGER = Path(__file__).with_name("ota.trigger")


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description="Broadcast an OTA/reboot command")
    parser.add_argument("--reboot", action="store_true", help="hard reset instead of OTA check")
    parser.add_argument("--message", default="", help="optional text logged by the Pico")
    args = parser.parse_args(argv)

    command = {"cmd": "reboot" if args.reboot else "ota"}
    if args.message:
        command["message"] = args.message
    TRIGGER.write_text(json.dumps(command), encoding="utf-8")
    print(f"[+] queued {command} via {TRIGGER}")


if __name__ == "__main__":
    main()

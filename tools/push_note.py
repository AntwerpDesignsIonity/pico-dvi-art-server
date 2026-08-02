"""Push a note to the scripture display's footer from any PC.

The firmware (built with --wifi-ssid/--wifi-pass) serves a note page on
port 80. This is a thin command-line client for it:

    py -3 tools\\push_note.py 192.168.1.42 "DINNER AT SEVEN"
    py -3 tools\\push_note.py 192.168.1.42 --clear

Any phone or browser can do the same by just opening http://<board-ip>/.
"""

from __future__ import annotations

import argparse
import sys
import urllib.parse
import urllib.request


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="IP address of the board (shown in its footer)")
    parser.add_argument("note", nargs="?", default="", help="the note text (omit with --clear)")
    parser.add_argument("--clear", action="store_true", help="clear the current note")
    args = parser.parse_args(argv)

    text = "" if args.clear else args.note
    if not text and not args.clear:
        parser.error("provide a note or pass --clear")

    url = f"http://{args.host}/note?t={urllib.parse.quote_plus(text)}"
    try:
        with urllib.request.urlopen(url, timeout=10) as response:
            response.read()
    except OSError as exc:
        print(f"could not reach {args.host}: {exc}", file=sys.stderr)
        return 1

    print("note cleared" if args.clear else f"note pushed: {text.upper()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

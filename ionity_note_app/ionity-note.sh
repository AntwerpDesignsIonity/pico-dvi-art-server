#!/bin/sh
# Ionity Note - push a note to the Ionity scripture display from any
# Linux/macOS shell. The display serves its note board on port 80.
#
# Usage:
#   ./ionity-note.sh <display-ip> "YOUR NOTE"
#   ./ionity-note.sh <display-ip> --clear
#
# (c) Ionity Global Pty Ltd. MIT licensed.

set -eu

if [ "$#" -lt 2 ]; then
    echo "usage: $0 <display-ip> \"NOTE TEXT\" | --clear" >&2
    exit 2
fi

HOST="$1"
shift

if [ "$1" = "--clear" ]; then
    NOTE=""
else
    NOTE="$*"
fi

# URL-encode the note (portable awk, no bashisms).
ENCODED=$(printf '%s' "$NOTE" | awk '
BEGIN { for (i = 0; i < 256; i++) ord[sprintf("%c", i)] = i }
{
    if (NR > 1) printf "%%0A"
    n = split($0, ch, "")
    for (i = 1; i <= n; i++) {
        c = ch[i]
        if (c ~ /[A-Za-z0-9._~-]/) printf "%s", c
        else if (c == " ") printf "+"
        else printf "%%%02X", ord[c]
    }
}')

if curl -fsS --max-time 10 "http://${HOST}/note?t=${ENCODED}" > /dev/null; then
    if [ -z "$NOTE" ]; then
        echo "note cleared"
    else
        echo "note pushed: $(printf '%s' "$NOTE" | tr '[:lower:]' '[:upper:]')"
    fi
else
    echo "could not reach ${HOST}" >&2
    exit 1
fi

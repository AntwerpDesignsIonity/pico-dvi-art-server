"""Virtual Pico W: verify the whole pipeline without any hardware.

    python pc_server/server.py                 # terminal 1
    python pc_server/fake_pico.py --frames 40  # terminal 2

It speaks the exact wire protocol the firmware speaks (frame + command
packets down, HELLO/TEMP/STAT up) and can dump what it received to PNG.
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from pixels import HEADER_SIZE, MAGIC, from_rgb565  # noqa: E402
from preview import write_png  # noqa: E402

COMMAND_MAGIC = b"\xAA\xBB\xCC\xEE"


def recv_exactly(sock: socket.socket, length: int) -> bytes:
    chunks = []
    remaining = length
    while remaining:
        block = sock.recv(min(65536, remaining))
        if not block:
            raise ConnectionError("stream closed")
        chunks.append(block)
        remaining -= len(block)
    return b"".join(chunks)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Virtual Pico W client")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5001)
    parser.add_argument("--frames", type=int, default=20)
    parser.add_argument("--width", type=int, default=400)
    parser.add_argument("--height", type=int, default=240)
    parser.add_argument("--byte-order", default="little", choices=["little", "big"])
    parser.add_argument("--temp", type=float, default=42.3, help="fake MCU temperature")
    parser.add_argument("--save", default="", help="write the last frame to this PNG")
    args = parser.parse_args(argv)

    expected = args.width * args.height * 2
    sock = socket.create_connection((args.host, args.port), timeout=15)
    sock.sendall(b"HELLO 1 fake-pico\n")
    sock.sendall(f"TEMP {args.temp:.2f}\n".encode())

    received = 0
    commands = 0
    last_payload = b""
    started = time.perf_counter()
    try:
        while received < args.frames:
            header = recv_exactly(sock, HEADER_SIZE)
            magic, length = header[:4], struct.unpack("<I", header[4:8])[0]
            if magic == MAGIC:
                if length != expected:
                    raise ValueError(f"unexpected frame size {length} (want {expected})")
                last_payload = recv_exactly(sock, length)
                received += 1
                if received % 10 == 0:
                    sock.sendall(f"TEMP {args.temp + received * 0.01:.2f}\n".encode())
                    sock.sendall(f"STAT fps={received / (time.perf_counter() - started):.1f} drops=0\n".encode())
            elif magic == COMMAND_MAGIC:
                payload = recv_exactly(sock, length)
                commands += 1
                print(f"[fake-pico] command: {json.loads(payload)}")
            else:
                raise ValueError(f"bad magic {magic!r}")
    finally:
        sock.close()

    elapsed = time.perf_counter() - started
    print(
        f"[fake-pico] {received} frames in {elapsed:.2f}s "
        f"({received / elapsed:.1f} fps, {received * expected / elapsed / 1e6:.2f} MB/s), "
        f"{commands} command(s)"
    )
    if args.save and last_payload:
        rgb = from_rgb565(last_payload, args.width, args.height, args.byte_order)
        write_png(Path(args.save), rgb)
        print(f"[fake-pico] last frame written to {args.save}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

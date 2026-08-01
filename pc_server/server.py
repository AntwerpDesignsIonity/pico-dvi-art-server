"""TCP frame server: infinite generative art + HUD, streamed as RGB565.

    python pc_server/server.py            # stream on 0.0.0.0:5001
    python pc_server/server.py --fps 25 --source hybrid
    python pc_server/server.py --self-test # render locally, no hardware needed

Wire protocol (server -> Pico), little-endian:

    frame  : b'\\xAA\\xBB\\xCC\\xDD' | uint32 length | length bytes RGB565
    command: b'\\xAA\\xBB\\xCC\\xEE' | uint32 length | length bytes UTF-8 JSON

Uplink (Pico -> server) is newline-terminated ASCII:

    HELLO <fw_version> <device_id>
    TEMP <celsius>          # RP2040 on-chip sensor -> shown as "MCU" on the HUD
    STAT fps=<f> drops=<n>
    PONG
"""

from __future__ import annotations

import argparse
import json
import select
import socket
import struct
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

import math_shaders  # noqa: E402
from ai_prompts import AiFrameSource  # noqa: E402
from config import Config  # noqa: E402
from hud import HudState, local_now, render_centered_hud, render_hud  # noqa: E402
from pixels import HEADER_SIZE, MAGIC, to_rgb565  # noqa: E402
from temperature import TemperatureProvider  # noqa: E402

MAGIC_COMMAND = b"\xAA\xBB\xCC\xEE"
OTA_TRIGGER = Path(__file__).with_name("ota.trigger")


@dataclass
class ClientSession:
    """Per-connection state fed by the uplink reader thread."""

    address: tuple[str, int]
    local_temp_c: float | None = None
    fw_version: str = "?"
    device_id: str = ""
    client_fps: float | None = None
    last_seen: float = field(default_factory=time.time)
    lock: threading.Lock = field(default_factory=threading.Lock)
    # Frames and OTA commands are written from different threads; without this
    # a command could be spliced into the middle of a frame payload.
    send_lock: threading.Lock = field(default_factory=threading.Lock)

    def snapshot(self) -> tuple[float | None, str]:
        with self.lock:
            return self.local_temp_c, self.fw_version


class ArtServer:
    def __init__(self, cfg: Config) -> None:
        self.cfg = cfg
        self.engine = math_shaders.InfiniteArtEngine(
            cfg.width, cfg.height, seed=cfg.seed, speed=cfg.speed
        )
        self.retro_engine = math_shaders.RetroArcadeEngine(
            cfg.width, cfg.height, seed=cfg.seed, speed=cfg.speed
        )
        self.temps = TemperatureProvider(
            source=cfg.temp_source,
            latitude=cfg.latitude,
            longitude=cfg.longitude,
            static_c=cfg.temp_static_c,
            refresh_s=cfg.temp_refresh_s,
        )
        self.ai: AiFrameSource | None = AiFrameSource(cfg) if cfg.ai_enabled else None
        self._clients: list[tuple[ClientSession, socket.socket]] = []
        self._clients_lock = threading.Lock()
        self._stop = threading.Event()
        self._t0 = time.monotonic()

    # -- rendering -------------------------------------------------------
    def render_frame(self, session: ClientSession | None = None) -> np.ndarray:
        cfg = self.cfg
        t = time.monotonic() - self._t0
        frame = (
            self.retro_engine.render(t)
            if cfg.source == "retro"
            else self.engine.render(t)
        )

        if self.ai is not None:
            ai_frame = self.ai.frame
            if ai_frame is not None:
                if cfg.source == "ai":
                    fade = min(1.0, self.ai.age_s / max(cfg.ai_fade_s, 0.1))
                    frame = math_shaders.blend(frame, ai_frame, fade)
                elif cfg.source == "hybrid":
                    pulse = 0.45 + 0.25 * np.sin(t * 0.11)
                    frame = math_shaders.blend(frame, ai_frame, float(pulse))

        if cfg.source != "retro":
            math_shaders.swirling_frame(
                frame,
                t,
                thickness=cfg.border_thickness,
                seed_phase=self.engine.hue_origin,
                intensity=cfg.border_intensity,
            )

        if cfg.hud:
            local_temp = session.snapshot()[0] if session else None
            state = HudState(
                server_temp_c=self.temps.value,
                local_temp_c=local_temp,
                now=local_now(cfg.timezone),
                server_label=cfg.temp_label_server,
                local_label=cfg.temp_label_local,
            )
            if cfg.source == "retro":
                render_centered_hud(frame, state, cfg)
            else:
                render_hud(frame, state, cfg)
        return frame

    def frame_bytes(self, session: ClientSession | None = None) -> bytes:
        payload = to_rgb565(self.render_frame(session), self.cfg.byte_order)
        return MAGIC + struct.pack("<I", len(payload)) + payload

    # -- networking ------------------------------------------------------
    def serve_forever(self) -> None:
        cfg = self.cfg
        self.temps.start()
        if self.ai is not None:
            self.ai.start()

        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        # Windows SO_REUSEADDR lets a second process silently steal a port that
        # is already bound, so a stale instance can swallow every connection and
        # the board just times out. SO_EXCLUSIVEADDRUSE makes the clash loud.
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            server.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        else:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            server.bind((cfg.host, cfg.port))
        except OSError as exc:
            server.close()
            raise RuntimeError(
                f"port {cfg.port} is already in use - another copy of the art "
                f"server is still running. Close it and start again ({exc})."
            ) from exc
        server.listen(4)
        server.settimeout(1.0)

        print(
            f"[*] pico-dvi-art-server on {cfg.host}:{cfg.port} | "
            f"{cfg.width}x{cfg.height} RGB565 ({cfg.byte_order}-endian) | "
            f"{cfg.frame_size} B/frame | source={cfg.source} | fps={cfg.fps:g}"
        )
        print(f"[*] local addresses: {', '.join(local_addresses())}")
        print(
            f"[*] use the app's 'Push OTA' button (or create {OTA_TRIGGER.name}, "
            "e.g. `New-Item ota.trigger` on Windows or `touch ota.trigger` on "
            "Linux/macOS, or run push_ota.py) to order an OTA update"
        )

        watcher = threading.Thread(target=self._ota_watch_loop, name="ota-watch", daemon=True)
        watcher.start()

        try:
            while not self._stop.is_set():
                try:
                    conn, addr = server.accept()
                except socket.timeout:
                    continue
                conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                threading.Thread(
                    target=self._client_thread, args=(conn, addr), daemon=True
                ).start()
        except KeyboardInterrupt:
            print("\n[*] shutting down")
        finally:
            self._stop.set()
            self.temps.stop()
            if self.ai is not None:
                self.ai.stop()
            server.close()

    def _client_thread(self, conn: socket.socket, addr) -> None:
        session = ClientSession(address=addr)
        print(f"[+] client connected: {addr[0]}:{addr[1]}")
        with self._clients_lock:
            self._clients.append((session, conn))

        reader = threading.Thread(
            target=self._uplink_reader, args=(conn, session), daemon=True
        )
        reader.start()

        interval = 1.0 / self.cfg.fps
        sent = 0
        started = time.monotonic()
        next_due = time.monotonic()
        try:
            # No overall socket timeout: a frame that takes a while to drain is
            # normal backpressure from a client slower than cfg.fps, not a
            # fault. _send_frame() enforces a *stall* timeout instead, so we
            # only drop a client that has genuinely stopped reading.
            conn.settimeout(None)
            while not self._stop.is_set():
                self._send_frame(conn, self.frame_bytes(session), session.send_lock)
                sent += 1
                if sent % 200 == 0:
                    elapsed = time.monotonic() - started
                    temp, fw = session.snapshot()
                    print(
                        f"[=] {addr[0]} {sent} frames, {sent / elapsed:.1f} fps out, "
                        f"fw={fw}, mcu={temp if temp is None else round(temp, 1)}"
                    )
                next_due += interval
                delay = next_due - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                else:  # we fell behind; resync instead of accumulating debt
                    next_due = time.monotonic()
        except (ConnectionResetError, BrokenPipeError, socket.timeout, OSError) as exc:
            print(f"[-] client {addr[0]} dropped: {exc}")
        finally:
            with self._clients_lock:
                self._clients = [(s, c) for s, c in self._clients if c is not conn]
            try:
                conn.close()
            except OSError:
                pass
            print(f"[-] client disconnected: {addr[0]} after {sent} frames")

    # A Pico that is drawing every frame it receives is allowed to be slower
    # than cfg.fps - TCP backpressure just makes send() wait. Only a client
    # that accepts nothing at all for this long is considered dead.
    STALL_TIMEOUT_S = 20.0

    def _send_frame(
        self, conn: socket.socket, payload: bytes, lock: threading.Lock
    ) -> None:
        view = memoryview(payload)
        offset = 0
        # Held for the whole frame: an OTA command written between two chunks
        # would land inside the payload and desync the client's parser.
        with lock:
            while offset < len(view):
                if not select.select([], [conn], [], self.STALL_TIMEOUT_S)[1]:
                    raise socket.timeout(
                        f"client accepted no data for {self.STALL_TIMEOUT_S:g}s"
                    )
                offset += conn.send(view[offset:])

    def _uplink_reader(self, conn: socket.socket, session: ClientSession) -> None:
        buffer = b""
        try:
            while not self._stop.is_set():
                chunk = conn.recv(256)
                if not chunk:
                    return
                buffer += chunk
                while b"\n" in buffer:
                    line, buffer = buffer.split(b"\n", 1)
                    self._handle_uplink(line.decode("utf-8", "replace").strip(), session)
                if len(buffer) > 1024:  # desynced garbage - drop it
                    buffer = b""
        except OSError:
            return

    @staticmethod
    def _handle_uplink(line: str, session: ClientSession) -> None:
        if not line:
            return
        parts = line.split()
        verb = parts[0].upper()
        with session.lock:
            session.last_seen = time.time()
            if verb == "TEMP" and len(parts) >= 2:
                try:
                    session.local_temp_c = float(parts[1])
                except ValueError:
                    pass
            elif verb == "HELLO":
                session.fw_version = parts[1] if len(parts) > 1 else "?"
                session.device_id = parts[2] if len(parts) > 2 else ""
                print(f"[i] {session.address[0]} says hello: fw={session.fw_version} id={session.device_id}")
            elif verb == "STAT":
                for token in parts[1:]:
                    if token.startswith("fps="):
                        try:
                            session.client_fps = float(token[4:])
                        except ValueError:
                            pass

    # -- OTA control -----------------------------------------------------
    def broadcast_command(self, command: dict) -> int:
        payload = json.dumps(command).encode("utf-8")
        packet = MAGIC_COMMAND + struct.pack("<I", len(payload)) + payload
        delivered = 0
        with self._clients_lock:
            targets = list(self._clients)
        for session, conn in targets:
            try:
                with session.send_lock:
                    conn.sendall(packet)
                delivered += 1
            except OSError:
                continue
        print(f"[*] command {command} sent to {delivered} client(s)")
        return delivered

    def _ota_watch_loop(self) -> None:
        while not self._stop.wait(2.0):
            if not OTA_TRIGGER.exists():
                continue
            try:
                raw = OTA_TRIGGER.read_text(encoding="utf-8").strip()
                command = json.loads(raw) if raw else {"cmd": "ota"}
            except Exception:
                command = {"cmd": "ota"}
            self.broadcast_command(command)
            try:
                OTA_TRIGGER.unlink()
            except OSError:
                pass


def local_addresses() -> list[str]:
    addresses = {"127.0.0.1"}
    try:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect(("8.8.8.8", 80))
        addresses.add(probe.getsockname()[0])
        probe.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addresses.add(info[4][0])
    except OSError:
        pass
    return sorted(addresses)


def self_test(cfg: Config, frames: int = 60) -> None:
    """Render frames without a network client and report throughput."""
    server = ArtServer(cfg)
    start = time.perf_counter()
    size = 0
    for _ in range(frames):
        size = len(server.frame_bytes(None))
    elapsed = time.perf_counter() - start
    expected = cfg.frame_size + HEADER_SIZE
    print(f"[self-test] {frames} frames in {elapsed:.2f}s -> {frames / elapsed:.1f} fps")
    print(f"[self-test] packet size {size} B (expected {expected} B)")
    if size != expected:
        raise SystemExit("frame size mismatch")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Pico DVI infinite art frame server")
    parser.add_argument("--host")
    parser.add_argument("--port", type=int)
    parser.add_argument("--fps", type=float)
    parser.add_argument("--source", choices=["shader", "retro", "ai", "hybrid"])
    parser.add_argument("--speed", type=float)
    parser.add_argument("--seed", type=int)
    parser.add_argument("--byte-order", dest="byte_order", choices=["little", "big"])
    parser.add_argument("--no-hud", action="store_true")
    parser.add_argument("--border", type=int, dest="border_thickness")
    parser.add_argument("--temp-source", dest="temp_source", choices=["weather", "cpu", "static", "none"])
    parser.add_argument("--self-test", type=int, nargs="?", const=60, default=None,
                        help="render N frames locally and exit")
    return parser.parse_args(argv)


def config_from_args(args: argparse.Namespace) -> Config:
    cfg = Config.load()
    overrides = {
        k: v
        for k, v in vars(args).items()
        if v is not None and k not in ("self_test", "no_hud")
    }
    if args.no_hud:
        overrides["hud"] = False
    cfg.update(overrides)
    cfg.validate()
    return cfg


def main(argv: list[str] | None = None) -> None:
    args = parse_args(argv)
    cfg = config_from_args(args)
    if args.self_test is not None:
        self_test(cfg, args.self_test)
        return
    ArtServer(cfg).serve_forever()


if __name__ == "__main__":
    main()

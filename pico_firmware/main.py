"""Pico W streaming client: pulls RGB565 frames from the art server.

Downlink packets (little-endian):
    b'\xAA\xBB\xCC\xDD' | uint32 length | RGB565 payload   -> blit to panel
    b'\xAA\xBB\xCC\xEE' | uint32 length | UTF-8 JSON        -> {"cmd": "ota"|"reboot"}

Uplink (plain text lines):
    HELLO <version> <device_id>
    TEMP <celsius>      # RP2040 on-chip sensor, rendered as "MCU" on the HUD
    STAT fps=<f> drops=<n>

If the server is unreachable the panel shows a local fallback animation and
the client keeps retrying, so the frame never goes black.
"""

import gc
import machine
import socket
import struct
import time

try:
    import ujson as json
except ImportError:
    import json

import config
import display_driver
import ota

FRAME_MAGIC = b"\xAA\xBB\xCC\xDD"
COMMAND_MAGIC = b"\xAA\xBB\xCC\xEE"
HEADER_SIZE = 8
FRAME_SIZE = config.FRAME_SIZE

_adc = machine.ADC(4)
_CONVERSION = 3.3 / 65535


def chip_temperature():
    """RP2040 internal temperature sensor, degrees Celsius."""
    voltage = _adc.read_u16() * _CONVERSION
    return 27.0 - (voltage - 0.706) / 0.001721


def firmware_version():
    return ota.local_version()


def read_exactly(sock, view, length):
    """Fill `view[:length]` or raise. Returns the number of bytes read."""
    got = 0
    while got < length:
        chunk = sock.readinto(view[got:length], length - got)
        if not chunk:
            raise OSError("stream closed")
        got += chunk
    return got


def read_bytes(sock, length):
    """Read exactly `length` bytes; `sock.read` may return short chunks."""
    parts = []
    got = 0
    while got < length:
        block = sock.read(length - got)
        if not block:
            raise OSError("stream closed")
        parts.append(block)
        got += len(block)
    return b"".join(parts)


def read_header(sock):
    header = read_bytes(sock, HEADER_SIZE)
    return header[:4], struct.unpack("<I", header[4:8])[0]


class ArtClient:
    def __init__(self, display):
        self.display = display
        self.buffer = display.buffer
        self.frames = 0
        self.drops = 0
        self.last_telemetry = 0
        self.last_ota_check = time.time()
        self.confirmed = False

    # ------------------------------------------------------------- helpers
    def send_line(self, sock, text):
        try:
            sock.write((text + "\n").encode())
        except OSError:
            pass

    def push_telemetry(self, sock, fps):
        now = time.time()
        if now - self.last_telemetry < config.TELEMETRY_INTERVAL_S:
            return
        self.last_telemetry = now
        self.send_line(sock, "TEMP %.2f" % chip_temperature())
        self.send_line(sock, "STAT fps=%.1f drops=%d" % (fps, self.drops))

    def handle_command(self, payload):
        try:
            command = json.loads(payload)
        except (ValueError, TypeError):
            return
        verb = command.get("cmd", "")
        print("[cmd] server issued:", command)
        if verb == "ota":
            if ota.connect_wifi(verbose=False):
                ota.check_and_update()  # resets the board when an update lands
        elif verb == "reboot":
            time.sleep(0.5)
            machine.reset()

    def maybe_scheduled_ota(self):
        minutes = getattr(config, "OTA_CHECK_MINUTES", 0)
        if not minutes:
            return
        if time.time() - self.last_ota_check < minutes * 60:
            return
        self.last_ota_check = time.time()
        print("[ota] scheduled check")
        try:
            ota.check_and_update(verbose=False)
        except Exception as exc:
            print("[ota] scheduled check failed:", exc)

    # ------------------------------------------------------------ fallback
    def show_offline(self, phase):
        display_driver.fill_gradient(self.buffer, phase)
        self.display.show()

    # -------------------------------------------------------------- stream
    def stream_once(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(config.SOCKET_TIMEOUT_S)
        address = socket.getaddrinfo(config.SERVER_IP, config.SERVER_PORT)[0][-1]
        print("[net] connecting to %s:%d" % (config.SERVER_IP, config.SERVER_PORT))
        sock.connect(address)
        print("[net] linked - streaming pixels")

        self.send_line(sock, "HELLO %d %s" % (firmware_version(), config.DEVICE_ID))
        self.send_line(sock, "TEMP %.2f" % chip_temperature())

        view = memoryview(self.buffer)
        window_start = time.ticks_ms() if hasattr(time, "ticks_ms") else 0
        window_frames = 0
        fps = 0.0
        try:
            while True:
                magic, length = read_header(sock)

                if magic == FRAME_MAGIC:
                    if length != FRAME_SIZE:
                        # Wrong geometry: drain and resync rather than corrupt RAM.
                        self.drops += 1
                        skipped = 0
                        while skipped < length:
                            block = sock.read(min(512, length - skipped))
                            if not block:
                                raise OSError("stream closed")
                            skipped += len(block)
                        continue
                    read_exactly(sock, view, FRAME_SIZE)
                    self.display.show()
                    self.frames += 1
                    window_frames += 1
                    if not self.confirmed and self.frames >= 20:
                        ota.mark_boot_ok()  # the new firmware really works
                        self.confirmed = True

                elif magic == COMMAND_MAGIC:
                    payload = read_bytes(sock, length)
                    self.handle_command(payload)
                    continue

                else:
                    self.drops += 1
                    continue

                if hasattr(time, "ticks_ms"):
                    elapsed = time.ticks_diff(time.ticks_ms(), window_start)
                    if elapsed >= 2000:
                        fps = window_frames * 1000.0 / elapsed
                        window_start = time.ticks_ms()
                        window_frames = 0

                self.push_telemetry(sock, fps)
                self.maybe_scheduled_ota()
                if self.frames % 120 == 0:
                    gc.collect()
        finally:
            try:
                sock.close()
            except OSError:
                pass

    def run(self):
        phase = 0
        while True:
            try:
                if not ota.connect_wifi(verbose=False).isconnected():
                    raise OSError("wi-fi down")
                self.stream_once()
            except Exception as exc:
                print("[net] link lost:", exc)
                for _ in range(config.RECONNECT_DELAY_S * 4):
                    phase += 7
                    self.show_offline(phase)
                    time.sleep(0.25)
                gc.collect()


def main():
    print("[boot] pico-dvi-art client v%d (%s)" % (firmware_version(), config.DEVICE_ID))
    display = display_driver.open_display()
    ArtClient(display).run()


main()

"""Display abstraction for the Pico DVI LCD (400x240 RGB565).

There is no single "picodvi" API across the MicroPython / CircuitPython
builds shipped for these panels, so this module probes the backends in order
and falls back to a headless stub. That means you can bring the Wi-Fi stream
up and verify it over the REPL before the panel library is sorted out.

Every backend exposes the same tiny interface:

    display.buffer   -> bytearray/memoryview of WIDTH*HEIGHT*2 bytes
    display.show()   -> push the buffer to the panel
    display.name     -> backend name for logging

Adapting to your library: implement a class with those three members and add
it to `_BACKENDS`.
"""

import config

FRAME_SIZE = config.FRAME_SIZE


class PicoDVIDisplay:
    """`import picodvi` style MicroPython builds."""

    name = "picodvi"

    def __init__(self):
        import picodvi

        self._dvi = picodvi.DVI(
            width=config.WIDTH, height=config.HEIGHT, color=picodvi.RGB565
        )
        fb = self._dvi.framebuffer()
        self._fb = fb
        # Prefer writing straight into the driver's memory: no second 192 kB
        # allocation, which the RP2040 cannot spare.
        own = getattr(fb, "buffer", None)
        if own is None and isinstance(fb, (bytearray, memoryview)):
            own = fb
        self._direct = own is not None
        self.buffer = memoryview(own) if self._direct else memoryview(bytearray(FRAME_SIZE))

    def show(self):
        if not self._direct:
            self._fb.write(self.buffer)
        self._dvi.show()


class FramebufDisplay:
    """CircuitPython `picodvi.Framebuffer` / `framebufferio` style builds."""

    name = "framebuf"

    def __init__(self):
        import picodvi
        import framebufferio
        import board
        import displayio

        displayio.release_displays()
        fb = picodvi.Framebuffer(
            config.WIDTH,
            config.HEIGHT,
            clk_dp=board.CKP,
            clk_dn=board.CKN,
            red_dp=board.D0P,
            red_dn=board.D0N,
            green_dp=board.D1P,
            green_dn=board.D1N,
            blue_dp=board.D2P,
            blue_dn=board.D2N,
            color_depth=16,
        )
        self._display = framebufferio.FramebufferDisplay(fb)
        self.buffer = memoryview(fb)

    def show(self):
        try:
            self._display.refresh(minimum_frames_per_second=0)
        except Exception:
            pass


class HeadlessDisplay:
    """No panel library available - keeps the pipeline testable."""

    name = "headless"

    def __init__(self):
        self.buffer = memoryview(bytearray(FRAME_SIZE))
        self.frames = 0

    def show(self):
        self.frames += 1
        if self.frames % 100 == 0:
            middle = self.frames and 2 * (FRAME_SIZE // 4)
            print(
                "[display] headless frame %d, centre pixel = %02x%02x"
                % (self.frames, self.buffer[middle], self.buffer[middle + 1])
            )


_BACKENDS = (PicoDVIDisplay, FramebufDisplay, HeadlessDisplay)


def open_display():
    """Instantiate the first backend that initialises successfully."""
    for backend in _BACKENDS:
        try:
            display = backend()
            print("[display] backend:", display.name)
            return display
        except Exception as exc:
            print("[display] %s unavailable: %s" % (backend.name, exc))
    raise RuntimeError("no display backend available")


def fill_gradient(buffer, phase):
    """Cheap offline animation: two rows are computed, then copied down.

    Full per-pixel maths is far too slow in MicroPython, so this builds an
    800-byte RGB565 row (plus a dimmed twin) and slices them into every line.
    """
    width = config.WIDTH
    height = config.HEIGHT
    row = bytearray(width * 2)
    dim = bytearray(width * 2)
    for x in range(width):
        level = (x + phase) % 512
        if level > 255:
            level = 511 - level
        r = level >> 3
        g = ((255 - level) >> 2) & 0x3F
        b = ((level * 2) % 256) >> 3
        value = (r << 11) | (g << 5) | b
        row[x * 2] = value & 0xFF
        row[x * 2 + 1] = value >> 8
        faded = ((r >> 1) << 11) | ((g >> 1) << 5) | (b >> 1)
        dim[x * 2] = faded & 0xFF
        dim[x * 2 + 1] = faded >> 8

    stride = width * 2
    band = phase // 4
    for y in range(height):
        start = y * stride
        buffer[start : start + stride] = row if (y + band) % 24 < 12 else dim
    return buffer

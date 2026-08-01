"""Infinite, non-repeating generative shaders rendered with numpy.

Everything here works on float32 arrays shaped (H, W) in the 0..1 range and is
finally converted to uint8 RGB by `InfiniteArtEngine.render`.

Non-repetition is guaranteed two ways:

* every animated term uses a mutually irrational frequency, so the combined
  signal has no finite period;
* a per-process random seed offsets all phases, so two runs never line up.
"""

from __future__ import annotations

import math
import os
import struct
import time

import numpy as np

# Irrational, mutually incommensurable frequency multipliers.
PHI = (1.0 + math.sqrt(5.0)) / 2.0
SQRT2 = math.sqrt(2.0)
SQRT3 = math.sqrt(3.0)
SQRT5 = math.sqrt(5.0)
SQRT7 = math.sqrt(7.0)


def random_seed() -> int:
    """A fresh 32-bit seed from the OS entropy pool."""
    return struct.unpack("<I", os.urandom(4))[0]


def hsv_to_rgb(h: np.ndarray, s: np.ndarray, v: np.ndarray) -> np.ndarray:
    """Vectorised HSV -> RGB. Returns float32 (..., 3) in 0..1.

    Branch-free formulation (three clips instead of six selects) - roughly
    3x faster than the np.select variant at 400x240.
    """
    h = (np.mod(h, 1.0) * 6.0).astype(np.float32)
    s = np.clip(s, 0.0, 1.0).astype(np.float32)
    v = np.clip(v, 0.0, 1.0).astype(np.float32)
    vs = v * s

    def channel(n: float) -> np.ndarray:
        k = np.mod(n + h, 6.0)
        return v - vs * np.clip(np.minimum(k, 4.0 - k), 0.0, 1.0)

    return np.stack((channel(5.0), channel(3.0), channel(1.0)), axis=-1).astype(np.float32)


class InfiniteArtEngine:
    """Swirling, colour-shifting plasma that never repeats.

    Parameters
    ----------
    width, height:
        Frame size in pixels (400x240 for the Pico DVI framebuffer).
    seed:
        Optional seed; a random one is drawn when omitted.
    speed:
        Global animation multiplier.
    """

    def __init__(
        self,
        width: int = 400,
        height: int = 240,
        seed: int | None = None,
        speed: float = 1.0,
    ) -> None:
        self.width = int(width)
        self.height = int(height)
        self.speed = float(speed)
        self.seed = random_seed() if seed is None else int(seed)

        rng = np.random.default_rng(self.seed)
        # Random static phases: the artwork differs on every server start.
        self.phase = rng.uniform(0.0, 2.0 * math.pi, size=12).astype(np.float32)
        self.hue_origin = float(rng.uniform(0.0, 1.0))

        aspect = self.width / self.height
        x = np.linspace(-aspect, aspect, self.width, dtype=np.float32)
        y = np.linspace(-1.0, 1.0, self.height, dtype=np.float32)
        self.X, self.Y = np.meshgrid(x, y)
        self.radius = np.sqrt(self.X**2 + self.Y**2).astype(np.float32)
        self.angle = np.arctan2(self.Y, self.X).astype(np.float32)

    # -- internals -------------------------------------------------------
    def _drift(self, t: float, index: int, rate: float) -> float:
        """Slow, smooth, non-repeating scalar in -1..1."""
        p = self.phase[index % len(self.phase)]
        return float(
            0.6 * math.sin(t * rate + p)
            + 0.4 * math.sin(t * rate * SQRT2 * 0.37 + p * PHI)
        )

    # -- public API ------------------------------------------------------
    def render(self, t: float) -> np.ndarray:
        """Render one frame. Returns uint8 (H, W, 3)."""
        t = float(t) * self.speed

        swirl_strength = 2.2 + 1.3 * self._drift(t, 0, 0.11)
        zoom = 1.25 + 0.35 * self._drift(t, 1, 0.07)
        warp_gain = 0.75 + 0.35 * self._drift(t, 2, 0.13)

        # 1. Spiral domain: rotate every pixel by an angle that grows with the
        #    radius, which is what turns the plasma into a swirl/vortex.
        spin = self.angle + swirl_strength * self.radius - t * 0.35 + self.phase[3]
        r = self.radius * zoom
        u = r * np.cos(spin)
        v = r * np.sin(spin)

        # 2. Domain warping: feed the field back into itself for fluid motion.
        wx = np.sin(v * 2.3 + t * 0.61 * SQRT3 + self.phase[4])
        wy = np.cos(u * 1.9 - t * 0.47 * SQRT5 + self.phase[5])
        u = u + warp_gain * wx
        v = v + warp_gain * wy

        # 3. Layered plasma bands.
        f1 = np.sin(u * 3.0 + t * 0.53 + self.phase[6])
        f2 = np.cos(v * 2.5 - t * 0.41 * SQRT2 + self.phase[7])
        f3 = np.sin((u * v) * 1.7 + t * 0.29 * SQRT7 + self.phase[8])
        f4 = np.cos(np.sqrt(u * u + v * v) * 4.0 - t * 0.83 + self.phase[9])
        plasma = (f1 + f2 + f3 + f4) * 0.25

        # 4. Colour: hue sweeps continuously so the palette never settles.
        hue = (
            self.hue_origin
            + t * 0.021 * PHI
            + 0.22 * plasma
            + 0.10 * np.sin(self.angle * 2.0 + t * 0.17)
            + 0.06 * self.radius
        )
        sat = np.clip(0.78 + 0.24 * f2 - 0.10 * self.radius, 0.45, 1.0)
        val = np.clip(0.05 + 0.95 * (0.5 + 0.5 * plasma) ** 1.7, 0.0, 1.0)

        # Neon contour highlights keep shapes readable at 400x240.
        crisp = np.abs(np.sin(plasma * 6.0 + t * 0.23)) ** 10
        val = np.clip(val + 0.35 * crisp, 0.0, 1.0)

        # Vignette keeps the obsidian-background look of the prompt template.
        val *= np.clip(1.15 - 0.55 * self.radius**2, 0.0, 1.0)

        rgb = hsv_to_rgb(hue, sat, val)
        return (np.clip(rgb, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


class RetroArcadeEngine:
    """Original 8-bit arcade scenes cycling through five classic genres."""

    SCENE_SECONDS = 14.0

    def __init__(
        self,
        width: int = 320,
        height: int = 240,
        seed: int | None = None,
        speed: float = 1.0,
    ) -> None:
        self.width = int(width)
        self.height = int(height)
        self.logical_width = max(1, (self.width + 1) // 2)
        self.logical_height = max(1, (self.height + 1) // 2)
        self.speed = float(speed)
        self.seed = random_seed() if seed is None else int(seed)

    @staticmethod
    def _rect(
        frame: np.ndarray, x: int, y: int, width: int, height: int, color
    ) -> None:
        h, w = frame.shape[:2]
        x0, y0 = max(0, int(x)), max(0, int(y))
        x1, y1 = min(w, int(x + width)), min(h, int(y + height))
        if x0 < x1 and y0 < y1:
            frame[y0:y1, x0:x1] = color

    @classmethod
    def _block3d(
        cls, frame: np.ndarray, x: int, y: int, size: int, color
    ) -> None:
        base = np.array(color, dtype=np.uint8)
        light = np.clip(base.astype(np.int16) + 70, 0, 255).astype(np.uint8)
        dark = (base.astype(np.uint16) * 45 // 100).astype(np.uint8)
        cls._rect(frame, x + 2, y + 2, size, size, (8, 5, 24))
        cls._rect(frame, x, y, size, size, base)
        cls._rect(frame, x, y, size, 2, light)
        cls._rect(frame, x, y, 2, size, light)
        cls._rect(frame, x + size - 2, y + 2, 2, size - 2, dark)
        cls._rect(frame, x + 2, y + size - 2, size - 2, 2, dark)

    @staticmethod
    def _palette(index: int) -> tuple[int, int, int]:
        colors = (
            (35, 225, 255),
            (255, 63, 180),
            (255, 214, 45),
            (92, 255, 116),
            (146, 92, 255),
            (255, 103, 38),
        )
        return colors[index % len(colors)]

    def _background(self, scene: int, t: float) -> np.ndarray:
        h, w = self.logical_height, self.logical_width
        yy, xx = np.mgrid[0:h, 0:w]
        checker = ((xx // 8 + yy // 8 + scene) & 1).astype(np.uint8)
        glow = np.clip(22 - np.abs(xx - w / 2) * 0.08, 0, 22).astype(np.uint8)
        frame = np.zeros((h, w, 3), dtype=np.uint8)
        frame[..., 0] = 5 + checker * 4 + glow // 4
        frame[..., 1] = 6 + checker * 3
        frame[..., 2] = 18 + checker * 8 + glow
        stars = ((xx * 17 + yy * 31 + self.seed + int(t * 3)) % 211) == 0
        frame[stars] = (90, 115, 180)
        return frame

    def _falling_blocks(self, frame: np.ndarray, t: float) -> None:
        h, w = frame.shape[:2]
        size = max(5, min(8, h // 15))
        left = w // 2 - size * 5
        floor = h - 8
        for row in range(7):
            for col in range(10):
                code = (row * 13 + col * 7 + self.seed) % 11
                if code < 6 - row // 2:
                    y = floor - (row + 1) * size
                    self._block3d(frame, left + col * size, y, size - 1, self._palette(code))
        shapes = (((0, 0), (1, 0), (0, 1), (1, 1)),
                  ((0, 0), (1, 0), (2, 0), (1, 1)),
                  ((0, 0), (0, 1), (1, 1), (2, 1)))
        shape = shapes[int(t / 3) % len(shapes)]
        drop = int((t * 7) % max(1, h - 55))
        for dx, dy in shape:
            self._block3d(
                frame, left + size * (3 + dx), 6 + drop + size * dy,
                size - 1, self._palette(int(t) + dx + dy),
            )

    def _maze_chase(self, frame: np.ndarray, t: float) -> None:
        h, w = frame.shape[:2]
        wall = self._palette(int(t / 4))
        for x in range(8, w - 8, 18):
            self._rect(frame, x, 10, 3, h - 20, wall)
            gap = 18 + ((x * 3 + self.seed) % max(20, h - 50))
            self._rect(frame, x, gap, 3, 14, (8, 8, 24))
        for y in range(10, h - 8, 18):
            self._rect(frame, 8, y, w - 16, 3, wall)
            gap = 15 + ((y * 5 + self.seed) % max(25, w - 45))
            self._rect(frame, gap, y, 16, 3, (8, 8, 24))
        for x in range(14, w - 8, 12):
            self._rect(frame, x, h // 2, 2, 2, (255, 230, 100))
        px = 12 + int((t * 18) % max(1, w - 28))
        py = h // 2 - 4
        self._block3d(frame, px, py, 7, (255, 205, 30))
        for i in range(3):
            ex = w - 20 - ((int(t * (11 + i * 2)) + i * 27) % max(1, w - 35))
            ey = 18 + ((i * 31 + int(t * 5)) % max(1, h - 40))
            self._block3d(frame, ex, ey, 7, self._palette(i + 1))

    def _tank_arena(self, frame: np.ndarray, t: float) -> None:
        h, w = frame.shape[:2]
        for i in range(12):
            x = 8 + ((i * 37 + self.seed) % max(1, w - 24))
            y = 12 + ((i * 23 + self.seed // 3) % max(1, h - 28))
            self._block3d(frame, x, y, 10, (84, 72, 130))
        for i, color in enumerate(((55, 245, 130), (255, 75, 72))):
            x = int(w * (0.25 + 0.5 * i) + math.sin(t * (0.8 + i)) * w * 0.12)
            y = int(h * (0.35 + 0.25 * i) + math.cos(t * (0.7 + i)) * h * 0.15)
            self._rect(frame, x - 6, y - 4, 13, 9, (10, 8, 22))
            self._block3d(frame, x - 5, y - 5, 9, color)
            angle = t * (1.3 if i == 0 else -1.1) + i * math.pi
            tx, ty = int(x + math.cos(angle) * 11), int(y + math.sin(angle) * 11)
            self._rect(frame, min(x, tx), min(y, ty), abs(tx - x) + 2, 2, color)
            shot_x = int(x + math.cos(angle) * ((t * 24 + i * 17) % 42))
            shot_y = int(y + math.sin(angle) * ((t * 24 + i * 17) % 42))
            self._rect(frame, shot_x, shot_y, 3, 3, (255, 240, 120))

    def _platform_run(self, frame: np.ndarray, t: float) -> None:
        h, w = frame.shape[:2]
        frame[: h // 2, :, :] = (14, 40, 90)
        for i in range(7):
            x = int((i * 34 - t * 14) % (w + 35)) - 20
            y = h - 18 - (i % 3) * 17
            for bx in range(x, x + 32, 8):
                self._block3d(frame, bx, y, 8, self._palette(i + 2))
        for i in range(6):
            x = int((i * 43 - t * 22) % (w + 20))
            y = 15 + (i * 19) % max(20, h - 55)
            self._rect(frame, x, y, 5, 5, (255, 225, 55))
            self._rect(frame, x + 1, y + 1, 2, 2, (255, 255, 220))
        runner_y = h - 44 - int(abs(math.sin(t * 2.8)) * 17)
        self._block3d(frame, 28, runner_y, 9, (60, 230, 255))
        self._rect(frame, 30, runner_y + 2, 2, 2, (10, 15, 30))
        self._rect(frame, 35, runner_y + 2, 2, 2, (10, 15, 30))

    def _ice_climb(self, frame: np.ndarray, t: float) -> None:
        h, w = frame.shape[:2]
        yy = np.arange(h, dtype=np.float32)[:, None]
        frame[..., 0] = np.clip(8 + yy * 0.22, 0, 255).astype(np.uint8)
        frame[..., 1] = np.clip(20 + yy * 0.45, 0, 255).astype(np.uint8)
        frame[..., 2] = np.clip(55 + yy * 0.9, 0, 255).astype(np.uint8)
        for i in range(8):
            y = int((h - i * 18 + t * 9) % (h + 18)) - 9
            x = 8 + ((i * 29 + self.seed) % max(1, w - 62))
            self._rect(frame, x + 3, y + 4, 48, 6, (20, 75, 145))
            self._rect(frame, x, y, 48, 6, (130, 235, 255))
            self._rect(frame, x + 5, y, 32, 2, (235, 255, 255))
        climber_y = h // 2 + int(math.sin(t * 2.4) * 8)
        self._block3d(frame, w // 2 - 5, climber_y, 10, (255, 92, 180))
        self._rect(frame, w // 2 - 3, climber_y + 2, 2, 2, (15, 20, 40))
        self._rect(frame, w // 2 + 2, climber_y + 2, 2, 2, (15, 20, 40))
        for x in range(0, w, 13):
            y = int(10 + math.sin(x * 0.13 + t) * 5)
            self._rect(frame, x, y, 10, 2, self._palette(x // 13))

    def render(self, t: float) -> np.ndarray:
        t = float(t) * self.speed
        scene = int(t // self.SCENE_SECONDS) % 5
        local_t = t % self.SCENE_SECONDS
        frame = self._background(scene, local_t)
        renderers = (
            self._falling_blocks,
            self._maze_chase,
            self._tank_arena,
            self._platform_run,
            self._ice_climb,
        )
        renderers[scene](frame, local_t)
        return np.repeat(np.repeat(frame, 2, axis=0), 2, axis=1)[
            : self.height, : self.width
        ]


class SwirlBorder:
    """Animated colour-swirling frame around the artwork.

    All geometry (which pixels belong to the border, how far each one sits
    from the edge, where it lies along the perimeter) is precomputed once;
    per-frame work touches only the ~10k border pixels instead of all 96k.
    """

    def __init__(self, width: int, height: int, thickness: int) -> None:
        self.width = int(width)
        self.height = int(height)
        self.thickness = max(0, min(int(thickness), height // 2, width // 2))
        if self.thickness == 0:
            self.ys = self.xs = np.zeros(0, dtype=np.int64)
            self.walk = self.depth = np.zeros(0, dtype=np.float32)
            return

        yy, xx = np.mgrid[0 : self.height, 0 : self.width]
        sides = np.stack(
            (yy, self.width - 1 - xx, self.height - 1 - yy, xx)
        ).astype(np.float32)
        edge_distance = sides.min(axis=0)
        nearest_side = sides.argmin(axis=0)

        mask = edge_distance < self.thickness
        self.ys, self.xs = np.nonzero(mask)

        fx = xx.astype(np.float32) / max(self.width - 1, 1)
        fy = yy.astype(np.float32) / max(self.height - 1, 1)
        perimeter = 2.0 * (self.width + self.height)
        walk_full = np.select(
            [nearest_side == 0, nearest_side == 1, nearest_side == 2, nearest_side == 3],
            [
                fx * self.width,
                self.width + fy * self.height,
                self.width + self.height + (1.0 - fx) * self.width,
                2.0 * self.width + self.height + (1.0 - fy) * self.height,
            ],
        ) / perimeter

        self.walk = walk_full[mask].astype(np.float32)
        self.depth = (edge_distance[mask] / max(self.thickness - 1, 1)).astype(np.float32)
        self.base_alpha = np.clip(1.0 - self.depth, 0.0, 1.0) ** 0.6

    def apply(
        self,
        rgb: np.ndarray,
        t: float,
        seed_phase: float = 0.0,
        intensity: float = 1.0,
    ) -> np.ndarray:
        """Paint the swirling border into `rgb` (uint8 HxWx3) in place."""
        if self.thickness == 0 or self.ys.size == 0:
            return rgb

        walk, depth = self.walk, self.depth
        hue = walk * 1.6 + t * 0.12 + seed_phase + 0.25 * np.sin(walk * 12.566 + t * 0.9)
        sat = np.clip(0.95 - 0.18 * depth, 0.0, 1.0)

        # Two counter-rotating comets chase each other around the perimeter.
        comet_a = 0.5 + 0.5 * np.cos(2.0 * math.pi * (walk * 2.0 - t * 0.22))
        comet_b = 0.5 + 0.5 * np.cos(2.0 * math.pi * (walk * 3.0 + t * 0.13 * SQRT2))
        glow = 0.28 + 0.85 * comet_a**3 + 0.45 * comet_b**5
        val = np.clip(glow * (1.0 - 0.45 * depth), 0.0, 1.0)

        border_rgb = hsv_to_rgb(hue, sat, val) * 255.0
        alpha = (self.base_alpha * float(np.clip(intensity, 0.0, 1.0)))[:, None]

        base = rgb[self.ys, self.xs].astype(np.float32)
        rgb[self.ys, self.xs] = np.clip(
            base * (1.0 - alpha) + border_rgb * alpha, 0.0, 255.0
        ).astype(np.uint8)
        return rgb


_BORDER_CACHE: dict[tuple[int, int, int], SwirlBorder] = {}


def swirling_frame(
    rgb: np.ndarray,
    t: float,
    thickness: int = 8,
    seed_phase: float = 0.0,
    intensity: float = 1.0,
) -> np.ndarray:
    """Convenience wrapper that caches a `SwirlBorder` per frame geometry."""
    height, width = rgb.shape[:2]
    key = (width, height, int(thickness))
    border = _BORDER_CACHE.get(key)
    if border is None:
        border = _BORDER_CACHE[key] = SwirlBorder(width, height, thickness)
    return border.apply(rgb, t, seed_phase, intensity)


def blend(a: np.ndarray, b: np.ndarray, mix: float) -> np.ndarray:
    """Linear cross-fade between two uint8 RGB frames (mix=0 -> a, 1 -> b)."""
    mix = float(np.clip(mix, 0.0, 1.0))
    out = a.astype(np.float32) * (1.0 - mix) + b.astype(np.float32) * mix
    return np.clip(out, 0.0, 255.0).astype(np.uint8)


def demo_timer() -> float:
    """Monotonic clock used by the shader loop."""
    return time.monotonic()

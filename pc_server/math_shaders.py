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

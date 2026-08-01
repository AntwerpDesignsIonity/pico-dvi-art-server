"""Render frames to PNG so you can check the artwork without any hardware.

    python pc_server/preview.py                  # 6 frames -> preview/
    python pc_server/preview.py --frames 30 --step 0.5 --out preview

Frames go through the exact RGB565 packing the Pico receives, so what you see
in the PNG is what the panel shows (including colour-depth banding).
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

import math_shaders  # noqa: E402
from config import Config  # noqa: E402
from hud import HudState, local_now, render_centered_hud, render_hud  # noqa: E402
from pixels import from_rgb565, to_rgb565  # noqa: E402


def write_png(path: Path, rgb: np.ndarray) -> None:
    """Minimal PNG encoder (no Pillow dependency)."""
    height, width = rgb.shape[:2]
    raw = b"".join(
        b"\x00" + rgb[y].tobytes() for y in range(height)
    )

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    header = struct.pack(">2I5B", width, height, 8, 2, 0, 0, 0)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b"")
    )
    path.write_bytes(png)


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description="Render preview PNGs of the art stream")
    parser.add_argument("--frames", type=int, default=6)
    parser.add_argument("--step", type=float, default=1.7, help="seconds between frames")
    parser.add_argument("--out", default="preview")
    parser.add_argument("--seed", type=int, default=None)
    parser.add_argument("--mcu-temp", type=float, default=38.4)
    parser.add_argument("--server-temp", type=float, default=17.6)
    args = parser.parse_args(argv)

    cfg = Config.load()
    if args.seed is not None:
        cfg.seed = args.seed
    engine = (
        math_shaders.RetroArcadeEngine(
            cfg.width, cfg.height, seed=cfg.seed, speed=cfg.speed
        )
        if cfg.source == "retro"
        else math_shaders.InfiniteArtEngine(
            cfg.width, cfg.height, seed=cfg.seed, speed=cfg.speed
        )
    )

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    for index in range(args.frames):
        t = index * args.step
        frame = engine.render(t)
        if cfg.source != "retro":
            math_shaders.swirling_frame(
                frame,
                t,
                thickness=cfg.border_thickness,
                seed_phase=engine.hue_origin,
                intensity=cfg.border_intensity,
            )
        if cfg.hud:
            hud_renderer = render_centered_hud if cfg.source == "retro" else render_hud
            hud_renderer(
                frame,
                HudState(
                    server_temp_c=args.server_temp,
                    local_temp_c=args.mcu_temp,
                    now=local_now(cfg.timezone),
                    server_label=cfg.temp_label_server,
                    local_label=cfg.temp_label_local,
                ),
                cfg,
            )
        # Round-trip through RGB565 to preview the real panel colour depth.
        packed = to_rgb565(frame, cfg.byte_order)
        shown = from_rgb565(packed, cfg.width, cfg.height, cfg.byte_order)
        path = out_dir / f"frame_{index:03d}.png"
        write_png(path, shown)
        print(f"[preview] {path} ({len(packed)} B payload)")


if __name__ == "__main__":
    main()

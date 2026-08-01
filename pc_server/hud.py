"""HUD overlay: clock (top right), date, server temperature, chipset temperature.

Drawn server-side straight into the RGB frame before RGB565 packing, so the
Pico only ever has to blit bytes.
"""

from __future__ import annotations

import datetime as _dt
from dataclasses import dataclass

import numpy as np

from font5x7 import text_mask, text_size

TRACKING = 1


@dataclass
class HudState:
    """Live values rendered onto the frame."""

    server_temp_c: float | None = None
    local_temp_c: float | None = None
    now: _dt.datetime | None = None
    server_label: str = "OUT"
    local_label: str = "MCU"
    note: str = ""


def draw_text(
    rgb: np.ndarray,
    text: str,
    x: int,
    y: int,
    scale: int = 2,
    color: tuple[int, int, int] = (255, 255, 255),
    align: str = "left",
    alpha: float = 1.0,
) -> tuple[int, int]:
    """Blit `text` onto `rgb` (uint8 HxWx3). Returns the (width, height) drawn."""
    mask = text_mask(text, scale=scale, tracking=TRACKING)
    height, width = mask.shape
    if width == 0:
        return 0, height

    if align == "right":
        x -= width
    elif align == "center":
        x -= width // 2

    frame_h, frame_w = rgb.shape[:2]
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(frame_w, x + width), min(frame_h, y + height)
    if x0 >= x1 or y0 >= y1:
        return width, height

    sub = mask[y0 - y : y1 - y, x0 - x : x1 - x].astype(np.float32) * float(alpha)
    target = rgb[y0:y1, x0:x1].astype(np.float32)
    tint = np.array(color, dtype=np.float32)
    blended = target * (1.0 - sub[..., None]) + tint * sub[..., None]
    rgb[y0:y1, x0:x1] = np.clip(blended, 0.0, 255.0).astype(np.uint8)
    return width, height


def draw_text_shadowed(
    rgb: np.ndarray,
    text: str,
    x: int,
    y: int,
    scale: int = 2,
    color: tuple[int, int, int] = (255, 255, 255),
    align: str = "right",
    shadow: tuple[int, int, int] = (0, 0, 0),
) -> tuple[int, int]:
    offset = max(1, scale // 2)
    draw_text(rgb, text, x + offset, y + offset, scale, shadow, align, alpha=0.75)
    return draw_text(rgb, text, x, y, scale, color, align)


def darken_panel(
    rgb: np.ndarray,
    x0: int,
    y0: int,
    x1: int,
    y1: int,
    strength: float = 0.55,
) -> None:
    """Translucent dark plate so the HUD stays readable over bright plasma."""
    frame_h, frame_w = rgb.shape[:2]
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(frame_w, x1), min(frame_h, y1)
    if x0 >= x1 or y0 >= y1:
        return
    region = rgb[y0:y1, x0:x1].astype(np.float32)
    rgb[y0:y1, x0:x1] = np.clip(region * (1.0 - strength), 0, 255).astype(np.uint8)


def format_temp(value: float | None) -> str:
    if value is None:
        return "--.-\u00b0C"
    return f"{value:.1f}\u00b0C"


def local_now(timezone: str = "") -> _dt.datetime:
    if not timezone:
        return _dt.datetime.now()
    try:
        from zoneinfo import ZoneInfo

        return _dt.datetime.now(ZoneInfo(timezone))
    except Exception:
        return _dt.datetime.now()


def render_hud(rgb: np.ndarray, state: HudState, cfg) -> np.ndarray:
    """Draw the whole HUD block into the top-right corner of `rgb`."""
    now = state.now or local_now(getattr(cfg, "timezone", ""))

    if getattr(cfg, "clock_24h", True):
        clock_fmt = "%H:%M:%S" if getattr(cfg, "show_seconds", True) else "%H:%M"
        clock_text = now.strftime(clock_fmt)
    else:
        clock_fmt = "%I:%M:%S %p" if getattr(cfg, "show_seconds", True) else "%I:%M %p"
        clock_text = now.strftime(clock_fmt).upper()
        if clock_text.startswith("0"):
            clock_text = clock_text[1:]
    date_text = now.strftime(getattr(cfg, "date_format", "%Y-%m-%d %a")).upper()
    server_text = f"{state.server_label} {format_temp(state.server_temp_c)}"
    local_text = f"{state.local_label} {format_temp(state.local_temp_c)}"

    margin = int(getattr(cfg, "hud_margin", 14))
    clock_scale = int(getattr(cfg, "hud_scale_clock", 3))
    small_scale = int(getattr(cfg, "hud_scale_small", 2))

    lines = [
        (clock_text, clock_scale, (255, 255, 255)),
        (date_text, small_scale, (185, 225, 255)),
        (server_text, small_scale, (120, 245, 205)),
        (local_text, small_scale, (255, 190, 110)),
    ]
    if state.note:
        lines.append((state.note.upper(), small_scale, (255, 120, 140)))

    gaps = [6, 5, 3, 3]
    widths = [text_size(t, s, TRACKING)[0] for t, s, _ in lines]
    heights = [text_size(t, s, TRACKING)[1] for t, s, _ in lines]
    block_w = max(widths)
    block_h = sum(heights) + sum(gaps[: len(lines) - 1])

    right = rgb.shape[1] - margin
    top = margin
    pad = 6
    darken_panel(rgb, right - block_w - pad, top - pad, right + pad, top + block_h + pad)

    y = top
    for index, (text, scale, color) in enumerate(lines):
        draw_text_shadowed(rgb, text, right, y, scale, color, align="right")
        y += heights[index] + (gaps[index] if index < len(gaps) else 3)

    # Accent rule under the clock, tinted by the current second.
    rule_y = top + heights[0] + 2
    rule_x0 = max(0, right - block_w)
    accent = (
        int(120 + 120 * abs((now.second % 10) / 10.0 - 0.5) * 2),
        90,
        int(230 - 80 * (now.second % 6) / 6.0),
    )
    rgb[rule_y : rule_y + 2, rule_x0:right] = np.array(accent, dtype=np.uint8)
    return rgb

"""Tiny 5x7 bitmap font rendered with numpy.

The HUD is drawn on the server (the RP2040 has no spare cycles for text
rasterising while it is blitting a 192 kB framebuffer), so the font lives here.
Glyphs are stored as row strings for readability and compiled once into
uint8 masks of shape (7, 5).
"""

from __future__ import annotations

import numpy as np

GLYPH_WIDTH = 5
GLYPH_HEIGHT = 7

_RAW_GLYPHS: dict[str, str] = {
    " ": "00000 00000 00000 00000 00000 00000 00000",
    "0": "01110 10001 10011 10101 11001 10001 01110",
    "1": "00100 01100 00100 00100 00100 00100 01110",
    "2": "01110 10001 00001 00010 00100 01000 11111",
    "3": "11111 00010 00100 00010 00001 10001 01110",
    "4": "00010 00110 01010 10010 11111 00010 00010",
    "5": "11111 10000 11110 00001 00001 10001 01110",
    "6": "00110 01000 10000 11110 10001 10001 01110",
    "7": "11111 00001 00010 00100 01000 01000 01000",
    "8": "01110 10001 10001 01110 10001 10001 01110",
    "9": "01110 10001 10001 01111 00001 00010 01100",
    ":": "00000 00100 00100 00000 00100 00100 00000",
    ".": "00000 00000 00000 00000 00000 01100 01100",
    ",": "00000 00000 00000 00000 01100 01100 01000",
    "-": "00000 00000 00000 11111 00000 00000 00000",
    "+": "00000 00100 00100 11111 00100 00100 00000",
    "/": "00001 00010 00010 00100 01000 01000 10000",
    "%": "11001 11010 00010 00100 01000 01011 10011",
    "\u00b0": "01100 10010 10010 01100 00000 00000 00000",
    "!": "00100 00100 00100 00100 00100 00000 00100",
    "?": "01110 10001 00001 00010 00100 00000 00100",
    "(": "00010 00100 01000 01000 01000 00100 00010",
    ")": "01000 00100 00010 00010 00010 00100 01000",
    "A": "01110 10001 10001 11111 10001 10001 10001",
    "B": "11110 10001 10001 11110 10001 10001 11110",
    "C": "01110 10001 10000 10000 10000 10001 01110",
    "D": "11110 10001 10001 10001 10001 10001 11110",
    "E": "11111 10000 10000 11110 10000 10000 11111",
    "F": "11111 10000 10000 11110 10000 10000 10000",
    "G": "01110 10001 10000 10111 10001 10001 01111",
    "H": "10001 10001 10001 11111 10001 10001 10001",
    "I": "01110 00100 00100 00100 00100 00100 01110",
    "J": "00111 00010 00010 00010 00010 10010 01100",
    "K": "10001 10010 10100 11000 10100 10010 10001",
    "L": "10000 10000 10000 10000 10000 10000 11111",
    "M": "10001 11011 10101 10101 10001 10001 10001",
    "N": "10001 11001 11001 10101 10011 10011 10001",
    "O": "01110 10001 10001 10001 10001 10001 01110",
    "P": "11110 10001 10001 11110 10000 10000 10000",
    "Q": "01110 10001 10001 10001 10101 10010 01101",
    "R": "11110 10001 10001 11110 10100 10010 10001",
    "S": "01111 10000 10000 01110 00001 00001 11110",
    "T": "11111 00100 00100 00100 00100 00100 00100",
    "U": "10001 10001 10001 10001 10001 10001 01110",
    "V": "10001 10001 10001 10001 10001 01010 00100",
    "W": "10001 10001 10001 10101 10101 11011 10001",
    "X": "10001 10001 01010 00100 01010 10001 10001",
    "Y": "10001 10001 01010 00100 00100 00100 00100",
    "Z": "11111 00001 00010 00100 01000 10000 11111",
}


def _compile(rows: str) -> np.ndarray:
    grid = rows.split()
    if len(grid) != GLYPH_HEIGHT or any(len(r) != GLYPH_WIDTH for r in grid):
        raise ValueError(f"glyph must be {GLYPH_HEIGHT}x{GLYPH_WIDTH}: {rows!r}")
    return np.array([[1 if c == "1" else 0 for c in row] for row in grid], dtype=np.uint8)


GLYPHS: dict[str, np.ndarray] = {ch: _compile(rows) for ch, rows in _RAW_GLYPHS.items()}
_FALLBACK = GLYPHS["?"]


def glyph(char: str) -> np.ndarray:
    return GLYPHS.get(char.upper(), _FALLBACK)


def text_mask(text: str, scale: int = 1, tracking: int = 1) -> np.ndarray:
    """Rasterise `text` into a uint8 mask of shape (H, W) holding 0/1 values."""
    if scale < 1:
        raise ValueError("scale must be >= 1")
    if not text:
        return np.zeros((GLYPH_HEIGHT * scale, 0), dtype=np.uint8)

    columns: list[np.ndarray] = []
    gap = np.zeros((GLYPH_HEIGHT, tracking), dtype=np.uint8)
    for index, char in enumerate(text):
        if index:
            columns.append(gap)
        columns.append(glyph(char))
    mask = np.hstack(columns)
    if scale > 1:
        mask = np.kron(mask, np.ones((scale, scale), dtype=np.uint8))
    return mask


def text_size(text: str, scale: int = 1, tracking: int = 1) -> tuple[int, int]:
    """Return (width, height) in pixels for `text`."""
    if not text:
        return 0, GLYPH_HEIGHT * scale
    width = len(text) * GLYPH_WIDTH + (len(text) - 1) * tracking
    return width * scale, GLYPH_HEIGHT * scale

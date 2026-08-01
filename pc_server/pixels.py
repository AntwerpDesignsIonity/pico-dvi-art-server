"""RGB888 -> RGB565 packing and framing helpers shared by server and tools."""

from __future__ import annotations

import struct

import numpy as np

MAGIC = b"\xAA\xBB\xCC\xDD"
HEADER_SIZE = 8  # magic + uint32 payload length


def to_rgb565(rgb: np.ndarray, byte_order: str = "little") -> bytes:
    """Pack a uint8 (H, W, 3) array into raw RGB565 bytes.

    `byte_order` must match the display driver. MicroPython's `framebuf`
    RGB565 mode is big-endian; a raw memory blit into a little-endian
    framebuffer wants "little". Wrong order shows up as swapped colours.
    """
    if rgb.dtype != np.uint8 or rgb.ndim != 3 or rgb.shape[2] != 3:
        raise ValueError("expected uint8 array shaped (H, W, 3)")

    r = rgb[..., 0].astype(np.uint16)
    g = rgb[..., 1].astype(np.uint16)
    b = rgb[..., 2].astype(np.uint16)
    packed = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

    if byte_order == "big":
        packed = packed.astype(">u2")
    elif byte_order == "little":
        packed = packed.astype("<u2")
    else:
        raise ValueError("byte_order must be 'little' or 'big'")
    return packed.tobytes()


def from_rgb565(data: bytes, width: int, height: int, byte_order: str = "little") -> np.ndarray:
    """Inverse of `to_rgb565`, used by tests and the preview tool."""
    dtype = "<u2" if byte_order == "little" else ">u2"
    packed = np.frombuffer(data, dtype=dtype).reshape(height, width).astype(np.uint16)
    r = ((packed >> 11) & 0x1F).astype(np.uint8)
    g = ((packed >> 5) & 0x3F).astype(np.uint8)
    b = (packed & 0x1F).astype(np.uint8)
    # Replicate high bits so 0x1F maps back to 0xFF.
    r = (r << 3) | (r >> 2)
    g = (g << 2) | (g >> 4)
    b = (b << 3) | (b >> 2)
    return np.stack((r, g, b), axis=-1)


def frame_packet(payload: bytes) -> bytes:
    """Wrap a frame payload with the sync magic and its length."""
    return MAGIC + struct.pack("<I", len(payload)) + payload

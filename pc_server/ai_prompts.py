"""Infinite AI art prompts plus optional image fetching / resizing / RGB565.

Two things live here:

1. `build_prompt()` - the master prompt template with the rotating variable
   lists, so no two generated pieces are ever identical.
2. `AiFrameSource` - an optional background worker that calls an image API
   (or reads a local folder), crops/resizes to the framebuffer size and hands
   the result to the streaming loop as a uint8 RGB array.

Pillow and an API key are only needed when `ai_enabled` is on; the shader
source runs with numpy alone.
"""

from __future__ import annotations

import base64
import io
import json
import os
import random
import threading
import time
import urllib.request
from pathlib import Path

import numpy as np

PROMPT_TEMPLATE = """A highly detailed generative artwork featuring {subject}.
Designed in a {style} aesthetic, focusing on bold geometric patterns, sharp edge contrasts, and sweeping gradients.
Composition: Center-focused, minimalist framing optimized for a tiny wide-aspect display.
Lighting: High-contrast neon, dramatic rim-lighting, and vibrant luminescence.
Color Palette: {color_a} and {color_b} over a deep obsidian background.
Technical constraints: Extremely clean lines, zero microscopic text, zero noisy textures, clear shape definition visible at low pixel resolutions.
Variation seed: {seed}."""

SUBJECTS = [
    "swirling fluid dynamics",
    "fractured crystalline spires",
    "pulsing neural network nodes",
    "bioluminescent ocean currents",
    "cosmic nebula solar flares",
    "interlocking magnetic field lines",
    "molten glass ribbons in zero gravity",
    "spiral galaxies collapsing into vortices",
]

STYLES = [
    "generative math vector art",
    "neo-plasticism abstract",
    "Bauhaus geometric layout",
    "retro-futurist synthwave",
    "abstract minimalism line art",
    "risograph duotone poster",
    "kinetic op-art",
]

PALETTES = [
    ("cyan", "hot magenta"),
    ("cyberpunk gold", "deep violet"),
    ("electric emerald", "midnight black"),
    ("plasma orange", "cobalt blue"),
    ("acid green", "dark charcoal"),
    ("arctic white", "ultramarine"),
    ("coral red", "teal"),
]


def build_prompt(rng: random.Random | None = None) -> tuple[str, dict]:
    """Return (prompt, chosen variables). Variables rotate on every call."""
    rng = rng or random.Random()
    subject = rng.choice(SUBJECTS)
    style = rng.choice(STYLES)
    color_a, color_b = rng.choice(PALETTES)
    seed = rng.getrandbits(32)
    variables = {
        "subject": subject,
        "style": style,
        "color_a": color_a,
        "color_b": color_b,
        "seed": seed,
    }
    return PROMPT_TEMPLATE.format(**variables), variables


# ---------------------------------------------------------------------------
# Image helpers
# ---------------------------------------------------------------------------
def _require_pillow():
    try:
        from PIL import Image  # noqa: PLC0415
    except ImportError as exc:  # pragma: no cover - depends on environment
        raise RuntimeError(
            "AI image mode needs Pillow. Install it with: pip install pillow"
        ) from exc
    return Image


def image_bytes_to_frame(data: bytes, width: int, height: int) -> np.ndarray:
    """Decode, center-crop to the display aspect and resize to width x height."""
    Image = _require_pillow()
    with Image.open(io.BytesIO(data)) as img:
        img = img.convert("RGB")
        target_ratio = width / height
        src_w, src_h = img.size
        src_ratio = src_w / src_h
        if src_ratio > target_ratio:  # too wide -> crop sides
            new_w = int(src_h * target_ratio)
            left = (src_w - new_w) // 2
            img = img.crop((left, 0, left + new_w, src_h))
        elif src_ratio < target_ratio:  # too tall -> crop top/bottom
            new_h = int(src_w / target_ratio)
            top = (src_h - new_h) // 2
            img = img.crop((0, top, src_w, top + new_h))
        img = img.resize((width, height), Image.LANCZOS)
        return np.asarray(img, dtype=np.uint8)


def fetch_openai_image(prompt: str, model: str, size: str, api_key: str, timeout: float = 120.0) -> bytes:
    """Call the OpenAI images endpoint and return raw PNG bytes."""
    payload = json.dumps({"model": model, "prompt": prompt, "size": size, "n": 1}).encode()
    request = urllib.request.Request(
        "https://api.openai.com/v1/images/generations",
        data=payload,
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {api_key}",
        },
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        body = json.loads(response.read().decode("utf-8"))
    item = body["data"][0]
    if item.get("b64_json"):
        return base64.b64decode(item["b64_json"])
    with urllib.request.urlopen(item["url"], timeout=timeout) as response:
        return response.read()


class AiFrameSource:
    """Background producer of 400x240 RGB frames from an image generator.

    Failures are non-fatal: the streaming loop keeps using the shader while
    this worker retries, so the display never stalls.
    """

    def __init__(self, cfg) -> None:
        self.cfg = cfg
        self.width = cfg.width
        self.height = cfg.height
        self.interval = max(10.0, float(cfg.ai_interval_s))
        self.rng = random.Random()
        self._lock = threading.Lock()
        self._frame: np.ndarray | None = None
        self._prompt: str = ""
        self._arrived_at: float = 0.0
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self.cache_dir = Path(cfg.ai_folder)

    def start(self) -> "AiFrameSource":
        self._thread = threading.Thread(target=self._loop, name="ai-source", daemon=True)
        self._thread.start()
        return self

    def stop(self) -> None:
        self._stop.set()

    @property
    def frame(self) -> np.ndarray | None:
        with self._lock:
            return None if self._frame is None else self._frame.copy()

    @property
    def prompt(self) -> str:
        with self._lock:
            return self._prompt

    @property
    def age_s(self) -> float:
        with self._lock:
            return time.time() - self._arrived_at if self._arrived_at else float("inf")

    def _loop(self) -> None:
        while not self._stop.is_set():
            try:
                self._produce()
            except Exception as exc:
                print(f"[ai] generation failed: {exc}")
            self._stop.wait(self.interval)

    def _produce(self) -> None:
        if self.cfg.ai_provider == "folder":
            frame, label = self._from_folder()
        else:
            frame, label = self._from_openai()
        if frame is None:
            return
        with self._lock:
            self._frame = frame
            self._prompt = label
            self._arrived_at = time.time()
        print(f"[ai] new frame ready: {label[:70]}")

    def _from_folder(self) -> tuple[np.ndarray | None, str]:
        if not self.cache_dir.is_dir():
            raise RuntimeError(f"ai_folder '{self.cache_dir}' does not exist")
        images = [
            p
            for p in sorted(self.cache_dir.iterdir())
            if p.suffix.lower() in (".png", ".jpg", ".jpeg", ".bmp", ".webp")
        ]
        if not images:
            raise RuntimeError(f"no images inside '{self.cache_dir}'")
        chosen = self.rng.choice(images)
        return image_bytes_to_frame(chosen.read_bytes(), self.width, self.height), chosen.name

    def _from_openai(self) -> tuple[np.ndarray | None, str]:
        api_key = os.environ.get(self.cfg.ai_api_key_env, "")
        if not api_key:
            raise RuntimeError(
                f"environment variable {self.cfg.ai_api_key_env} is not set"
            )
        prompt, variables = build_prompt(self.rng)
        data = fetch_openai_image(prompt, self.cfg.ai_model, self.cfg.ai_size, api_key)
        self.cache_dir.mkdir(parents=True, exist_ok=True)
        (self.cache_dir / f"{int(time.time())}_{variables['seed']}.png").write_bytes(data)
        return image_bytes_to_frame(data, self.width, self.height), f"{variables['subject']} / {variables['style']}"


if __name__ == "__main__":  # quick prompt preview
    for _ in range(3):
        text, _vars = build_prompt()
        print(text)
        print("-" * 70)

"""Server-side configuration.

Values come from (lowest to highest priority):
  1. the defaults below,
  2. `pc_server/config.json` if present (copy `config.example.json`),
  3. environment variables `PICOART_<UPPER_KEY>`.

Never put API keys in config.json inside the repo - use the environment.
"""

from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass, field, fields
from pathlib import Path

CONFIG_PATH = Path(__file__).with_name("config.json")


@dataclass
class Config:
    # --- network ---
    host: str = "0.0.0.0"
    port: int = 5001

    # --- framebuffer ---
    # 320x240 is what the PICO-DVI-LCD carrier shows: the RP2350 emits
    # 640x480p60 over DVI and pixel-doubles this buffer into it.
    width: int = 320
    height: int = 240
    byte_order: str = "little"  # "little" or "big" - must match the driver
    fps: float = 20.0

    # --- art ---
    source: str = "shader"  # shader | ai | hybrid
    speed: float = 1.0
    seed: int | None = None
    border_thickness: int = 8
    border_intensity: float = 1.0

    # --- HUD ---
    hud: bool = True
    clock_24h: bool = True
    show_seconds: bool = True
    date_format: str = "%Y-%m-%d %a"
    timezone: str = ""  # empty = machine local time, else e.g. "Europe/Brussels"
    hud_margin: int = 14
    hud_scale_clock: int = 3
    hud_scale_small: int = 2

    # --- temperatures ---
    temp_source: str = "weather"  # weather | cpu | static | none
    temp_static_c: float = 21.0
    latitude: float = 51.2194  # Antwerp
    longitude: float = 4.4025
    temp_refresh_s: float = 600.0
    temp_label_server: str = "OUT"
    temp_label_local: str = "MCU"

    # --- AI image source (optional) ---
    ai_enabled: bool = False
    ai_provider: str = "openai"  # openai | folder
    ai_model: str = "gpt-image-1"
    ai_size: str = "1024x1024"
    ai_interval_s: float = 90.0
    ai_fade_s: float = 3.0
    ai_folder: str = "art_cache"
    ai_api_key_env: str = "OPENAI_API_KEY"

    extra: dict = field(default_factory=dict)

    @classmethod
    def load(cls, path: Path | None = None) -> "Config":
        cfg = cls()
        path = CONFIG_PATH if path is None else path
        if path.exists():
            data = json.loads(path.read_text(encoding="utf-8"))
            cfg.update(data)
        cfg.update(cls._from_env())
        cfg.validate()
        return cfg

    @staticmethod
    def _from_env() -> dict:
        out: dict = {}
        for f in fields(Config):
            if f.name == "extra":
                continue
            raw = os.environ.get(f"PICOART_{f.name.upper()}")
            if raw is None:
                continue
            out[f.name] = raw
        return out

    def update(self, data: dict) -> None:
        types = {f.name: f.type for f in fields(self)}
        for key, value in data.items():
            if key not in types or key == "extra":
                self.extra[key] = value
                continue
            setattr(self, key, _coerce(getattr(self, key), value))

    def validate(self) -> None:
        if self.byte_order not in ("little", "big"):
            raise ValueError("byte_order must be 'little' or 'big'")
        if self.source not in ("shader", "ai", "hybrid"):
            raise ValueError("source must be shader, ai or hybrid")
        if self.temp_source not in ("weather", "cpu", "static", "none"):
            raise ValueError("temp_source must be weather, cpu, static or none")
        if self.width <= 0 or self.height <= 0:
            raise ValueError("width/height must be positive")
        if self.fps <= 0:
            raise ValueError("fps must be positive")
        if self.source in ("ai", "hybrid"):
            self.ai_enabled = True

    @property
    def frame_size(self) -> int:
        return self.width * self.height * 2

    def as_dict(self) -> dict:
        return asdict(self)


def _coerce(current, value):
    """Convert env/json values to the type of the existing default."""
    if isinstance(value, str):
        if isinstance(current, bool):
            return value.strip().lower() in ("1", "true", "yes", "on")
        if isinstance(current, int) and not isinstance(current, bool):
            return int(value)
        if isinstance(current, float):
            return float(value)
        if current is None:
            stripped = value.strip()
            if stripped == "" or stripped.lower() == "none":
                return None
            try:
                return int(stripped)
            except ValueError:
                return stripped
        return value
    if isinstance(current, float) and isinstance(value, int):
        return float(value)
    return value

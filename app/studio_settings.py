from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from pathlib import Path

DVI_MODES = ("640x480", "800x480")
SETTINGS_PATH = Path(__file__).with_name("settings.json")


@dataclass(slots=True)
class StudioSettings:
    dvi_mode: str = "640x480"
    dvi_invert_diffpairs: int = 1

    def validate(self) -> None:
        if self.dvi_mode not in DVI_MODES:
            raise ValueError(f"dvi_mode must be one of {list(DVI_MODES)}")
        if self.dvi_invert_diffpairs not in (0, 1):
            raise ValueError("dvi_invert_diffpairs must be 0 or 1")

    def update(self, values: dict[str, object]) -> None:
        if "dvi_mode" in values:
            self.dvi_mode = str(values["dvi_mode"])
        if "dvi_invert_diffpairs" in values:
            self.dvi_invert_diffpairs = int(values["dvi_invert_diffpairs"])
        self.validate()

    def as_dict(self) -> dict[str, object]:
        return asdict(self)


def load_settings(path: Path = SETTINGS_PATH) -> StudioSettings:
    settings = StudioSettings()
    if not path.exists():
        return settings
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError("settings file must contain a JSON object")
    settings.update(data)
    return settings


def save_settings(settings: StudioSettings, path: Path = SETTINGS_PATH) -> None:
    settings.validate()
    path.write_text(
        json.dumps(settings.as_dict(), indent=2) + "\n",
        encoding="utf-8",
    )

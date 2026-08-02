from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "app"))

import studio_settings  # noqa: E402


class StudioSettingsTests(unittest.TestCase):
    def test_missing_file_returns_defaults(self):
        with tempfile.TemporaryDirectory() as folder:
            settings = studio_settings.load_settings(Path(folder) / "settings.json")
        self.assertEqual(settings.dvi_mode, "640x480")
        self.assertEqual(settings.dvi_invert_diffpairs, 1)

    def test_round_trip_persists_values(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "settings.json"
            original = studio_settings.StudioSettings(
                dvi_mode="800x480",
                dvi_invert_diffpairs=0,
            )
            studio_settings.save_settings(original, path)
            restored = studio_settings.load_settings(path)
        self.assertEqual(restored.dvi_mode, "800x480")
        self.assertEqual(restored.dvi_invert_diffpairs, 0)

    def test_invalid_mode_is_rejected(self):
        settings = studio_settings.StudioSettings()
        with self.assertRaises(ValueError):
            settings.update({"dvi_mode": "1024x600"})

    def test_invalid_polarity_is_rejected(self):
        settings = studio_settings.StudioSettings()
        with self.assertRaises(ValueError):
            settings.update({"dvi_invert_diffpairs": 2})


if __name__ == "__main__":
    unittest.main(verbosity=2)

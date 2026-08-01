"""OTA tests for the Pico firmware, running on desktop Python.

MicroPython-only modules (`machine`, `urequests`) and the device `config` are
stubbed, so the real staging / rollback / blocked-version logic is exercised
against a temporary filesystem.
"""

from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import types
import unittest
from pathlib import Path

FIRMWARE = Path(__file__).resolve().parents[1] / "pico_firmware"


class ResetCalled(Exception):
    """Raised by the stubbed machine.reset() to stop execution like a real reboot."""


class FakeResponse:
    def __init__(self, text: str):
        self.text = text
        self.closed = False

    def json(self):
        return json.loads(self.text)

    def close(self):
        self.closed = True


class FakeGitHub:
    """Serves the files of a fake repository over the stubbed urequests."""

    def __init__(self):
        self.files: dict[str, str] = {}
        self.fail_on: set[str] = set()
        self.requests: list[str] = []
        self.headers: list[dict] = []

    def get(self, url, timeout=15, headers=None):
        name = url.split("/")[-1].split("?")[0]
        self.requests.append(name)
        self.headers.append(headers or {})
        if name in self.fail_on:
            raise OSError("network down for " + name)
        if name not in self.files:
            return FakeResponse("404: Not Found")
        return FakeResponse(self.files[name])


class OtaTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="picoota-"))
        self.cwd = Path.cwd()
        os.chdir(self.tmp)

        self.github = FakeGitHub()

        machine = types.ModuleType("machine")
        machine.reset = self._reset
        self.reset_count = 0

        urequests = types.ModuleType("urequests")
        urequests.get = self.github.get

        config = types.ModuleType("config")
        config.RAW_BASE = "https://raw.githubusercontent.com/u/r/main/pico_firmware/"
        config.WIFI_SSID = "ssid"
        config.WIFI_PASS = "pass"
        config.WIFI_TIMEOUT_S = 1
        config.WIFI_COUNTRY = ""
        config.OTA_ON_BOOT = True

        self._saved = {name: sys.modules.get(name) for name in ("machine", "urequests", "config", "ota")}
        sys.modules["machine"] = machine
        sys.modules["urequests"] = urequests
        sys.modules["config"] = config
        sys.modules.pop("ota", None)

        sys.path.insert(0, str(FIRMWARE))
        import ota  # noqa: PLC0415

        self.ota = ota

        # Device starts on v1 with the shipped files.
        for name in ("main.py", "ota.py", "display_driver.py", "boot.py"):
            Path(name).write_text(f"# firmware v1 {name}\nVALUE = 1\n")
        Path("version.json").write_text(
            json.dumps({"version": 1, "files": ["main.py", "ota.py", "display_driver.py", "boot.py"]})
        )

    def tearDown(self):
        sys.path.remove(str(FIRMWARE))
        for name, module in self._saved.items():
            if module is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = module
        os.chdir(self.cwd)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _reset(self):
        self.reset_count += 1
        raise ResetCalled()

    def publish(self, version: int, body: str = "# firmware v{v}\nVALUE = {v}\n"):
        files = ["main.py", "ota.py", "display_driver.py", "boot.py"]
        self.github.files = {"version.json": json.dumps({"version": version, "files": files})}
        for name in files:
            self.github.files[name] = body.format(v=version)

    # ------------------------------------------------------------------ tests
    def test_no_update_when_versions_match(self):
        self.publish(1)
        self.assertFalse(self.ota.check_and_update())
        self.assertEqual(self.reset_count, 0)
        self.assertEqual(Path("main.py").read_text(), "# firmware v1 main.py\nVALUE = 1\n")

    def test_requests_defeat_the_cdn_cache(self):
        self.publish(1)
        self.ota.check_and_update()
        self.assertTrue(self.github.headers, "no request was made")
        for sent in self.github.headers:
            self.assertEqual(sent.get("Cache-Control"), "no-cache")

    def test_update_stages_backs_up_and_resets(self):
        self.publish(2)
        with self.assertRaises(ResetCalled):
            self.ota.check_and_update()
        self.assertEqual(self.reset_count, 1)
        self.assertIn("VALUE = 2", Path("main.py").read_text())
        self.assertIn("VALUE = 1", Path("main.py.bak").read_text())
        self.assertTrue(Path(self.ota.PENDING_FLAG).exists())
        self.assertEqual(self.ota.local_version(), 2)
        self.assertFalse(Path("main.py.new").exists())

    def test_config_is_never_overwritten(self):
        Path("config.py").write_text("WIFI_SSID = 'mine'\n")
        Path("device_secrets.py").write_text("WIFI_PASS = 'mine'\n")
        self.publish(2)
        self.github.files["config.py"] = "WIFI_SSID = 'stolen'\n"
        self.github.files["device_secrets.py"] = "WIFI_PASS = 'stolen'\n"
        data = json.loads(self.github.files["version.json"])
        data["files"] += ["config.py", "device_secrets.py"]
        self.github.files["version.json"] = json.dumps(data)
        with self.assertRaises(ResetCalled):
            self.ota.check_and_update()
        self.assertEqual(Path("config.py").read_text(), "WIFI_SSID = 'mine'\n")
        self.assertEqual(Path("device_secrets.py").read_text(), "WIFI_PASS = 'mine'\n")

    def test_failed_download_leaves_firmware_untouched(self):
        self.publish(2)
        self.github.fail_on.add("display_driver.py")
        self.assertFalse(self.ota.check_and_update())
        self.assertEqual(self.reset_count, 0)
        self.assertIn("VALUE = 1", Path("main.py").read_text())
        self.assertEqual(self.ota.local_version(), 1)
        self.assertFalse(Path("main.py.new").exists())

    def test_html_error_page_is_rejected(self):
        self.publish(2)
        self.github.files["ota.py"] = "404: Not Found"
        self.assertFalse(self.ota.check_and_update())
        self.assertIn("VALUE = 1", Path("main.py").read_text())

    def test_mark_boot_ok_clears_pending_and_backups(self):
        self.publish(2)
        with self.assertRaises(ResetCalled):
            self.ota.check_and_update()
        self.ota.mark_boot_ok()
        self.assertFalse(Path(self.ota.PENDING_FLAG).exists())
        self.assertFalse(Path("main.py.bak").exists())

    def test_rollback_restores_previous_firmware(self):
        self.publish(2)
        with self.assertRaises(ResetCalled):
            self.ota.check_and_update()
        # Simulate a crash before mark_boot_ok(): next boot must roll back.
        with self.assertRaises(ResetCalled):
            self.ota.rollback_if_needed()
        self.assertIn("VALUE = 1", Path("main.py").read_text())
        self.assertEqual(self.ota.local_version(), 1)
        self.assertEqual(self.ota.blocked_version(), 2)
        self.assertFalse(Path(self.ota.PENDING_FLAG).exists())

    def test_blocked_version_is_not_reinstalled(self):
        self.publish(2)
        with self.assertRaises(ResetCalled):
            self.ota.check_and_update()
        with self.assertRaises(ResetCalled):
            self.ota.rollback_if_needed()
        self.assertFalse(self.ota.check_and_update())  # v2 stays blocked
        self.assertIn("VALUE = 1", Path("main.py").read_text())

        self.publish(3)  # a fix installs normally
        with self.assertRaises(ResetCalled):
            self.ota.check_and_update()
        self.assertIn("VALUE = 3", Path("main.py").read_text())

    def test_rollback_is_a_noop_without_pending_flag(self):
        self.assertFalse(self.ota.rollback_if_needed())
        self.assertEqual(self.reset_count, 0)

    def test_version_check_survives_network_failure(self):
        self.github.fail_on.add("version.json")
        self.assertFalse(self.ota.check_and_update())
        self.assertEqual(self.reset_count, 0)

    def test_publish_landing_mid_download_is_rejected(self):
        """A newer push arriving while we download must not mix file versions."""
        self.publish(2)
        original_get = self.github.get
        state = {"seen": 0}

        def racing_get(url, timeout=15, headers=None):
            name = url.split("/")[-1].split("?")[0]
            if name == "version.json":
                state["seen"] += 1
                if state["seen"] > 1:  # the re-check sees v3 already published
                    return FakeResponse(json.dumps({"version": 3, "files": []}))
            return original_get(url, timeout, headers)

        self.github.get = racing_get
        sys.modules["urequests"].get = racing_get

        self.assertFalse(self.ota.check_and_update())
        self.assertEqual(self.reset_count, 0)
        self.assertIn("VALUE = 1", Path("main.py").read_text())
        self.assertFalse(Path("main.py.new").exists())
        self.assertEqual(self.ota.local_version(), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)

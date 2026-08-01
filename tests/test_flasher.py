"""Tests for tools/flash_pico.py - the automated USB provisioner.

The dangerous paths (erasing a board) are exercised only through the decision
functions, never against real hardware.
"""

import argparse
import io
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import flash_pico  # noqa: E402


def board(state: str, port: str = "COM9") -> flash_pico.Board:
    item = flash_pico.Board(port)
    item.state = state
    return item


class TargetChoiceTests(unittest.TestCase):
    def test_foreign_board_is_refused_without_force(self):
        boards = [board(flash_pico.FOREIGN, "COM5")]
        with mock.patch("sys.stdout", new=io.StringIO()):
            self.assertIsNone(flash_pico.choose_target(boards, force=False))

    def test_foreign_board_is_allowed_with_force(self):
        boards = [board(flash_pico.FOREIGN, "COM5")]
        with mock.patch("sys.stdout", new=io.StringIO()):
            chosen = flash_pico.choose_target(boards, force=True)
        self.assertIsNotNone(chosen)
        self.assertEqual(chosen.port, "COM5")

    def test_bootsel_is_preferred_over_micropython(self):
        boards = [
            board(flash_pico.MICROPYTHON, "COM7"),
            board(flash_pico.BOOTSEL, "E:/"),
        ]
        self.assertEqual(flash_pico.choose_target(boards, force=False).port, "E:/")

    def test_foreign_board_never_wins_over_a_safe_one(self):
        boards = [
            board(flash_pico.FOREIGN, "COM5"),
            board(flash_pico.MICROPYTHON, "COM7"),
        ]
        self.assertEqual(flash_pico.choose_target(boards, force=False).port, "COM7")

    def test_no_boards_means_no_target(self):
        self.assertIsNone(flash_pico.choose_target([], force=True))


class UnattendedTests(unittest.TestCase):
    def test_ask_never_blocks_in_auto_mode(self):
        with mock.patch("builtins.input", side_effect=AssertionError("must not prompt")):
            self.assertEqual(flash_pico.ask("SSID", "fallback", auto=True), "fallback")

    def test_ask_prompts_when_interactive(self):
        with mock.patch("builtins.input", return_value="typed"):
            self.assertEqual(flash_pico.ask("SSID", "fallback", auto=False), "typed")

    def test_ask_falls_back_on_empty_input(self):
        with mock.patch("builtins.input", return_value="   "):
            self.assertEqual(flash_pico.ask("SSID", "fallback", auto=False), "fallback")

    def test_auto_run_with_no_boards_exits_zero(self):
        args = argparse.Namespace(list=False, dry_run=True, port=None, force=False,
                                  no_reboot=True, auto=True, watch=None)
        with mock.patch.object(flash_pico, "discover", return_value=[]), \
                mock.patch("sys.stdout", new=io.StringIO()):
            self.assertEqual(flash_pico.provision_once(args), 0)

    def test_interactive_run_with_no_boards_signals_failure(self):
        args = argparse.Namespace(list=False, dry_run=True, port=None, force=False,
                                  no_reboot=True, auto=False, watch=None)
        with mock.patch.object(flash_pico, "discover", return_value=[]), \
                mock.patch("sys.stdout", new=io.StringIO()):
            self.assertEqual(flash_pico.provision_once(args), 1)

    def test_auto_run_against_foreign_firmware_changes_nothing(self):
        args = argparse.Namespace(list=False, dry_run=False, port=None, force=False,
                                  no_reboot=True, auto=True, watch=None)
        copied = mock.Mock()
        with mock.patch.object(flash_pico, "discover", return_value=[board(flash_pico.FOREIGN)]), \
                mock.patch.object(flash_pico, "copy_firmware", copied), \
                mock.patch.object(flash_pico, "install_micropython", copied), \
                mock.patch("sys.stdout", new=io.StringIO()):
            self.assertEqual(flash_pico.provision_once(args), 0)
        copied.assert_not_called()

    def test_current_firmware_is_not_reflashed(self):
        args = argparse.Namespace(list=False, dry_run=False, port=None, force=False,
                                  no_reboot=True, auto=True, watch=None)
        copied = mock.Mock()
        with mock.patch.object(flash_pico, "discover",
                               return_value=[board(flash_pico.MICROPYTHON, "COM7")]), \
                mock.patch.object(flash_pico, "already_provisioned", return_value=True), \
                mock.patch.object(flash_pico, "copy_firmware", copied), \
                mock.patch("sys.stdout", new=io.StringIO()):
            self.assertEqual(flash_pico.provision_once(args), 0)
        copied.assert_not_called()


class SecretsTests(unittest.TestCase):
    def test_server_ip_is_rewritten_to_this_pc(self):
        import tempfile

        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "device_secrets.py"
            path.write_text(
                'WIFI_SSID = "net"\nSERVER_IP = "10.0.0.1"\nSERVER_PORT = 5001\n',
                encoding="utf-8",
            )
            with mock.patch.object(flash_pico, "SECRETS", path), \
                    mock.patch.object(flash_pico, "lan_address", return_value="192.168.2.44"), \
                    mock.patch("sys.stdout", new=io.StringIO()):
                flash_pico.refresh_server_ip()
            text = path.read_text(encoding="utf-8")
        self.assertIn('SERVER_IP = "192.168.2.44"', text)
        self.assertIn('WIFI_SSID = "net"', text)
        self.assertNotIn("10.0.0.1", text)

    def test_existing_secrets_are_never_overwritten(self):
        import tempfile

        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "device_secrets.py"
            path.write_text('WIFI_PASS = "keep-me"\n', encoding="utf-8")
            with mock.patch.object(flash_pico, "SECRETS", path), \
                    mock.patch("sys.stdout", new=io.StringIO()):
                flash_pico.write_secrets(dry_run=False, auto=True)
            self.assertIn("keep-me", path.read_text(encoding="utf-8"))

    def test_auto_mode_refuses_to_invent_credentials(self):
        import tempfile

        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "device_secrets.py"
            with mock.patch.object(flash_pico, "SECRETS", path), \
                    mock.patch.dict("os.environ", {"PICO_WIFI_SSID": ""}, clear=False), \
                    mock.patch("sys.stdout", new=io.StringIO()):
                flash_pico.write_secrets(dry_run=False, auto=True)
            self.assertFalse(path.exists())

    def test_auto_mode_uses_environment_credentials(self):
        import tempfile

        env = {"PICO_WIFI_SSID": "Antwerp Ionity", "PICO_WIFI_PASS": "hunter2",
               "PICO_SERVER_IP": "192.168.5.9"}
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "device_secrets.py"
            with mock.patch.object(flash_pico, "SECRETS", path), \
                    mock.patch.dict("os.environ", env, clear=False), \
                    mock.patch("sys.stdout", new=io.StringIO()):
                flash_pico.write_secrets(dry_run=False, auto=True)
            text = path.read_text(encoding="utf-8")
        self.assertIn("'Antwerp Ionity'", text)
        self.assertIn("'hunter2'", text)
        self.assertIn("'192.168.5.9'", text)
        self.assertIn("SERVER_PORT = 5001", text)


class Uf2Tests(unittest.TestCase):
    PAGE = b'''
      <a href="/resources/firmware/RPI_PICO_W-20250101-v1.99.0.uf2">latest</a>
      <a href="/resources/firmware/RPI_PICO_W-20250201-v1.99.9-preview.uf2">preview</a>
    '''

    def test_stable_build_is_preferred_over_preview(self):
        response = mock.MagicMock()
        response.read.return_value = self.PAGE
        response.__enter__.return_value = response
        with mock.patch("urllib.request.urlopen", return_value=response):
            url = flash_pico.latest_uf2_url()
        self.assertEqual(
            url, "https://micropython.org/resources/firmware/RPI_PICO_W-20250101-v1.99.0.uf2"
        )

    def test_missing_link_raises(self):
        response = mock.MagicMock()
        response.read.return_value = b"<html>nothing here</html>"
        response.__enter__.return_value = response
        with mock.patch("urllib.request.urlopen", return_value=response):
            with self.assertRaises(RuntimeError):
                flash_pico.latest_uf2_url()


class ManifestTests(unittest.TestCase):
    def test_every_firmware_file_the_installer_copies_exists(self):
        for name in flash_pico.FIRMWARE_FILES:
            self.assertTrue((flash_pico.FIRMWARE_DIR / name).is_file(), name)

    def test_installer_does_not_copy_the_example_secrets(self):
        self.assertNotIn("device_secrets.example.py", flash_pico.FIRMWARE_FILES)


if __name__ == "__main__":
    unittest.main()

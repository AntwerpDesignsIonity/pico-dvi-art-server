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
        with mock.patch.object(flash_pico, "reboot_to_bootsel", return_value=Path("H:/")), \
                mock.patch("sys.stdout", new=io.StringIO()):
            chosen = flash_pico.choose_target(boards, force=True)
        self.assertIsNotNone(chosen)
        self.assertEqual(chosen.state, flash_pico.BOOTSEL)

    def test_force_gives_up_when_the_board_will_not_enter_bootsel(self):
        boards = [board(flash_pico.FOREIGN, "COM5")]
        with mock.patch.object(flash_pico, "reboot_to_bootsel", return_value=None), \
                mock.patch("sys.stdout", new=io.StringIO()):
            self.assertIsNone(flash_pico.choose_target(boards, force=True))

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
            url = flash_pico.latest_uf2_url("RPI_PICO_W")
        self.assertEqual(
            url, "https://micropython.org/resources/firmware/RPI_PICO_W-20250101-v1.99.0.uf2"
        )

    def test_riscv_variant_is_skipped_for_rp2350(self):
        page = (b'<a href="/resources/firmware/RPI_PICO2_W-20260406-v1.28.0-riscv.uf2">r</a>'
                b'<a href="/resources/firmware/RPI_PICO2_W-20260406-v1.28.0.uf2">arm</a>')
        response = mock.MagicMock()
        response.read.return_value = page
        response.__enter__.return_value = response
        with mock.patch("urllib.request.urlopen", return_value=response):
            url = flash_pico.latest_uf2_url("RPI_PICO2_W")
        self.assertTrue(url.endswith("RPI_PICO2_W-20260406-v1.28.0.uf2"))

    def test_missing_link_raises(self):
        response = mock.MagicMock()
        response.read.return_value = b"<html>nothing here</html>"
        response.__enter__.return_value = response
        with mock.patch("urllib.request.urlopen", return_value=response):
            with self.assertRaises(RuntimeError):
                flash_pico.latest_uf2_url("RPI_PICO_W")


class ChipDetectionTests(unittest.TestCase):
    """The connected panel board is an RP2350; the RP2040 build would be wrong."""

    def _drive(self, info: str) -> Path:
        import tempfile

        folder = Path(tempfile.mkdtemp())
        (folder / "INFO_UF2.TXT").write_text(info, encoding="utf-8")
        self.addCleanup(lambda: __import__("shutil").rmtree(folder, ignore_errors=True))
        return folder

    def test_rp2350_maps_to_the_pico_2_w_build(self):
        drive = self._drive("UF2 Bootloader v1.0\nModel: Raspberry Pi RP2350\nBoard-ID: RP2350\n")
        self.assertEqual(flash_pico.board_from_drive(drive), "RPI_PICO2_W")

    def test_rp2040_maps_to_the_pico_w_build(self):
        drive = self._drive("UF2 Bootloader v3.0\nModel: Raspberry Pi RP2\nBoard-ID: RP2040\n")
        self.assertEqual(flash_pico.board_from_drive(drive), "RPI_PICO_W")

    def test_unreadable_drive_falls_back(self):
        import tempfile

        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(flash_pico.board_from_drive(Path(folder)), flash_pico.DEFAULT_BOARD)


class CopyOrderTests(unittest.TestCase):
    """Regression: a half-provisioned board locked itself out of the REPL.

    boot.py must land last - once it exists the board can reset into firmware
    that blocks on Wi-Fi, stranding any file not yet copied. And the whole copy
    has to be one mpremote invocation, because each one re-enters the REPL.
    """

    def setUp(self):
        # copy_firmware() checks the local files exist before touching the
        # board. device_secrets.py is git-ignored, so on a fresh clone it is
        # absent - point the flasher at a fully populated stand-in directory
        # to keep these tests hermetic.
        import tempfile

        folder = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(folder, ignore_errors=True))
        for name in flash_pico.FIRMWARE_FILES + ["device_secrets.py"]:
            (folder / name).write_text("# stub\n", encoding="utf-8")
        patcher_dir = mock.patch.object(flash_pico, "FIRMWARE_DIR", folder)
        patcher_secrets = mock.patch.object(flash_pico, "SECRETS", folder / "device_secrets.py")
        patcher_dir.start()
        patcher_secrets.start()
        self.addCleanup(patcher_dir.stop)
        self.addCleanup(patcher_secrets.stop)

    def _capture(self):
        calls = []

        def fake_mp(port, *args, **kwargs):
            calls.append(args)
            result = mock.Mock()
            result.returncode = 0
            result.stdout = "\n".join(flash_pico.FIRMWARE_FILES + [flash_pico.SECRETS.name])
            result.stderr = ""
            return result

        return calls, fake_mp

    def test_boot_py_is_written_last_and_secrets_first(self):
        calls, fake_mp = self._capture()
        with mock.patch.object(flash_pico, "mp", fake_mp), \
                mock.patch("sys.stdout", new=io.StringIO()):
            self.assertTrue(flash_pico.copy_firmware("COM9"))

        copy = next(c for c in calls if c[0] == "fs" and c[1] == "cp")
        names = [Path(a).name for a in copy[2:-1]]
        self.assertEqual(names[0], "device_secrets.py")
        self.assertEqual(names[-1], "boot.py")
        self.assertEqual(copy[-1], ":")

    def test_all_files_go_in_a_single_invocation(self):
        calls, fake_mp = self._capture()
        with mock.patch.object(flash_pico, "mp", fake_mp), \
                mock.patch("sys.stdout", new=io.StringIO()):
            flash_pico.copy_firmware("COM9")
        self.assertEqual(len([c for c in calls if c[:2] == ("fs", "cp")]), 1)

    def test_every_required_file_is_included_exactly_once(self):
        calls, fake_mp = self._capture()
        with mock.patch.object(flash_pico, "mp", fake_mp), \
                mock.patch("sys.stdout", new=io.StringIO()):
            flash_pico.copy_firmware("COM9")
        copy = next(c for c in calls if c[:2] == ("fs", "cp"))
        names = [Path(a).name for a in copy[2:-1]]
        expected = set(flash_pico.FIRMWARE_FILES) | {flash_pico.SECRETS.name}
        self.assertEqual(sorted(names), sorted(expected))
        self.assertEqual(len(names), len(set(names)))

    def test_missing_file_aborts_before_touching_the_board(self):
        calls, fake_mp = self._capture()
        with mock.patch.object(flash_pico, "FIRMWARE_FILES", ["nope.py"]), \
                mock.patch.object(flash_pico, "mp", fake_mp), \
                mock.patch("sys.stdout", new=io.StringIO()):
            self.assertFalse(flash_pico.copy_firmware("COM9"))
        self.assertEqual(calls, [])


class ResumeTests(unittest.TestCase):
    """Regression: plain `mpremote connect` soft-resets and re-blocks the board."""

    def test_resume_is_tried_first(self):
        seen = []

        def fake(*args, **kwargs):
            seen.append(args)
            result = mock.Mock()
            result.returncode = 0
            result.stdout = ""
            return result

        with mock.patch.object(flash_pico, "mpremote", fake):
            flash_pico.mp("COM9", "fs", "ls")
        self.assertEqual(seen[0][:3], ("connect", "COM9", "resume"))
        self.assertEqual(len(seen), 1)

    def test_plain_connect_is_the_fallback(self):
        seen = []

        def fake(*args, **kwargs):
            seen.append(args)
            result = mock.Mock()
            result.returncode = 1 if "resume" in args else 0
            result.stdout = ""
            return result

        with mock.patch.object(flash_pico, "mpremote", fake):
            result = flash_pico.mp("COM9", "fs", "ls")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(len(seen), 2)
        self.assertNotIn("resume", seen[1])


class ManifestTests(unittest.TestCase):
    def test_every_firmware_file_the_installer_copies_exists(self):
        for name in flash_pico.FIRMWARE_FILES:
            self.assertTrue((flash_pico.FIRMWARE_DIR / name).is_file(), name)

    def test_installer_does_not_copy_the_example_secrets(self):
        self.assertNotIn("device_secrets.example.py", flash_pico.FIRMWARE_FILES)


class VersionTests(unittest.TestCase):
    """Regression: a failed exec used to look like 'no version' and reflash forever."""

    def _mp(self, returncode: int, stdout: str):
        def fake(port, *args, **kwargs):
            result = mock.Mock()
            result.returncode = returncode
            result.stdout = stdout
            result.stderr = ""
            return result

        return fake

    def test_version_is_parsed_from_the_board(self):
        with mock.patch.object(flash_pico, "mp", self._mp(0, "V=3\n")):
            self.assertEqual(flash_pico.board_firmware_version("COM9"), "3")

    def test_failed_exec_reports_unknown(self):
        with mock.patch.object(flash_pico, "mp", self._mp(1, "")):
            self.assertIsNone(flash_pico.board_firmware_version("COM9"))

    def test_absent_version_file_reports_unknown(self):
        with mock.patch.object(flash_pico, "mp", self._mp(0, "V=\n")):
            self.assertIsNone(flash_pico.board_firmware_version("COM9"))

    def test_matching_version_counts_as_provisioned(self):
        listing = mock.Mock(returncode=0, stdout="main.py device_secrets.py", stderr="")
        with mock.patch.object(flash_pico, "mp", return_value=listing), \
                mock.patch.object(flash_pico, "local_firmware_version", return_value="3"), \
                mock.patch.object(flash_pico, "board_firmware_version", return_value="3"):
            self.assertTrue(flash_pico.already_provisioned("COM9"))

    def test_different_version_triggers_a_reflash(self):
        listing = mock.Mock(returncode=0, stdout="main.py device_secrets.py", stderr="")
        with mock.patch.object(flash_pico, "mp", return_value=listing), \
                mock.patch.object(flash_pico, "local_firmware_version", return_value="4"), \
                mock.patch.object(flash_pico, "board_firmware_version", return_value="3"):
            self.assertFalse(flash_pico.already_provisioned("COM9"))

    def test_board_without_credentials_is_not_provisioned(self):
        listing = mock.Mock(returncode=0, stdout="main.py boot.py", stderr="")
        with mock.patch.object(flash_pico, "mp", return_value=listing):
            self.assertFalse(flash_pico.already_provisioned("COM9"))


if __name__ == "__main__":
    unittest.main()

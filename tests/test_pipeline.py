"""Pipeline tests: font, packing, shaders, HUD, config and the live socket.

    python -m unittest discover -s tests -v
"""

from __future__ import annotations

import socket
import struct
import sys
import threading
import time
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "pc_server"))

import font5x7  # noqa: E402
import math_shaders  # noqa: E402
import server as art_server  # noqa: E402
from config import Config  # noqa: E402
from hud import HudState, render_centered_hud, render_hud  # noqa: E402
from pixels import HEADER_SIZE, MAGIC, from_rgb565, to_rgb565  # noqa: E402


class FontTests(unittest.TestCase):
    def test_glyph_geometry(self):
        for char, glyph in font5x7.GLYPHS.items():
            self.assertEqual(glyph.shape, (7, 5), f"bad glyph for {char!r}")

    def test_text_size_matches_mask(self):
        for text, scale in (("12:34:56", 3), ("2026-08-01 SAT", 2), ("MCU 38.4\u00b0C", 2)):
            mask = font5x7.text_mask(text, scale=scale)
            width, height = font5x7.text_size(text, scale=scale)
            self.assertEqual(mask.shape, (height, width))
            self.assertTrue(mask.max() == 1, "glyphs should light some pixels")

    def test_unknown_char_falls_back(self):
        self.assertTrue(np.array_equal(font5x7.glyph("\u00a9"), font5x7.GLYPHS["?"]))

    def test_hud_text_fits_the_panel(self):
        cfg = Config()
        widest = max(
            font5x7.text_size("23:59:59", cfg.hud_scale_clock)[0],
            font5x7.text_size("2026-12-31 WED", cfg.hud_scale_small)[0],
            font5x7.text_size("MCU -10.0\u00b0C", cfg.hud_scale_small)[0],
        )
        self.assertLess(widest + 2 * cfg.hud_margin, cfg.width)


class PixelTests(unittest.TestCase):
    def test_round_trip_is_close(self):
        rng = np.random.default_rng(4)
        rgb = rng.integers(0, 256, size=(240, 400, 3), dtype=np.uint8)
        for order in ("little", "big"):
            packed = to_rgb565(rgb, order)
            self.assertEqual(len(packed), 400 * 240 * 2)
            back = from_rgb565(packed, 400, 240, order)
            self.assertLessEqual(int(np.abs(back.astype(int) - rgb.astype(int)).max()), 8)

    def test_known_colours(self):
        rgb = np.array([[[255, 0, 0], [0, 255, 0], [0, 0, 255], [0, 0, 0]]], dtype=np.uint8)
        words = np.frombuffer(to_rgb565(rgb, "little"), dtype="<u2")
        self.assertEqual(list(words), [0xF800, 0x07E0, 0x001F, 0x0000])

    def test_endianness_differs(self):
        rgb = np.array([[[255, 128, 0]]], dtype=np.uint8)
        self.assertNotEqual(to_rgb565(rgb, "little"), to_rgb565(rgb, "big"))

    def test_rejects_bad_input(self):
        with self.assertRaises(ValueError):
            to_rgb565(np.zeros((4, 4), dtype=np.uint8))


class ShaderTests(unittest.TestCase):
    def setUp(self):
        self.engine = math_shaders.InfiniteArtEngine(400, 240, seed=99)

    def test_frame_shape_and_type(self):
        frame = self.engine.render(1.25)
        self.assertEqual(frame.shape, (240, 400, 3))
        self.assertEqual(frame.dtype, np.uint8)

    def test_deterministic_for_a_seed(self):
        twin = math_shaders.InfiniteArtEngine(400, 240, seed=99)
        self.assertTrue(np.array_equal(self.engine.render(2.0), twin.render(2.0)))

    def test_never_repeats_over_time(self):
        base = self.engine.render(0.0).astype(np.int16)
        for t in (5.0, 50.0, 500.0, 5000.0, 50000.0):
            other = self.engine.render(t).astype(np.int16)
            self.assertGreater(np.abs(base - other).mean(), 3.0, f"frame at t={t} looks repeated")

    def test_seeds_differ(self):
        other = math_shaders.InfiniteArtEngine(400, 240, seed=1234)
        difference = np.abs(
            self.engine.render(3.0).astype(np.int16) - other.render(3.0).astype(np.int16)
        ).mean()
        self.assertGreater(difference, 3.0)

    def test_hsv_matches_colorsys(self):
        import colorsys

        for h, s, v in ((0.0, 1.0, 1.0), (0.33, 0.5, 0.8), (0.75, 0.2, 0.4), (0.99, 1.0, 0.1)):
            expected = np.array(colorsys.hsv_to_rgb(h, s, v))
            actual = math_shaders.hsv_to_rgb(
                np.array([h]), np.array([s]), np.array([v])
            )[0]
            self.assertTrue(np.allclose(actual, expected, atol=1e-5), f"{actual} != {expected}")

    def test_border_only_touches_the_edges(self):
        frame = np.zeros((240, 400, 3), dtype=np.uint8)
        math_shaders.swirling_frame(frame, 1.0, thickness=8)
        self.assertGreater(frame[:8].max(), 0)
        self.assertGreater(frame[-8:].max(), 0)
        self.assertGreater(frame[:, :8].max(), 0)
        self.assertGreater(frame[:, -8:].max(), 0)
        self.assertEqual(frame[8:-8, 8:-8].max(), 0)

    def test_border_animates(self):
        a = np.zeros((240, 400, 3), dtype=np.uint8)
        b = np.zeros((240, 400, 3), dtype=np.uint8)
        math_shaders.swirling_frame(a, 0.0, thickness=8)
        math_shaders.swirling_frame(b, 4.0, thickness=8)
        self.assertFalse(np.array_equal(a, b))

    def test_border_thickness_zero_is_a_noop(self):
        frame = np.zeros((240, 400, 3), dtype=np.uint8)
        math_shaders.swirling_frame(frame, 1.0, thickness=0)
        self.assertEqual(frame.max(), 0)


class RetroArcadeTests(unittest.TestCase):
    def test_cycles_through_five_distinct_pixel_scenes(self):
        engine = math_shaders.RetroArcadeEngine(320, 240, seed=99)
        frames = [
            engine.render(index * engine.SCENE_SECONDS + 1.0)
            for index in range(5)
        ]
        self.assertTrue(all(frame.shape == (240, 320, 3) for frame in frames))
        self.assertTrue(
            all(not np.array_equal(frames[i], frames[(i + 1) % 5]) for i in range(5))
        )
        self.assertTrue(all(np.array_equal(frame[0::2], frame[1::2]) for frame in frames))


class HudTests(unittest.TestCase):
    def setUp(self):
        self.cfg = Config()
        self.now = __import__("datetime").datetime(2026, 8, 1, 4, 5, 6)

    def render(self, **kwargs):
        frame = np.zeros((self.cfg.height, self.cfg.width, 3), dtype=np.uint8)
        state = HudState(now=self.now, **kwargs)
        return render_hud(frame, state, self.cfg)

    def test_draws_in_the_top_right(self):
        frame = self.render(server_temp_c=17.6, local_temp_c=38.4)
        top_right = frame[0:130, self.cfg.width // 2 :]
        top_left = frame[0:130, : self.cfg.width // 3]
        bottom = frame[160:, :]
        self.assertGreater(top_right.max(), 0)
        self.assertEqual(top_left.max(), 0)
        self.assertEqual(bottom.max(), 0)

    def test_missing_temperatures_render_placeholders(self):
        frame = self.render()
        self.assertGreater(frame.max(), 0)

    def test_clock_changes_every_second(self):
        first = self.render(server_temp_c=1.0, local_temp_c=2.0)
        self.now = self.now.replace(second=self.now.second + 1)
        second = self.render(server_temp_c=1.0, local_temp_c=2.0)
        self.assertFalse(np.array_equal(first, second))

    def test_twelve_hour_mode(self):
        self.cfg.clock_24h = False
        frame = self.render(server_temp_c=1.0)
        self.assertGreater(frame.max(), 0)

    def test_centered_hud_has_no_panel_or_border(self):
        frame = np.zeros((self.cfg.height, self.cfg.width, 3), dtype=np.uint8)
        render_centered_hud(
            frame,
            HudState(now=self.now, server_temp_c=17.6, local_temp_c=38.4),
            self.cfg,
        )
        self.assertGreater(frame[60:185, self.cfg.width // 4 : -self.cfg.width // 4].max(), 0)
        self.assertEqual(frame[:20, :20].max(), 0)
        yellow = (
            (frame[..., 0] == 255)
            & (frame[..., 1] >= 190)
            & (frame[..., 2] <= 180)
        )
        self.assertGreater(int(yellow.sum()), 20)


class ConfigTests(unittest.TestCase):
    def test_env_coercion(self):
        cfg = Config()
        cfg.update({"port": "6001", "fps": "12.5", "hud": "false", "seed": "77"})
        self.assertEqual(cfg.port, 6001)
        self.assertAlmostEqual(cfg.fps, 12.5)
        self.assertFalse(cfg.hud)
        self.assertEqual(cfg.seed, 77)

    def test_frame_size(self):
        cfg = Config()
        self.assertEqual(cfg.frame_size, cfg.width * cfg.height * 2)
        self.assertEqual(cfg.frame_size, 153600)

    def test_validation(self):
        cfg = Config()
        cfg.byte_order = "middle"
        with self.assertRaises(ValueError):
            cfg.validate()
        cfg.byte_order = "big"
        with self.assertRaises(ValueError):
            cfg.validate()

    def test_ai_source_enables_worker(self):
        cfg = Config()
        cfg.source = "hybrid"
        cfg.validate()
        self.assertTrue(cfg.ai_enabled)

    def test_retro_source_is_valid(self):
        cfg = Config()
        cfg.source = "retro"
        cfg.validate()
        self.assertFalse(cfg.ai_enabled)


class PromptTests(unittest.TestCase):
    def test_prompts_vary(self):
        from ai_prompts import build_prompt

        prompts = {build_prompt()[0] for _ in range(25)}
        self.assertGreater(len(prompts), 20)

    def test_template_has_no_placeholders_left(self):
        from ai_prompts import build_prompt

        text, variables = build_prompt()
        self.assertNotIn("[", text)
        self.assertIn(variables["subject"], text)


class StreamingTests(unittest.TestCase):
    """End-to-end over a real socket, exactly like the Pico sees it."""

    @classmethod
    def setUpClass(cls):
        cfg = Config()
        cfg.update({"port": 0, "host": "127.0.0.1", "temp_source": "static", "fps": 60.0})
        cls.cfg = cfg
        cls.server = art_server.ArtServer(cfg)

        cls.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        cls.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        cls.listener.bind(("127.0.0.1", 0))
        cls.listener.listen(1)
        cls.port = cls.listener.getsockname()[1]

        def accept_loop():
            conn, addr = cls.listener.accept()
            cls.server._client_thread(conn, addr)

        cls.thread = threading.Thread(target=accept_loop, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server._stop.set()
        cls.listener.close()

    @staticmethod
    def recv_exactly(sock, length):
        chunks = []
        while length:
            block = sock.recv(min(65536, length))
            if not block:
                raise ConnectionError("closed")
            chunks.append(block)
            length -= len(block)
        return b"".join(chunks)

    def test_frames_and_telemetry(self):
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=10)
        self.addCleanup(sock.close)
        sock.sendall(b"HELLO 7 unit-test\nTEMP 41.50\n")

        for _ in range(3):
            header = self.recv_exactly(sock, HEADER_SIZE)
            magic, length = header[:4], struct.unpack("<I", header[4:8])[0]
            self.assertEqual(magic, MAGIC)
            self.assertEqual(length, self.cfg.frame_size)
            payload = self.recv_exactly(sock, length)
            rgb = from_rgb565(payload, self.cfg.width, self.cfg.height, self.cfg.byte_order)
            self.assertEqual(rgb.shape, (self.cfg.height, self.cfg.width, 3))
            self.assertGreater(rgb.max(), 0)

        deadline = time.time() + 5
        session = None
        while time.time() < deadline:
            with self.server._clients_lock:
                if self.server._clients:
                    session = self.server._clients[0][0]
            if session and session.snapshot()[0] is not None:
                break
            time.sleep(0.05)

        self.assertIsNotNone(session, "server never registered the client")
        temp, firmware = session.snapshot()
        self.assertAlmostEqual(temp, 41.5, places=2)
        self.assertEqual(firmware, "7")

    def test_command_broadcast(self):
        delivered = self.server.broadcast_command({"cmd": "ota"})
        self.assertGreaterEqual(delivered, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)

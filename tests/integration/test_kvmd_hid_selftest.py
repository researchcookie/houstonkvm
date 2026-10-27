"""scripts/kvmd-hid-selftest.py against a FakeBoard (see test_kvmd_hid.py):
that it passes a healthy board, blames the right thing on an unhealthy one,
and leaves the target as it found it.
"""
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_kvmd_hid import CMD_ABS, CMD_KEY, FakeBoard

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "kvmd-hid-selftest.py"


def run(*args, stdin=""):
    return subprocess.run([sys.executable, str(SCRIPT), *args], input=stdin,
                          capture_output=True, text=True, timeout=120)


class TestBoardCheck(unittest.TestCase):
    def board(self, **kw):
        board = FakeBoard(**kw)
        self.addCleanup(board.close)
        return board

    def test_a_healthy_board_passes_and_is_left_as_found(self):
        board = self.board()
        out = run(board.path, stdin="y\n")
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn("mouse: absolute USB", out.stdout)
        self.assertIn("50/50 answered", out.stdout)
        self.assertIn("switched its Caps Lock light", out.stdout)
        self.assertFalse(board.caps)          # pressed twice: back as it was
        self.assertEqual(board.keys, set())
        self.assertEqual(board.pos, (0, 0))   # the pointer ends in the middle

    def test_a_pointer_that_did_not_follow_fails(self):
        out = run(self.board().path, stdin="n\n")
        self.assertEqual(out.returncode, 1, out.stdout)
        self.assertIn("didn't follow", out.stdout)

    def test_skipping_the_target_tests_touches_nothing(self):
        board = self.board()
        out = run(board.path, "--skip-target-tests")
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertEqual([c for c, _ in board.commands() if c in (CMD_KEY, CMD_ABS)], [])

    def test_a_relative_mouse_fails(self):
        out = run(self.board(mouse=0x10).path, "--skip-target-tests")
        self.assertEqual(out.returncode, 1, out.stdout)
        self.assertIn("mouse: relative USB", out.stdout)

    def test_a_target_that_has_not_recognised_it_fails(self):
        out = run(self.board(enumerated=False).path, "--skip-target-tests")
        self.assertEqual(out.returncode, 1, out.stdout)
        self.assertIn("hasn't recognised the keyboard", out.stdout)

    def test_no_caps_lock_light_is_only_a_warning(self):
        out = run(self.board(leds=False).path, stdin="y\n")
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn("[WARN]", out.stdout)
        self.assertIn("unconfirmed", out.stdout)

    def test_detect_finds_the_firmwares_rate(self):
        board = self.board(baud=57600)
        out = run(board.path, "--detect-baud")
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn("answers at 57600", out.stdout)

    def test_a_board_that_never_answers_points_at_the_wiring(self):
        out = run(self.board(silent=True).path)
        self.assertEqual(out.returncode, 1, out.stdout)
        self.assertIn("GP22", out.stdout)


if __name__ == "__main__":
    unittest.main()

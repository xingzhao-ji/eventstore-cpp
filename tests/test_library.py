import os
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LibraryTests(unittest.TestCase):
    def run_mode(self, mode, inject=False):
        env = dict(os.environ)
        if inject:
            env["LD_PRELOAD"] = str(ROOT / "build/faultio.so")
        result = subprocess.run([str(ROOT / "build/library_tests"), mode],
                                env=env, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_library_bounds_utf8_reader_locks_and_threads(self):
        self.run_mode("normal")

    def test_real_partial_write_limit_rolls_back_and_keeps_id(self):
        self.run_mode("rlimit")

    @unittest.skipUnless(sys.platform.startswith("linux"), "LD_PRELOAD fault injection is Linux-only")
    def test_fsync_failure_rolls_back_durably(self):
        self.run_mode("sync-once", inject=True)

    @unittest.skipUnless(sys.platform.startswith("linux"), "LD_PRELOAD fault injection is Linux-only")
    def test_rollback_fsync_failure_poisons_object(self):
        self.run_mode("sync-always", inject=True)

    @unittest.skipUnless(sys.platform.startswith("linux"), "LD_PRELOAD fault injection is Linux-only")
    def test_rollback_truncate_failure_poisons_object(self):
        self.run_mode("truncate-fail", inject=True)

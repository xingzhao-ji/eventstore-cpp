"""Real-file contract tests; each case targets loss, corruption or wrong results."""
import collections
import json
import os
from pathlib import Path
import random
import select
import signal
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

BIN = Path(__file__).resolve().parents[1] / "build/eventstore"


class StoreTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / "events.evt"

    def tearDown(self):
        self.tmp.cleanup()

    def run_cli(self, *args, ok=True, input=None):
        proc = subprocess.run([str(BIN), *map(str, args)], input=input,
                              capture_output=True, text=True, timeout=10)
        self.assertEqual(proc.returncode == 0, ok, (proc.stdout, proc.stderr))
        return proc

    def append(self, ts=100, category="demo", value=7, payload="hello"):
        return json.loads(self.run_cli("append", self.path, ts, category, value, payload).stdout)

    def query(self, start=-(2**63), end=2**63-1, category=None):
        args = ["query", self.path, start, end]
        if category is not None:
            args.append(category)
        return [json.loads(x) for x in self.run_cli(*args).stdout.splitlines()]

    def test_acknowledged_append_reopen_ids_and_json_text(self):
        self.assertEqual(self.append(payload='line\n"\\\t☃'),
                         {"id": 1, "timestamp": 100, "category": "demo",
                          "value": 7, "payload": 'line\n"\\\t☃'})
        self.append(ts=-4, category="other", value=-(2**63), payload="")
        rows = self.query()
        self.assertEqual([x["id"] for x in rows], [1, 2])
        self.assertEqual(rows[1]["value"], -(2**63))

    def test_invalid_inputs_never_append(self):
        self.append()
        original = self.path.read_bytes()
        for args in [("abc", "x", "1", "p"), ("0", "", "1", "p"),
                     ("0", "x" * 65, "1", "p"), ("0", "x", str(2**63), "p"),
                     ("1garbage", "x", "1", "p")]:
            self.run_cli("append", self.path, *args, ok=False)
            self.assertEqual(self.path.read_bytes(), original)
        self.run_cli("query", self.path, 100, 0, ok=False)

    @unittest.skipUnless(sys.platform.startswith("linux") and Path("/dev/full").exists(),
                         "requires Linux /dev/full")
    def test_counts_reports_buffered_output_failure(self):
        self.append(category="first")
        self.append(category="second")
        before = self.path.read_bytes()
        try:
            sink = open("/dev/full", "wb")
        except OSError as error:
            self.skipTest(f"cannot open /dev/full: {error}")
        with sink:
            proc = subprocess.run([str(BIN), "counts", str(self.path), str(-(2**63)), str(2**63 - 1)],
                                  stdout=sink, stderr=subprocess.PIPE, text=True, timeout=10)
        self.assertEqual(proc.returncode, 1, "buffered output failure must not report success")
        self.assertIn("output failed", proc.stderr)
        self.assertEqual(self.path.read_bytes(), before)

    def test_inclusive_filter_and_group_counts_match_reference(self):
        rng = random.Random(810)
        data = [(rng.randint(-20, 20), rng.choice(["a", "b", "c"]), i, f"event-{i}")
                for i in range(80)]
        text = "".join("\t".join(map(str, row)) + "\n" for row in data)
        acks = self.run_cli("ingest", self.path, input=text).stdout.splitlines()
        self.assertEqual(len(acks), len(data))
        for start, end in [(-5, 5), (0, 0), (-20, 20), (99, 100)]:
            want = [{"id": i + 1, "timestamp": row[0], "category": row[1],
                     "value": row[2], "payload": row[3]}
                    for i, row in enumerate(data) if start <= row[0] <= end]
            self.assertEqual(self.query(start, end), want)
            self.assertEqual(self.query(start, end, "b"), [r for r in want if r["category"] == "b"])
            counts = dict(collections.Counter(r["category"] for r in want))
            actual = [json.loads(x) for x in self.run_cli("counts", self.path, start, end).stdout.splitlines()]
            self.assertEqual(actual, [{"category": k, "count": counts[k]} for k in sorted(counts)])

    def test_every_truncated_record_suffix_recovers_only_last_record(self):
        self.append(payload="first")
        intact = self.path.read_bytes()
        self.append(payload="last")
        complete = self.path.read_bytes()
        tail = complete[len(intact):]
        for n in range(1, len(tail)):
            self.path.write_bytes(intact + tail[:n])
            before = self.path.read_bytes()
            self.run_cli("query", self.path, -1000, 1000, ok=False)
            self.assertEqual(self.path.read_bytes(), before, "reader must not recover")
            result = json.loads(self.run_cli("recover", self.path).stdout)
            self.assertEqual(result["recovered_bytes"], n)
            self.assertEqual(self.path.read_bytes(), intact)
            self.assertEqual(self.append()["id"], 2)

    def test_complete_corruption_fails_closed_even_at_tail(self):
        self.append(payload="first")
        boundary = self.path.stat().st_size
        self.append(payload="second")
        original = self.path.read_bytes()
        # File header, frame length/checksum, interior body and final body.
        for offset in [0, 8, 16, 20, 24, 32, boundary - 1, len(original) - 1]:
            broken = bytearray(original)
            broken[offset] ^= 0x40
            self.path.write_bytes(broken)
            self.run_cli("recover", self.path, ok=False)
            self.assertEqual(self.path.read_bytes(), broken)

    def test_valid_checksum_cannot_bypass_ids_lengths_utf8_or_bounds(self):
        self.append()
        original = self.path.read_bytes()
        # Recompute checksums independently so semantic validation is exercised.
        for mutate in [lambda body: struct.pack_into("<Q", body, 0, 9),
                       lambda body: struct.pack_into("<I", body, 16, 0),
                       lambda body: body.__setitem__(32, 0xff)]:
            body = bytearray(original[32:])
            mutate(body)
            header = b"EVT1" + struct.pack("<II", len(body), zlib.crc32(body))
            frame = header + struct.pack("<I", zlib.crc32(header)) + body
            broken = original[:16] + frame
            self.path.write_bytes(broken)
            self.run_cli("recover", self.path, ok=False)
            self.assertEqual(self.path.read_bytes(), broken)
        header = b"EVT1" + struct.pack("<II", 2**32 - 1, 0)
        broken = original[:16] + header + struct.pack("<I", zlib.crc32(header))
        self.path.write_bytes(broken)
        self.run_cli("recover", self.path, ok=False)
        self.assertEqual(self.path.read_bytes(), broken)

    def test_kill_after_ack_preserves_event_and_releases_lock(self):
        proc = subprocess.Popen([str(BIN), "ingest", str(self.path)], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            proc.stdin.write("100\tcrash\t42\tack survives SIGKILL\n")
            proc.stdin.flush()
            self.assertTrue(select.select([proc.stdout], [], [], 5)[0], "ack timed out")
            line = proc.stdout.readline()
            self.assertTrue(line, "writer exited without an acknowledgment")
            ack = json.loads(line)
            self.assertEqual(ack["id"], 1)
            self.run_cli("append", self.path, 0, "x", 0, "blocked", ok=False)
            self.run_cli("query", self.path, 0, 1000, ok=False)
            proc.kill()
            proc.wait(timeout=5)
            self.assertEqual(self.query(), [ack])
            self.assertEqual(self.append()["id"], 2)
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.communicate(timeout=5)

    def test_partial_initial_header_recovery_and_wrong_header_rejection(self):
        self.append()
        header = self.path.read_bytes()[:16]
        for n in range(16):
            self.path.write_bytes(header[:n])
            self.run_cli("recover", self.path)
            self.assertEqual(self.query(), [])
        self.path.write_bytes(b"WRONG")
        self.run_cli("recover", self.path, ok=False)
        self.assertEqual(self.path.read_bytes(), b"WRONG")

    def test_oversize_or_malformed_ingest_does_not_ack(self):
        self.run_cli("ingest", self.path, input="0\tx\t1\t" + "p" * (1024 * 1024 + 1) + "\n", ok=False)
        self.assertEqual(self.query(), [])
        self.run_cli("ingest", self.path, input="missing fields\n", ok=False)
        self.assertEqual(self.query(), [])

    def test_symlink_and_nonregular_file_refused(self):
        self.append()
        alias = Path(self.tmp.name) / "alias"
        alias.symlink_to(self.path)
        self.run_cli("recover", alias, ok=False)
        self.run_cli("recover", self.tmp.name, ok=False)

    def test_group_limit_is_bounded_and_does_not_return_partial_counts(self):
        # Independent format producer avoids 100001 fsyncs; checksums match zlib.
        header = b"EVTLOG01" + struct.pack("<I", 1)
        with self.path.open("wb") as stream:
            stream.write(header + struct.pack("<I", zlib.crc32(header)))
            for i in range(100001):
                category = f"group-{i}".encode()
                body = struct.pack("<QqIIq", i + 1, i, len(category), 0, 0) + category
                frame = b"EVT1" + struct.pack("<II", len(body), zlib.crc32(body))
                stream.write(frame + struct.pack("<I", zlib.crc32(frame)) + body)
        failed = self.run_cli("counts", self.path, 0, 100000, ok=False)
        self.assertEqual(failed.stdout, "")
        self.assertIn("group count", failed.stderr)
        self.assertEqual(len(self.run_cli("counts", self.path, 0, 0).stdout.splitlines()), 1)

    @unittest.skipUnless(sys.platform.startswith("linux"), "LD_PRELOAD fault injection is Linux-only")
    def test_append_fsync_failure_never_acknowledges(self):
        self.append()
        before = self.path.read_bytes()
        env = dict(os.environ, LD_PRELOAD=str(BIN.parent / "faultio.so"), EVENTSTORE_TEST_FAIL_FSYNC="once")
        proc = subprocess.run([str(BIN), "append", str(self.path), "1", "a", "1", "unacknowledged"],
                              env=env, capture_output=True, text=True, timeout=10)
        self.assertNotEqual(proc.returncode, 0)
        self.assertEqual(proc.stdout, "")
        self.assertEqual(self.path.read_bytes(), before)

    @unittest.skipUnless(sys.platform.startswith("linux"), "LD_PRELOAD fault injection is Linux-only")
    def test_recovery_sync_failure_is_reported(self):
        self.append()
        intact = self.path.read_bytes()
        self.path.write_bytes(intact + b"EVT")
        env = dict(os.environ, LD_PRELOAD=str(BIN.parent / "faultio.so"), EVENTSTORE_TEST_FAIL_FSYNC="always")
        proc = subprocess.run([str(BIN), "recover", str(self.path)], env=env,
                              capture_output=True, text=True, timeout=10)
        self.assertNotEqual(proc.returncode, 0)
        self.assertEqual(proc.stdout, "")
        # Truncation happened, but its durability was not confirmed; later reopen is permitted.
        self.assertEqual(self.path.read_bytes(), intact)
        self.assertEqual(len(self.query()), 1)


if __name__ == "__main__":
    unittest.main()

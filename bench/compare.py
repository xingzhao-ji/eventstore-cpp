#!/usr/bin/env python3
"""Synthetic, deterministic full-scan grouped-count comparison; no speed claim."""
import argparse
import collections
import datetime
import json
import os
from pathlib import Path
import platform
import random
import sqlite3
import statistics
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SQL = "SELECT category, COUNT(*) FROM events WHERE timestamp >= ? AND timestamp <= ? GROUP BY category ORDER BY category"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rows", type=int, default=2000)
    parser.add_argument("--repetitions", type=int, default=7)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not 1 <= args.rows <= 1000000 or not 1 <= args.repetitions <= 1000:
        parser.error("rows must be 1..1000000 and repetitions 1..1000")
    rng = random.Random(20261023)
    rows = [(rng.randint(-1000, 1000), f"category-{rng.randrange(12):02}", i,
             f"synthetic-event-{i}:" + "x" * 48) for i in range(args.rows)]
    data = "".join("\t".join(map(str, row)) + "\n" for row in rows)
    with tempfile.TemporaryDirectory(prefix="eventstore-bench-") as directory:
        path = Path(directory)
        store_path, sqlite_path = path / "events.evt", path / "events.sqlite"
        start = time.perf_counter()
        proc = subprocess.run([str(ROOT / "build/eventstore"), "ingest", str(store_path)],
                              input=data, text=True, capture_output=True, check=True)
        store_write = time.perf_counter() - start
        assert len(proc.stdout.splitlines()) == args.rows
        db = sqlite3.connect(sqlite_path)
        db.execute("PRAGMA journal_mode=DELETE")
        db.execute("PRAGMA synchronous=FULL")
        db.execute("CREATE TABLE events (id INTEGER PRIMARY KEY, timestamp INTEGER NOT NULL, category TEXT NOT NULL, value INTEGER NOT NULL, payload TEXT NOT NULL)")
        db.commit()
        start = time.perf_counter()
        for row in rows:
            db.execute("INSERT INTO events(timestamp,category,value,payload) VALUES (?,?,?,?)", row)
            db.commit()
        sqlite_write = time.perf_counter() - start
        db.close()
        results = []
        for low, high in [(-1000, 1000), (-100, 100), (0, 0)]:
            cpp = json.loads(subprocess.check_output([str(ROOT / "build/query_bench"),
                str(store_path), str(low), str(high), str(args.repetitions)], text=True))
            start = time.perf_counter()
            db = sqlite3.connect(sqlite_path)
            sqlite_open = time.perf_counter() - start
            db.execute(SQL, (low, high)).fetchall()  # warmup
            times = []
            for _ in range(args.repetitions):
                start = time.perf_counter()
                actual = db.execute(SQL, (low, high)).fetchall()
                times.append(time.perf_counter() - start)
            db.close()
            reference = collections.Counter(row[1] for row in rows if low <= row[0] <= high)
            expected = [{"category": k, "count": reference[k]} for k in sorted(reference)]
            assert cpp["counts"] == expected
            assert [{"category": k, "count": n} for k, n in actual] == expected
            results.append({"inclusive_window": [low, high], "matching_rows": sum(reference.values()),
                "eventstore": {"open_seconds": cpp["open_seconds"], "query_seconds": cpp["query_seconds"],
                               "median_query_seconds": statistics.median(cpp["query_seconds"])},
                "sqlite": {"open_seconds": sqlite_open, "query_seconds": times,
                           "median_query_seconds": statistics.median(times)}, "reference_equal": True})
        compiler = subprocess.check_output([os.environ.get("CXX", "c++"), "--version"], text=True).splitlines()[0]
        fs = os.statvfs(path)
        report = {
            "measured_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "environment": {"platform": platform.platform(), "machine": platform.machine(),
                "logical_cpu_count": os.cpu_count(), "python": platform.python_version(),
                "sqlite": sqlite3.sqlite_version, "compiler": compiler,
                "build_flags": os.environ.get("CXXFLAGS", "-O2 -g -std=c++20 -Wall -Wextra -Wpedantic -Werror -pthread"),
                "filesystem_block_size": fs.f_frsize, "temporary_directory_storage": "host default temp directory"},
            "dataset": {"rows": args.rows, "seed": 20261023, "categories": 12, "synthetic": True},
            "method": {"query": SQL, "index": "primary key only; no timestamp/category index",
                "warmup_queries": 1, "timed_repetitions": args.repetitions,
                "durability": "eventstore fsync per event; SQLite DELETE journal and FULL synchronous per commit",
                "timing_limits": "Queries timed inside each process after warmup; C++ open validates every frame, SQLite connect is lazy. Ingest timings differ: C++ includes CLI creation/TSV/JSON acknowledgments; SQLite excludes schema setup. Storage cache is not evicted. This is not an engine ranking or a power-loss test."},
            "write_seconds": {"eventstore_cli": store_write, "sqlite_python": sqlite_write},
            "file_bytes": {"eventstore": store_path.stat().st_size, "sqlite": sqlite_path.stat().st_size},
            "queries": results,
        }
    output = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output)
    print(output, end="")


if __name__ == "__main__":
    main()

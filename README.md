# Eventstore C++

A small C++20 append-only event store built to make persistence failures observable. It stores typed events in a checksummed binary file, acknowledges each append after `fsync`, and offers time-window queries and grouped counts through a library and JSONL CLI.

The interesting part is the failure contract: interrupted trailing writes can be recovered, complete corrupt records are refused, failed appends are rolled back durably, and a failed rollback poisons the open object. This is a bounded local storage project, with no SQL parser, replication or consensus.

## Build and try it

Requirements: Linux or macOS, a C++20 compiler (GCC 12+ or a recent Apple Clang), Make, and Python 3 for tests/benchmarks. The library and CLI have no third-party dependencies.

```sh
make
./build/eventstore append demo.evt 1700000000000000 checkout 42 'synthetic order'
./build/eventstore append demo.evt 1700000000000100 checkout -3 'synthetic refund'
./build/eventstore query demo.evt 1700000000000000 1700000000000200
./build/eventstore counts demo.evt 1700000000000000 1700000000000200
make test
```

The first append returns:

```json
{"id":1,"timestamp":1700000000000000,"category":"checkout","value":42,"payload":"synthetic order"}
```

Counts output is one JSON object per category, sorted by category:

```json
{"category":"checkout","count":2}
```

CLI forms:

| Command | Behavior |
| --- | --- |
| `append FILE TIMESTAMP CATEGORY VALUE PAYLOAD` | Append one event and print its acknowledgment |
| `ingest FILE` | Read TSV from stdin; acknowledge each individual event |
| `query FILE FROM TO [CATEGORY]` | Emit matching events in append order |
| `counts FILE FROM TO [CATEGORY]` | Emit grouped counts |
| `recover FILE` | Exclusively open, recover an incomplete tail and report removed bytes; creates an empty store if absent |

Timestamps are caller-supplied signed 64-bit integers, interpreted as microseconds since the Unix epoch by convention. They may arrive out of order. Both range endpoints are inclusive. IDs start at 1 and are contiguous in append order. The value is a signed 64-bit integer. Category is 1–64 UTF-8 bytes; payload is 0–1,048,576 UTF-8 bytes. Controls such as NUL are allowed by the library and escaped in JSON. JSON numbers preserve 64-bit decimal values; JavaScript clients should use a parser that preserves integers larger than 2^53.

For ingest, each line contains `timestamp<TAB>category<TAB>value<TAB>payload`. Only the first three tabs are separators. Payload may contain tabs, but embedded newlines require the library or a single `append` argument. Input is bounded before accumulating a whole oversized line. Malformed input stops ingestion; earlier acknowledged lines stay committed. Ingest is not a batch transaction.

Exit codes: 0 success, 1 storage/input/output failure, 2 invalid command usage. Diagnostics go to stderr. Queries never modify a file. If they find an incomplete tail, use `recover` or open a writer first.

## Durability and concurrency contract

- Each successful `Store::append` writes a complete frame and waits for `fsync(file)` before returning. CLI acknowledgments are emitted and flushed after that return. The parent directory is also synced when a writer opens, covering file creation before an acknowledgment.
- A process killed after receiving an acknowledgment can reopen and read that event. The tests exercise real `SIGKILL`. Hardware power loss is **not** simulated: persistence relies on the operating system, filesystem and device honoring `fsync`. This is not a guarantee against failing hardware or dishonest caches; on macOS this uses `fsync`, not `F_FULLFSYNC`.
- Writers acquire a nonblocking exclusive `flock` for the lifetime of the object; readers acquire a shared lock. Multiple readers can coexist. A reader and a writer cannot coexist. Contention fails immediately, without waiting. Calls on the same object serialize through a mutex.
- Recovery first validates the complete prefix. A trailing fragment shorter than a frame header, or a valid frame header whose body extends past EOF, is truncated to the last complete record and the truncation is synced. Complete bad headers, bad checksums, invalid UTF-8, invalid lengths or noncontiguous IDs fail closed without changing the file. A partial initial file header can be completed only if it is an exact prefix of the expected header.
- If writing or syncing an append fails, the library attempts `ftruncate` back to the previous end and then `fsync`. A confirmed rollback preserves the next ID. If either rollback step fails, the object rejects all subsequent reads/writes. Close it, resolve the storage error, and inspect/reopen. An unacknowledged event may be present after an interrupted or uncertain operation.
- A committed append whose acknowledgment is lost may be retried as a duplicate. There is no idempotency key or exactly-once delivery claim. Retained unacknowledged complete records get IDs; removed incomplete records do not.

Use a local filesystem with supported advisory locks and directory syncing. All access must cooperate with locking. Do not edit, replace, rename or unlink an active store, or share a `Store` across `fork`. The final pathname must not be a symlink, and the file must be regular. Parent directories should be trusted. CRC32 detects accidental corruption; it provides no cryptographic authenticity.

## Library

```cpp
#include "eventstore.hpp"
#include <iostream>

int main() {
    eventstore::Store store("library-demo.evt");
    auto acknowledged = store.append({100, "checkout", 42, "synthetic"});
    std::cout << acknowledged.id << '\n';
    store.query({0, 1000, std::nullopt}, [](const eventstore::Event& event) {
        std::cout << eventstore::to_json(event) << '\n';
    });
}
```

Link with `c++ -std=c++20 -pthread -Iinclude example.cpp build/libeventstore.a -o example` after `make`. Open `Store(path, Access::ReadOnly)` for readers. Exceptions report validation, corruption, lock and I/O failures. A query visitor is called while the object mutex is held; it must not re-enter that object. Visitor exceptions propagate without poisoning the store.

`make` produces both `build/eventstore` and `build/libeventstore.a`. The public API is in [`include/eventstore.hpp`](include/eventstore.hpp). The [format and architecture](docs/architecture.md) are documented separately.

## Verification and measurement

`make test` runs filesystem/subprocess tests for append/reopen, every partial-frame truncation point, header/body corruption, malicious lengths with valid checksums, UTF-8/size bounds, reference queries, reader/writer locks, threaded calls, and termination after acknowledgment. `RLIMIT_FSIZE` forces a real partial write. Linux-only test interposition forces append-sync and rollback failures without modifying the shipped library. macOS runs the portable cases and skips five Linux interposition tests.

```sh
make bench
# Or choose a size and retain the report:
make build/query_bench
python3 bench/compare.py --rows 2000 --repetitions 7 --output bench/results/local.json
```

The benchmark generates deterministic synthetic events and compares the same inclusive time-window grouped count with Python's SQLite. It validates both results against an independent in-memory reference. Query times exclude process startup and include no timestamp index in either engine. File pages are warmed; storage caches are not evicted. Each eventstore query validates frame checksums, while SQLite follows its own storage path. Store-open validation is measured separately. Write measurements include different ingestion overheads, spelled out in the JSON report.

[`bench/results/sample-linux.json`](bench/results/sample-linux.json) records one 2,000-event run and its environment. The full-window median was approximately **1.66 ms** for this event store and **0.36 ms** for SQLite. This is a reproducible baseline, not a speed advantage or a general database comparison.

## Scope and limits

Opening validates the whole file in O(file bytes). Queries scan it again and use O(maximum record size) streaming memory. Counts additionally use O(number of categories), with a 100,000-category cap. No indexes, compaction, deletion, retention, cross-file transaction or schema evolution is implemented. Files can grow until storage/offset limits; disk-full conditions return errors. A query can stop with an error after already streaming rows, so consumers must check the process exit status. The intended dataset is modest local event logs where transparent correctness matters more than query speed.

CI is configured for Linux and macOS plus Linux undefined-behavior sanitizer tests. A Dockerfile is provided as optional packaging; it builds and runs from the same Debian release. Example after building the image:

```sh
docker build -t eventstore-cpp .
docker run --rm -v "$PWD:/data" eventstore-cpp append /data/container.evt 100 demo 1 hello
```

The CLI has no network service or external account dependency.

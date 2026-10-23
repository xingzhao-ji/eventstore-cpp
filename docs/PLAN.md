# Implementation plan

Build a dependency-free POSIX C++20 event store with a library and CLI. Use bounded checksummed frames, exclusive writer/shared reader advisory locks, synced acknowledgments and conservative tail recovery. Refuse complete corrupt records. Exercise real files and subprocesses, then compare the same time-window grouped-count query with SQLite using synthetic data.

Stages: specify failure tests; implement framing and durable append; add streaming queries and JSONL CLI; validate crash/lock/input/corruption behavior; document guarantees and measure a reproducible benchmark. No SQL engine, replication or consensus.

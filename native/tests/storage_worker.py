"""A separate process that uses the storage files, for the concurrency and crash tests.

Usage: python -I storage_worker.py <mode> <data_dir> [count]
"""

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))

from rq_engine import storage as st  # noqa: E402


def write_pairs(store, count, tag):
    """Each source and its audit event commit together, so their counts must always agree."""
    for index in range(count):
        store.add_data_source(f"{tag}-{index}", "synthetic", "synthetic_test", "Written by the test worker.")


def main():
    mode, data_dir = sys.argv[1], sys.argv[2]
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 100
    store = st.Storage(data_dir)
    if mode == "init":
        store.initialise()
    elif mode == "pairs":
        write_pairs(store, count, f"worker-{os.getpid()}")
    elif mode == "forever":
        print("started", flush=True)
        write_pairs(store, 10 ** 9, f"worker-{os.getpid()}")
    elif mode == "crash_in_transaction":
        # Dies abruptly with uncommitted work, as a crash or a killed process would.
        connection = st.connect(store.path(st.RESEARCH))
        connection.execute("BEGIN IMMEDIATE")
        connection.execute("INSERT INTO data_sources (id, name, provider, permitted_use, licence_notes, created_ms) VALUES ('crash', 'crash', 'x', 'unknown', 'x', 1)")
        os._exit(7)
    elif mode == "crash_during_dataset":
        # Dies after some bars are committed, before the dataset is marked ready.
        source = store.add_data_source("crash-source", "synthetic", "synthetic_test", "Written by the test worker.")

        def bars():
            for index in range(st.BAR_CHUNK + 10):
                if index == st.BAR_CHUNK + 5:
                    os._exit(7)
                yield {"symbol": "T", "time_ms": index, "open": 1.0, "high": 2.0, "low": 0.5, "close": 1.0, "volume": 1.0}

        store.add_dataset(source, "crash", "1M", bars())
    elif mode == "hold_open":
        connection = st.connect(store.path(st.RESEARCH))
        connection.execute("SELECT count(*) FROM audit_events").fetchone()
        print("open", flush=True)
        sys.stdin.read()
        connection.close()
    else:
        raise SystemExit(f"Unknown mode: {mode}")


if __name__ == "__main__":
    main()

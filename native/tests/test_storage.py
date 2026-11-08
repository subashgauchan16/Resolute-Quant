"""Tests the local storage on the private Python, using real files and real separate processes."""

import contextlib
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

NATIVE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(NATIVE / "python"))

from rq_engine import storage as st  # noqa: E402

WORKER = str(Path(__file__).with_name("storage_worker.py"))


def worker(mode, folder, *extra):
    return subprocess.Popen([sys.executable, "-I", WORKER, mode, str(folder), *map(str, extra)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.PIPE, text=True)


def bars(count, symbol="T", start=0):
    for index in range(start, start + count):
        yield {"symbol": symbol, "time_ms": index * 60000, "open": 10.0 + index, "high": 12.0 + index, "low": 9.0 + index, "close": 11.0 + index, "volume": None if index == 3 else 5.5}


class Base(unittest.TestCase):
    def setUp(self):
        self._folder = tempfile.TemporaryDirectory(prefix="rq-storage-test-", ignore_cleanup_errors=True)
        self.addCleanup(self._folder.cleanup)
        self.folder = Path(self._folder.name)
        self.store = st.Storage(self.folder / "data")
        self.store.initialise()

    def source(self, permitted_use="synthetic_test", name="source"):
        return self.store.add_data_source(name, "synthetic", permitted_use, "Test data.")

    def count(self, table, kind=st.RESEARCH, where="1=1"):
        with self.store.open(kind) as connection:
            return connection.execute(f"SELECT count(*) FROM {table} WHERE {where}").fetchone()[0]


class Library(unittest.TestCase):
    def test_this_runtime_has_the_fixed_library(self):
        self.assertGreaterEqual(sqlite3.sqlite_version_info[:3], st.MINIMUM_SQLITE)

    def test_older_versions_are_refused(self):
        for version in ((3, 50, 4), (3, 51, 2)):
            with self.assertRaisesRegex(st.StorageError, "needs 3.51.3"):
                st.check_sqlite_version(version)
        st.check_sqlite_version((3, 51, 3))

    def test_an_old_library_refuses_to_open_a_database(self):
        with mock.patch.object(st.sqlite3, "sqlite_version_info", (3, 50, 4)), self.assertRaises(st.StorageError):
            st.connect(":memory:")


class SafeLocations(unittest.TestCase):
    def test_network_paths_are_refused(self):
        with self.assertRaisesRegex(st.StorageError, "network path"):
            st.check_safe_location(r"\\server\share\data")

    def test_network_drives_are_refused(self):
        with mock.patch.object(st.ctypes.windll.kernel32, "GetDriveTypeW", return_value=st.DRIVE_REMOTE), self.assertRaisesRegex(st.StorageError, "network drive"):
            st.check_safe_location(r"Z:\data")

    def test_synced_folders_are_refused(self):
        for path in (r"C:\Users\a\OneDrive\data", r"C:\Users\a\OneDrive - Company\data", r"C:\Users\a\Dropbox\data", r"C:\Users\a\Google Drive\data"):
            with self.subTest(path), self.assertRaisesRegex(st.StorageError, "cloud-synced"):
                st.check_safe_location(path)
        with mock.patch.dict(st.os.environ, {"OneDrive": r"D:\Sync Here"}), self.assertRaisesRegex(st.StorageError, "cloud-synced"):
            st.check_safe_location(r"D:\Sync Here\data")

    def test_the_default_folder_is_local_and_allowed(self):
        self.assertIn("ResoluteQuant", str(st.default_data_dir()))
        st.check_safe_location(st.default_data_dir())

    def test_a_refused_location_creates_nothing(self):
        with tempfile.TemporaryDirectory() as folder:
            target = Path(folder) / "OneDrive" / "data"
            with self.assertRaises(st.StorageError):
                st.Storage(target).initialise()
            self.assertFalse(target.parent.exists())


class Persistence(Base):
    def test_settings_on_every_connection(self):
        with self.store.open() as connection:
            self.assertEqual(connection.execute("PRAGMA journal_mode").fetchone()[0], "wal")
            self.assertEqual(connection.execute("PRAGMA foreign_keys").fetchone()[0], 1)
            self.assertEqual(connection.execute("PRAGMA synchronous").fetchone()[0], 2)  # FULL
            self.assertEqual(connection.execute("PRAGMA busy_timeout").fetchone()[0], st.BUSY_TIMEOUT_MS)
        with self.store.open(st.MARKET) as market:
            self.assertEqual(market.execute("PRAGMA synchronous").fetchone()[0], 1)  # NORMAL

    def test_data_survives_closing_and_reopening(self):
        source = self.source()
        dataset = self.store.add_dataset(source, "d", "1D", bars(20))
        reopened = st.Storage(self.store.data_dir)
        self.assertEqual(reopened.initialise(), {st.RESEARCH: [], st.MARKET: []})
        with reopened.open() as connection:
            row = connection.execute("SELECT status, row_count FROM datasets WHERE id = ?", (dataset,)).fetchone()
        self.assertEqual(row, ("ready", 20))
        self.assertTrue(reopened.verify_dataset(dataset))

    def test_bars_read_back_exactly(self):
        dataset = self.store.add_dataset(self.source(), "d", "1D", bars(5))
        with self.store.open(st.MARKET) as market:
            rows = market.execute("SELECT time_ms, open, volume FROM bars WHERE dataset_id = ? ORDER BY time_ms", (dataset,)).fetchall()
        self.assertEqual(rows[0], (0, 10.0, 5.5))
        self.assertEqual(rows[3], (180000, 13.0, None))  # A missing volume stays missing, never zero.

    def test_market_data_and_records_are_in_separate_files(self):
        self.assertEqual(self.count("sqlite_master", st.MARKET, "name='data_sources'"), 0)
        self.assertEqual(self.count("sqlite_master", st.RESEARCH, "name='bars'"), 0)
        self.assertTrue(self.store.artifacts.is_dir())


class Migrations(Base):
    def make(self, *bodies):
        folder = Path(tempfile.mkdtemp(dir=self.folder))
        for number, body in enumerate(bodies, 1):
            (folder / f"{number:03}_step.sql").write_text(body, encoding="utf-8")
        return folder

    def test_an_empty_database_reaches_the_latest_version(self):
        fresh = st.Storage(self.folder / "fresh")
        applied = fresh.initialise()
        self.assertEqual(applied, {st.RESEARCH: [1], st.MARKET: [1]})
        self.assertEqual(self.store.status()["databases"][st.RESEARCH]["schema_version"], 1)

    def test_an_older_database_is_upgraded_keeping_data_and_backing_up_first(self):
        path = self.folder / "old.sqlite"
        first = self.make("CREATE TABLE a (x INTEGER) STRICT;")
        with contextlib.closing(st.connect(path)) as connection:
            st.migrate(connection, first)
            connection.execute("INSERT INTO a VALUES (5)")
        second = self.make("CREATE TABLE a (x INTEGER) STRICT;", "ALTER TABLE a ADD COLUMN y INTEGER DEFAULT 9;")
        backups = []
        with contextlib.closing(st.connect(path)) as connection:
            self.assertEqual(st.migrate(connection, second, lambda: backups.append(1)), [2])
            self.assertEqual(connection.execute("SELECT x, y FROM a").fetchone(), (5, 9))
        self.assertEqual(backups, [1])

    def test_initialise_backs_up_before_an_upgrade_with_a_verified_copy(self):
        self.source()
        folder = self.folder / "next"
        shutil.copytree(st.MIGRATIONS, folder)
        (folder / st.RESEARCH / "002_extra.sql").write_text("CREATE TABLE extra (x INTEGER) STRICT;", encoding="utf-8")
        upgraded = st.Storage(self.store.data_dir, migrations=folder)
        self.assertEqual(upgraded.initialise()[st.RESEARCH], [2])
        snapshots = list(self.store.backups.glob("research-*.sqlite"))
        self.assertEqual(len(snapshots), 1)
        with contextlib.closing(sqlite3.connect(snapshots[0])) as copy:
            self.assertEqual(copy.execute("SELECT count(*) FROM data_sources").fetchone()[0], 1)
            self.assertEqual(copy.execute("SELECT count(*) FROM sqlite_master WHERE name='extra'").fetchone()[0], 0)

    def test_a_failed_migration_changes_nothing(self):
        path = self.folder / "failing.sqlite"
        folder = self.make("CREATE TABLE good (x INTEGER) STRICT;", "CREATE TABLE half (x INTEGER) STRICT;\nINSERT INTO good VALUES (1);\nTHIS IS NOT SQL;")
        with contextlib.closing(st.connect(path)) as connection:
            with self.assertRaises(sqlite3.Error):
                st.migrate(connection, folder)
            self.assertEqual(connection.execute("SELECT max(version) FROM schema_migrations").fetchone()[0], 1)
            self.assertEqual(connection.execute("SELECT count(*) FROM sqlite_master WHERE name='half'").fetchone()[0], 0)
            self.assertEqual(connection.execute("SELECT count(*) FROM good").fetchone()[0], 0)
            self.assertFalse(connection.in_transaction)

    def test_a_migration_that_breaks_references_is_rolled_back(self):
        path = self.folder / "broken.sqlite"
        folder = self.make("CREATE TABLE p (id INTEGER PRIMARY KEY) STRICT;",
                           "PRAGMA defer_foreign_keys = ON; CREATE TABLE c (p INTEGER REFERENCES p (id)) STRICT; INSERT INTO c VALUES (99);")
        with contextlib.closing(st.connect(path)) as connection:
            with self.assertRaisesRegex(st.StorageError, "broken references"):
                st.migrate(connection, folder)
            self.assertEqual(connection.execute("SELECT count(*) FROM sqlite_master WHERE name='c'").fetchone()[0], 0)

    def test_an_edited_migration_is_refused(self):
        path = self.folder / "edited.sqlite"
        folder = self.make("CREATE TABLE a (x INTEGER) STRICT;")
        with contextlib.closing(st.connect(path)) as connection:
            st.migrate(connection, folder)
            (folder / "001_step.sql").write_text("CREATE TABLE a (x TEXT) STRICT;", encoding="utf-8")
            with self.assertRaisesRegex(st.StorageError, "has changed"):
                st.migrate(connection, folder)

    def test_a_newer_database_is_refused(self):
        path = self.folder / "newer.sqlite"
        with contextlib.closing(st.connect(path)) as connection:
            st.migrate(connection, self.make("CREATE TABLE a (x INTEGER) STRICT;", "CREATE TABLE b (x INTEGER) STRICT;"))
            with self.assertRaisesRegex(st.StorageError, "newer than this app"):
                st.migrate(connection, self.make("CREATE TABLE a (x INTEGER) STRICT;"))

    def test_gaps_in_numbering_are_refused(self):
        folder = self.make("CREATE TABLE a (x INTEGER) STRICT;")
        (folder / "003_step.sql").write_text("SELECT 1;", encoding="utf-8")
        with contextlib.closing(st.connect(self.folder / "gap.sqlite")) as connection, self.assertRaisesRegex(st.StorageError, "no gaps"):
            st.migrate(connection, folder)

    def test_several_processes_starting_together_all_succeed(self):
        target = self.folder / "together"
        workers = [worker("init", target) for _ in range(4)]
        results = [(w.communicate(timeout=60), w.returncode) for w in workers]
        for (output, code) in results:
            self.assertEqual(code, 0, output[1])
        fresh = st.Storage(target)
        self.assertEqual(fresh.initialise(), {st.RESEARCH: [], st.MARKET: []})


class Rules(Base):
    def dataset(self, permitted_use="synthetic_test"):
        return self.store.add_dataset(self.source(permitted_use, name=f"s-{st.new_id()}"), "d", "1D", bars(5))

    def register(self, dataset, **overrides):
        values = {"name": "e", "dataset_id": dataset, "hypothesis": "IF X THEN Y", "plan": {"trials": 1}, "splits": {"train": [0, 1]}, "costs": {"fee_bps": 1}, "code_version": "abc"}
        values.update(overrides)
        return self.store.register_experiment(**values)

    def test_audit_events_cannot_be_changed_or_deleted(self):
        self.source()
        with self.store.open() as connection:
            for statement in ("UPDATE audit_events SET actor = 'x'", "DELETE FROM audit_events"):
                with self.subTest(statement), self.assertRaisesRegex(sqlite3.IntegrityError, "Audit events cannot"):
                    connection.execute(statement)
        self.assertEqual(self.count("audit_events", where="action='data_source.added'"), 1)

    def test_invalid_references_are_rejected(self):
        with self.store.open() as connection, self.assertRaises(sqlite3.IntegrityError):
            connection.execute("INSERT INTO datasets (id, source_id, name, bar_interval, status, created_ms) VALUES ('d', 'missing', 'n', '1D', 'pending', 1)")

    def test_a_dataset_cannot_claim_to_be_ready_without_a_hash(self):
        source = self.source()
        with self.store.open() as connection, self.assertRaises(sqlite3.IntegrityError):
            connection.execute("INSERT INTO datasets (id, source_id, name, bar_interval, status, created_ms) VALUES ('d', ?, 'n', '1D', 'ready', 1)", (source,))

    def test_impossible_bars_fail_the_whole_dataset(self):
        source = self.source()
        bad = list(bars(3)) + [{"symbol": "T", "time_ms": 999, "open": 5.0, "high": 1.0, "low": 0.5, "close": 1.0, "volume": 1.0}]
        with self.assertRaises(sqlite3.IntegrityError):
            self.store.add_dataset(source, "bad", "1D", bad)
        self.assertEqual(self.count("datasets", where="status='failed'"), 1)
        self.assertEqual(self.count("bars", st.MARKET), 0)

    def test_nan_and_infinity_are_refused_and_negative_prices_kept(self):
        source = self.source()
        for value in (float("nan"), float("inf")):
            with self.subTest(value), self.assertRaises(sqlite3.IntegrityError):
                self.store.add_dataset(source, "bad", "1D", [{"symbol": "T", "time_ms": 1, "open": value, "high": value, "low": value, "close": value}])
        dataset = self.store.add_dataset(source, "oil", "1D", [{"symbol": "CL", "time_ms": 1, "open": -5.0, "high": -2.0, "low": -6.0, "close": -3.0}])
        self.assertTrue(self.store.verify_dataset(dataset))

    def test_experiments_need_a_source_whose_terms_allow_model_work(self):
        for permitted in ("no_model_use", "unknown"):
            dataset = self.dataset(permitted)
            with self.subTest(permitted), self.assertRaisesRegex(sqlite3.IntegrityError, "do not allow model work"):
                self.register(dataset)
        self.assertTrue(self.register(self.dataset("personal_research")))
        self.assertTrue(self.register(self.dataset("synthetic_test")))

    def test_experiments_need_a_ready_dataset(self):
        source = self.source()
        with self.store.open() as connection:
            connection.execute("INSERT INTO datasets (id, source_id, name, bar_interval, status, created_ms) VALUES ('p', ?, 'n', '1D', 'pending', 1)", (source,))
        with self.assertRaisesRegex(sqlite3.IntegrityError, "not ready"):
            self.register("p")

    def test_a_registered_plan_cannot_change_but_its_verdict_can_be_recorded(self):
        experiment = self.register(self.dataset())
        with self.store.open() as connection:
            for column in ("hypothesis", "plan_json", "split_json", "cost_json", "config_sha256"):
                with self.subTest(column), self.assertRaisesRegex(sqlite3.IntegrityError, "cannot be changed"):
                    connection.execute(f"UPDATE experiments SET {column} = 'changed' WHERE id = ?", (experiment,))
            with self.assertRaises(sqlite3.IntegrityError):  # A finished experiment must have a verdict.
                connection.execute("UPDATE experiments SET status = 'finished' WHERE id = ?", (experiment,))
            connection.execute("UPDATE experiments SET status = 'finished', verdict = 'unsupported', finished_ms = 1 WHERE id = ?", (experiment,))

    def test_a_changed_bar_is_detected(self):
        dataset = self.dataset()
        self.assertTrue(self.store.verify_dataset(dataset))
        with self.store.open(st.MARKET) as market:
            market.execute("UPDATE bars SET close = close + 0.5 WHERE dataset_id = ? AND time_ms = 0", (dataset,))
        self.assertFalse(self.store.verify_dataset(dataset))

    def test_no_table_has_anywhere_to_put_a_secret(self):
        forbidden = ("password", "passwd", "secret", "token", "api_key", "apikey", "credential", "private_key", "account_number", "broker")
        for kind in st.DATABASES:
            with self.store.open(kind) as connection:
                tables = [row[0] for row in connection.execute("SELECT name FROM sqlite_master WHERE type = 'table'")]
                for table in tables:
                    for column in connection.execute(f"PRAGMA table_info({table})"):
                        with self.subTest(table=table, column=column[1]):
                            self.assertFalse(any(word in column[1].lower() for word in forbidden))


class Transactions(Base):
    def test_a_failure_leaves_neither_the_record_nor_its_audit_event(self):
        with mock.patch.object(st.Storage, "audit", side_effect=RuntimeError("audit failed")), self.assertRaises(RuntimeError):
            self.source()
        self.assertEqual(self.count("data_sources"), 0)
        self.assertEqual(self.count("audit_events"), 0)

    def test_a_process_dying_mid_transaction_leaves_nothing(self):
        crashed = worker("crash_in_transaction", self.store.data_dir)
        crashed.communicate(timeout=30)
        self.assertEqual(crashed.returncode, 7)
        self.assertEqual(self.count("data_sources", where="id='crash'"), 0)
        self.source()  # The write lock was released with the process.
        with self.store.open() as connection:
            self.assertEqual(st.check_integrity(connection), [])

    def test_a_process_killed_while_writing_loses_no_consistency(self):
        for _ in range(3):
            running = worker("forever", self.store.data_dir)
            running.stdout.readline()
            time.sleep(0.5)
            running.kill()
            running.communicate(timeout=30)
        with self.store.open() as connection:
            self.assertEqual(st.check_integrity(connection), [])
            sources = connection.execute("SELECT count(*) FROM data_sources").fetchone()[0]
            audits = connection.execute("SELECT count(*) FROM audit_events WHERE action = 'data_source.added'").fetchone()[0]
        self.assertGreater(sources, 0)
        self.assertEqual(sources, audits)

    def test_a_crash_during_a_dataset_never_leaves_it_marked_complete(self):
        crashed = worker("crash_during_dataset", self.store.data_dir)
        crashed.communicate(timeout=60)
        self.assertEqual(crashed.returncode, 7)
        self.assertEqual(self.count("datasets", where="status='ready'"), 0)
        self.assertEqual(self.count("datasets", where="status='pending'"), 1)
        self.assertEqual(self.count("bars", st.MARKET), st.BAR_CHUNK)  # One finished chunk, and none of the next.
        self.assertEqual(self.store.discard_pending_bars(), 1)
        self.assertEqual(self.count("datasets", where="status='failed'"), 1)
        self.assertEqual(self.count("bars", st.MARKET), 0)


class Concurrency(Base):
    def test_readers_see_consistent_data_during_writes(self):
        running = worker("pairs", self.store.data_dir, 300)
        errors, reads = [], [0]
        stop = threading.Event()

        def read():
            try:
                with self.store.open() as connection:
                    while not stop.is_set():
                        connection.execute("BEGIN")
                        sources = connection.execute("SELECT count(*) FROM data_sources").fetchone()[0]
                        audits = connection.execute("SELECT count(*) FROM audit_events WHERE action = 'data_source.added'").fetchone()[0]
                        connection.execute("COMMIT")
                        if sources != audits:
                            errors.append((sources, audits))
                        reads[0] += 1
            except Exception as error:
                errors.append(error)

        readers = [threading.Thread(target=read) for _ in range(4)]
        for reader in readers:
            reader.start()
        output = running.communicate(timeout=120)
        stop.set()
        for reader in readers:
            reader.join()
        self.assertEqual(running.returncode, 0, output[1])
        self.assertEqual(errors, [])
        self.assertGreater(reads[0], 50)
        self.assertEqual(self.count("data_sources"), 300)

    def test_a_reader_is_not_blocked_by_an_open_write_transaction(self):
        with self.store.open() as writer, self.store.open() as reader:
            writer.execute("BEGIN IMMEDIATE")
            writer.execute("INSERT INTO data_sources (id, name, provider, permitted_use, licence_notes, created_ms) VALUES ('w', 'w', 'x', 'unknown', 'x', 1)")
            started = time.monotonic()
            self.assertEqual(reader.execute("SELECT count(*) FROM data_sources").fetchone()[0], 0)  # Sees only committed work.
            self.assertLess(time.monotonic() - started, 1.0)
            writer.execute("ROLLBACK")

    def test_writers_in_separate_processes_queue_and_lose_nothing(self):
        writers = [worker("pairs", self.store.data_dir, 100) for _ in range(3)]
        for running in writers:
            output = running.communicate(timeout=180)
            self.assertEqual(running.returncode, 0, output[1])
        self.assertEqual(self.count("data_sources"), 300)
        self.assertEqual(self.count("audit_events", where="action='data_source.added'"), 300)

    def test_a_blocked_writer_gives_a_clear_failure_after_its_wait(self):
        with self.store.open() as holder:
            holder.execute("BEGIN IMMEDIATE")
            other = sqlite3.connect(self.store.path(st.RESEARCH), timeout=0.2, autocommit=True)
            self.addCleanup(other.close)
            with self.assertRaisesRegex(sqlite3.OperationalError, "locked"):
                other.execute("BEGIN IMMEDIATE")
            holder.execute("ROLLBACK")

    def test_the_wal_file_can_shrink_after_writes(self):
        for index in range(50):
            self.source(name=f"s{index}")
        with self.store.open() as connection:
            busy, _, _ = connection.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchone()
        self.assertEqual(busy, 0)


class BackupAndRestore(Base):
    def test_a_backup_is_a_verified_copy(self):
        self.source()
        backup = self.store.backup()
        self.assertEqual(st._check_file(backup), [])
        with contextlib.closing(sqlite3.connect(backup)) as copy:
            self.assertEqual(copy.execute("SELECT count(*) FROM data_sources").fetchone()[0], 1)
        self.assertEqual(list(self.store.backups.glob("*.partial")), [])

    def test_a_backup_taken_during_writes_is_sound_and_consistent(self):
        running = worker("forever", self.store.data_dir)
        running.stdout.readline()
        try:
            snapshots = [self.store.backup() for _ in range(5)]
        finally:
            running.kill()
            running.communicate(timeout=30)
        for snapshot in snapshots:
            self.assertEqual(st._check_file(snapshot), [])
            with contextlib.closing(sqlite3.connect(snapshot)) as copy:
                sources = copy.execute("SELECT count(*) FROM data_sources").fetchone()[0]
                audits = copy.execute("SELECT count(*) FROM audit_events WHERE action = 'data_source.added'").fetchone()[0]
            self.assertEqual(sources, audits)

    def test_restore_brings_back_exactly_what_was_backed_up_and_keeps_the_old_files(self):
        self.source(name="before")
        backup = self.store.backup()
        self.source(name="after")
        kept = self.store.restore(backup)
        with self.store.open() as connection:
            self.assertEqual([row[0] for row in connection.execute("SELECT name FROM data_sources")], ["before"])
            self.assertEqual(st.check_integrity(connection), [])
            self.assertEqual(connection.execute("PRAGMA journal_mode").fetchone()[0], "wal")
        self.assertEqual(self.store.initialise(), {st.RESEARCH: [], st.MARKET: []})
        with contextlib.closing(sqlite3.connect(kept)) as old:
            self.assertEqual(old.execute("SELECT count(*) FROM data_sources").fetchone()[0], 2)

    def test_restore_is_refused_while_the_database_is_in_use(self):
        self.source(name="kept")
        backup = self.store.backup()
        self.source(name="still here")
        holder = worker("hold_open", self.store.data_dir)
        holder.stdout.readline()
        try:
            with self.assertRaisesRegex(st.StorageError, "in use"):
                self.store.restore(backup)
        finally:
            holder.communicate(input="", timeout=30)
        self.assertEqual(self.count("data_sources"), 2)
        self.assertEqual(list(self.store.data_dir.glob("*.restoring")), [])

    def test_a_damaged_backup_is_refused(self):
        self.source()
        damaged = self.folder / "damaged.sqlite"
        damaged.write_bytes(b"this is not a database" * 500)
        with self.assertRaisesRegex(st.StorageError, "can't be restored"):
            self.store.restore(damaged)
        self.assertEqual(self.count("data_sources"), 1)

    def test_a_truncated_backup_is_refused(self):
        for index in range(100):
            self.source(name=f"s{index}")
        backup = self.store.backup()
        truncated = self.folder / "truncated.sqlite"
        truncated.write_bytes(backup.read_bytes()[: backup.stat().st_size // 2])
        with self.assertRaisesRegex(st.StorageError, "can't be restored"):
            self.store.restore(truncated)

    def test_old_backups_are_pruned(self):
        for _ in range(st.BACKUPS_KEPT + 3):
            self.store.backup()
        self.assertEqual(len(list(self.store.backups.glob("research-*.sqlite"))), st.BACKUPS_KEPT)


class Status(Base):
    def test_status_reports_plain_facts(self):
        self.source()
        report = st.status_job({"data_dir": str(self.store.data_dir)}, None)
        self.assertTrue(report["ok"])
        self.assertEqual(report["sqlite_version"], sqlite3.sqlite_version)
        self.assertEqual(report["databases"][st.RESEARCH], {"schema_version": 1, "journal_mode": "wal"})
        self.assertEqual(report["counts"]["data_sources"], 1)

    def test_self_test_passes(self):
        self.assertTrue(st.self_test_job({}, None)["ok"])


if __name__ == "__main__":
    unittest.main()

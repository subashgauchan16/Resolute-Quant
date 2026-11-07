"""Local storage for research and paper trading, in SQLite files on this PC.

research.sqlite  sources, datasets, experiments and the audit trail.
market.sqlite    historical bars only, which can be rebuilt from their source.

Only Python opens these files. The Qt window asks through PythonEngine jobs, so one SQLite library handles
every connection. No table holds a password, key or token: those belong in Windows Credential Manager.
"""

import contextlib
import ctypes
import hashlib
import json
import os
import shutil
import sqlite3
import time
import uuid
from pathlib import Path

# WAL databases with several connections can hit a reset bug fixed in 3.51.3, and backported to 3.50.7.
MINIMUM_SQLITE = (3, 51, 3)
BUSY_TIMEOUT_MS = 5000
BAR_CHUNK = 5000
BACKUPS_KEPT = 10
MIGRATIONS = Path(__file__).with_name("migrations")
RESEARCH, MARKET = "research", "market"
DATABASES = {RESEARCH: "research.sqlite", MARKET: "market.sqlite"}
# Research and paper records must survive a power cut. Market data can be downloaded again.
SYNCHRONOUS = {RESEARCH: "FULL", MARKET: "NORMAL"}
SYNCED_FOLDER_NAMES = ("onedrive", "dropbox", "google drive", "icloud")
DRIVE_REMOTE = 4


class StorageError(Exception):
    """A storage problem the person can act on. The message is safe to show on screen."""


def default_data_dir():
    return Path(os.environ["LOCALAPPDATA"]) / "ResoluteQuant" / "data"


def check_sqlite_version(version=None):
    version = sqlite3.sqlite_version_info if version is None else version
    if tuple(version[:3]) < MINIMUM_SQLITE:
        needed = ".".join(map(str, MINIMUM_SQLITE))
        raise StorageError(f"This Python uses SQLite {'.'.join(map(str, version[:3]))}, and needs {needed} or newer. Run native\\tools\\setup_runtime.ps1.")


def check_safe_location(folder):
    """Refuses places where SQLite's file locks and paired files can't be relied on."""
    resolved = str(Path(folder).absolute())
    if resolved.startswith("\\\\"):
        raise StorageError(f"Storage can't be on a network path: {resolved}")
    drive = os.path.splitdrive(resolved)[0]
    if drive and ctypes.windll.kernel32.GetDriveTypeW(drive + "\\") == DRIVE_REMOTE:
        raise StorageError(f"Storage can't be on a network drive: {resolved}")
    lowered = resolved.lower()
    for variable in ("OneDrive", "OneDriveConsumer", "OneDriveCommercial"):
        root = os.environ.get(variable)
        if root and (lowered == root.lower() or lowered.startswith(root.lower().rstrip("\\") + "\\")):
            raise StorageError(f"Storage can't be in a cloud-synced folder: {resolved}")
    parts = {part.lower() for part in Path(resolved).parts}
    if any(any(part.startswith(name) for name in SYNCED_FOLDER_NAMES) for part in parts):
        raise StorageError(f"Storage can't be in a cloud-synced folder: {resolved}")


def new_id():
    return str(uuid.uuid4())


def now_ms():
    return int(time.time() * 1000)


def connect(path, kind=RESEARCH):
    """Opens one connection, for use on one thread. Transactions are always explicit."""
    check_sqlite_version()
    connection = sqlite3.connect(path, timeout=BUSY_TIMEOUT_MS / 1000, autocommit=True)
    try:
        connection.execute(f"PRAGMA busy_timeout = {BUSY_TIMEOUT_MS}")
        connection.execute("PRAGMA foreign_keys = ON")
        mode = connection.execute("PRAGMA journal_mode = WAL").fetchone()[0]
        if mode.lower() != "wal":
            raise StorageError(f"Could not turn on WAL for {path}: the mode is {mode}.")
        connection.execute(f"PRAGMA synchronous = {SYNCHRONOUS[kind]}")
    except Exception:
        connection.close()
        raise
    return connection


@contextlib.contextmanager
def transaction(connection):
    """Everything inside commits together, or not at all."""
    connection.execute("BEGIN IMMEDIATE")
    try:
        yield connection
    except BaseException:
        connection.execute("ROLLBACK")
        raise
    connection.execute("COMMIT")


def _migration_files(folder):
    files = []
    for path in sorted(Path(folder).glob("*.sql")):
        files.append((int(path.name.split("_", 1)[0]), path.name, path.read_bytes()))
    versions = [version for version, _, _ in files]
    if versions != list(range(1, len(versions) + 1)):
        raise StorageError(f"Migration numbers in {folder} must run 1, 2, 3 with no gaps: {versions}")
    return files


def _applied_migrations(connection):
    connection.execute("""CREATE TABLE IF NOT EXISTS schema_migrations (
        version INTEGER PRIMARY KEY, name TEXT NOT NULL, sha256 TEXT NOT NULL, applied_ms INTEGER NOT NULL) STRICT""")
    return {row[0]: (row[1], row[2]) for row in connection.execute("SELECT version, name, sha256 FROM schema_migrations")}


def migrate(connection, folder, before_changes=None):
    """Brings a database up to date. Each migration commits fully or not at all. Returns the versions applied."""
    files = _migration_files(folder)
    applied = _applied_migrations(connection)
    known = {version: (name, hashlib.sha256(body).hexdigest()) for version, name, body in files}
    for version, (name, digest) in applied.items():
        if version not in known:
            raise StorageError(f"This database has version {version}, which is newer than this app. Update the app, or restore an older backup.")
        if known[version][1] != digest:
            raise StorageError(f"Migration {name} has changed since it was applied. Applied migrations must never be edited.")
    pending = [entry for entry in files if entry[0] not in applied]
    if pending and applied and before_changes is not None:
        before_changes()
    done = []
    for version, name, body in pending:
        with transaction(connection):
            # Another process may have applied it while this one waited for the write lock.
            if connection.execute("SELECT 1 FROM schema_migrations WHERE version = ?", (version,)).fetchone():
                continue
            connection.executescript(body.decode("utf-8"))
            problems = connection.execute("PRAGMA foreign_key_check").fetchall()
            if problems:
                raise StorageError(f"Migration {name} left {len(problems)} broken references.")
            connection.execute("INSERT INTO schema_migrations (version, name, sha256, applied_ms) VALUES (?, ?, ?, ?)",
                               (version, name, hashlib.sha256(body).hexdigest(), now_ms()))
        done.append(version)
    return done


def check_integrity(connection, full=True):
    """Returns a list of problems. Empty means the file is sound."""
    pragma = "integrity_check" if full else "quick_check"
    problems = [row[0] for row in connection.execute(f"PRAGMA {pragma}") if row[0] != "ok"]
    problems += [f"Broken reference in {row[0]}, row {row[1]}" for row in connection.execute("PRAGMA foreign_key_check")]
    return problems


def _check_file(path):
    """Opens a file read-only and checks it, so that checking can't change it."""
    uri = Path(path).absolute().as_uri() + "?mode=ro"
    try:
        connection = sqlite3.connect(uri, uri=True, autocommit=True)
        try:
            return check_integrity(connection)
        finally:
            connection.close()
    except sqlite3.Error as error:
        return [str(error)]


class Storage:
    """The two databases, and the folders around them, for one data directory."""

    def __init__(self, data_dir=None, migrations=MIGRATIONS):
        self.data_dir = Path(data_dir) if data_dir is not None else default_data_dir()
        self.migrations = Path(migrations)
        self.artifacts = self.data_dir / "artifacts"
        self.backups = self.data_dir / "backups"

    def path(self, kind):
        return self.data_dir / DATABASES[kind]

    def initialise(self):
        """Creates the folders and brings both databases up to date. A database with data is backed up first."""
        check_sqlite_version()
        check_safe_location(self.data_dir)
        for folder in (self.data_dir, self.artifacts, self.backups):
            folder.mkdir(parents=True, exist_ok=True)
        applied = {}
        for kind in DATABASES:
            with contextlib.closing(connect(self.path(kind), kind)) as connection:
                applied[kind] = migrate(connection, self.migrations / kind, lambda k=kind: self.backup(k))
        return applied

    @contextlib.contextmanager
    def open(self, kind=RESEARCH):
        connection = connect(self.path(kind), kind)
        try:
            yield connection
        finally:
            connection.close()

    # Backup and restore.

    def backup(self, kind=RESEARCH):
        """A verified snapshot, safe to take while others write. Never copy the live files by hand."""
        self.backups.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
        final = self.backups / f"{kind}-{stamp}-{new_id()[:8]}.sqlite"
        temporary = final.with_suffix(".partial")
        try:
            with self.open(kind) as connection:
                connection.execute("VACUUM INTO ?", (str(temporary),))
            problems = _check_file(temporary)
            if problems:
                raise StorageError(f"The new {kind} backup failed its check: {problems[0]}")
            os.replace(temporary, final)
        finally:
            with contextlib.suppress(FileNotFoundError):
                temporary.unlink()
        for old in sorted(self.backups.glob(f"{kind}-*.sqlite"))[:-BACKUPS_KEPT]:
            old.unlink()
        return final

    def restore(self, backup_file, kind=RESEARCH):
        """Replaces a database with a verified backup. The current files are kept beside it, as a set."""
        backup_file = Path(backup_file)
        problems = _check_file(backup_file)
        if problems:
            raise StorageError(f"That backup can't be restored: {problems[0]}")
        target = self.path(kind)
        temporary = target.with_suffix(".restoring")
        shutil.copyfile(backup_file, temporary)
        kept = target.with_name(f"{target.name}.before-restore-{new_id()[:8]}")
        moved = []
        try:
            for suffix in ("", "-wal", "-shm"):
                current = Path(str(target) + suffix)
                if current.exists():
                    # Windows refuses this while SQLite has the file open, which stops a restore under a running app.
                    os.rename(current, str(kept) + suffix)
                    moved.append(suffix)
        except OSError as error:
            for suffix in reversed(moved):
                os.rename(str(kept) + suffix, str(target) + suffix)
            temporary.unlink()
            raise StorageError("The database is in use. Close everything using it, then restore again.") from error
        os.replace(temporary, target)
        return kept

    # Application records. Each change and its audit event commit together.

    @staticmethod
    def audit(connection, actor, action, target_type, target_id, detail=None):
        connection.execute("INSERT INTO audit_events (time_ms, actor, action, target_type, target_id, detail_json) VALUES (?, ?, ?, ?, ?, ?)",
                           (now_ms(), actor, action, target_type, target_id, None if detail is None else json.dumps(detail, sort_keys=True)))

    def add_data_source(self, name, provider, permitted_use, licence_notes, actor="system", terms_reviewed_ms=None):
        source_id = new_id()
        with self.open() as connection, transaction(connection):
            connection.execute("INSERT INTO data_sources (id, name, provider, permitted_use, licence_notes, terms_reviewed_ms, created_ms) VALUES (?, ?, ?, ?, ?, ?, ?)",
                               (source_id, name, provider, permitted_use, licence_notes, terms_reviewed_ms, now_ms()))
            self.audit(connection, actor, "data_source.added", "data_source", source_id, {"name": name, "permitted_use": permitted_use})
        return source_id

    def add_dataset(self, source_id, name, bar_interval, bars, actor="system"):
        """Stores bars, then marks the dataset ready. A crash in between leaves it pending, never falsely complete.

        The two files can't commit together, so this order is what keeps them honest.
        """
        dataset_id = new_id()
        with self.open() as connection, transaction(connection):
            connection.execute("INSERT INTO datasets (id, source_id, name, bar_interval, status, created_ms) VALUES (?, ?, ?, ?, 'pending', ?)",
                               (dataset_id, source_id, name, bar_interval, now_ms()))
            self.audit(connection, actor, "dataset.started", "dataset", dataset_id, {"name": name})
        try:
            with self.open(MARKET) as market:
                chunk = []
                for bar in bars:
                    chunk.append((dataset_id, bar["symbol"], bar["time_ms"], bar["open"], bar["high"], bar["low"], bar["close"], bar.get("volume")))
                    if len(chunk) >= BAR_CHUNK:
                        self._insert_bars(market, chunk)
                        chunk = []
                if chunk:
                    self._insert_bars(market, chunk)
                count, digest = self._hash_bars(market, dataset_id)
        except Exception as error:
            with self.open() as connection, transaction(connection):
                connection.execute("UPDATE datasets SET status = 'failed', finalized_ms = ? WHERE id = ?", (now_ms(), dataset_id))
                self.audit(connection, actor, "dataset.failed", "dataset", dataset_id, {"error": type(error).__name__})
            with self.open(MARKET) as market, transaction(market):
                market.execute("DELETE FROM bars WHERE dataset_id = ?", (dataset_id,))
            raise
        with self.open() as connection, transaction(connection):
            connection.execute("UPDATE datasets SET status = 'ready', row_count = ?, content_sha256 = ?, finalized_ms = ? WHERE id = ?", (count, digest, now_ms(), dataset_id))
            self.audit(connection, actor, "dataset.ready", "dataset", dataset_id, {"rows": count, "sha256": digest})
        return dataset_id

    @staticmethod
    def _insert_bars(market, rows):
        with transaction(market):
            market.executemany("INSERT INTO bars (dataset_id, symbol, time_ms, open, high, low, close, volume) VALUES (?, ?, ?, ?, ?, ?, ?, ?)", rows)

    @staticmethod
    def _hash_bars(market, dataset_id):
        digest = hashlib.sha256()
        count = 0
        for row in market.execute("SELECT symbol, time_ms, open, high, low, close, volume FROM bars WHERE dataset_id = ? ORDER BY symbol, time_ms", (dataset_id,)):
            digest.update(repr(row).encode("utf-8"))
            count += 1
        return count, digest.hexdigest()

    def verify_dataset(self, dataset_id):
        """Recomputes a dataset's hash from its bars, and says whether it still matches."""
        with self.open() as connection:
            row = connection.execute("SELECT content_sha256, row_count, status FROM datasets WHERE id = ?", (dataset_id,)).fetchone()
        if row is None or row[2] != "ready":
            return False
        with self.open(MARKET) as market:
            return self._hash_bars(market, dataset_id) == (row[1], row[0])

    def discard_pending_bars(self, actor="system"):
        """After a crash: marks unfinished datasets as failed and removes their bars. Returns how many."""
        with self.open() as connection:
            pending = [row[0] for row in connection.execute("SELECT id FROM datasets WHERE status = 'pending'")]
        for dataset_id in pending:
            with self.open(MARKET) as market, transaction(market):
                market.execute("DELETE FROM bars WHERE dataset_id = ?", (dataset_id,))
            with self.open() as connection, transaction(connection):
                connection.execute("UPDATE datasets SET status = 'failed', finalized_ms = ? WHERE id = ?", (now_ms(), dataset_id))
                self.audit(connection, actor, "dataset.discarded", "dataset", dataset_id)
        return len(pending)

    def register_experiment(self, name, dataset_id, hypothesis, plan, splits, costs, code_version, actor="system"):
        """Records the plan before any results exist. The database refuses later changes to it."""
        experiment_id = new_id()
        plan_json, split_json, cost_json = (json.dumps(value, sort_keys=True) for value in (plan, splits, costs))
        config = hashlib.sha256("\n".join((hypothesis, plan_json, split_json, cost_json, dataset_id)).encode("utf-8")).hexdigest()
        with self.open() as connection, transaction(connection):
            connection.execute("""INSERT INTO experiments (id, name, dataset_id, hypothesis, plan_json, split_json, cost_json, config_sha256, code_version, created_ms)
                                  VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
                               (experiment_id, name, dataset_id, hypothesis, plan_json, split_json, cost_json, config, code_version, now_ms()))
            self.audit(connection, actor, "experiment.registered", "experiment", experiment_id, {"config_sha256": config})
        return experiment_id

    # Status.

    def status(self):
        """Plain facts for the window. Calling this creates and upgrades the databases if needed."""
        applied = self.initialise()
        report = {"sqlite_version": sqlite3.sqlite_version, "data_dir": str(self.data_dir), "ok": True, "problems": [], "migrated": applied, "databases": {}}
        for kind in DATABASES:
            with self.open(kind) as connection:
                problems = check_integrity(connection, full=False)
                report["databases"][kind] = {
                    "schema_version": connection.execute("SELECT max(version) FROM schema_migrations").fetchone()[0],
                    "journal_mode": connection.execute("PRAGMA journal_mode").fetchone()[0],
                }
                report["problems"] += problems
        with self.open() as connection:
            counts = {table: connection.execute(f"SELECT count(*) FROM {table}").fetchone()[0] for table in ("data_sources", "datasets", "experiments", "audit_events")}
        with self.open(MARKET) as market:
            counts["bars"] = market.execute("SELECT count(*) FROM bars").fetchone()[0]
        report["counts"] = counts
        report["ok"] = not report["problems"]
        return report


def status_job(args, _host):
    """Job for the window. Tests pass their own folder, so they never touch real data."""
    return Storage(args.get("data_dir")).status()


def self_test_job(_args, _host):
    """Used by the app's smoke test: proves the deployed Python, library and migrations work, in a temporary folder."""
    import tempfile

    with tempfile.TemporaryDirectory(prefix="rq-storage-") as folder:
        storage = Storage(folder)
        storage.initialise()
        source = storage.add_data_source("self-test", "synthetic", "synthetic_test", "Generated by the self-test.")
        bars = [{"symbol": "TEST", "time_ms": i * 60000, "open": 1.0, "high": 2.0, "low": 0.5, "close": 1.5, "volume": 1.0} for i in range(10)]
        dataset = storage.add_dataset(source, "self-test", "1M", bars)
        backup = storage.backup()
        report = storage.status()
        return {"ok": report["ok"] and storage.verify_dataset(dataset) and backup.exists(), "sqlite_version": sqlite3.sqlite_version}

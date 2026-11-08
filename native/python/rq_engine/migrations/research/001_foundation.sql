-- Application records: where data came from, what was registered, and what happened.
-- Times are UTC milliseconds. Identifiers are UUID text, so records can later move to another
-- database without clashing. No table here may ever hold a password, key or token.

CREATE TABLE data_sources (
  id                TEXT PRIMARY KEY,
  name              TEXT NOT NULL UNIQUE,
  provider          TEXT NOT NULL,
  -- Whether the provider's terms allow model work. Unknown is treated as not allowed.
  permitted_use     TEXT NOT NULL CHECK (permitted_use IN ('personal_research', 'no_model_use', 'synthetic_test', 'unknown')),
  licence_notes     TEXT NOT NULL,
  terms_reviewed_ms INTEGER,
  created_ms        INTEGER NOT NULL
) STRICT;

CREATE TABLE datasets (
  id             TEXT PRIMARY KEY,
  source_id      TEXT NOT NULL REFERENCES data_sources (id),
  name           TEXT NOT NULL,
  bar_interval   TEXT NOT NULL,
  status         TEXT NOT NULL CHECK (status IN ('pending', 'ready', 'failed')),
  row_count      INTEGER CHECK (row_count IS NULL OR row_count >= 0),
  content_sha256 TEXT,
  audit_json     TEXT,
  created_ms     INTEGER NOT NULL,
  finalized_ms   INTEGER,
  -- A dataset can only claim to be complete once its contents are counted and hashed.
  CHECK (status <> 'ready' OR (row_count IS NOT NULL AND content_sha256 IS NOT NULL AND finalized_ms IS NOT NULL))
) STRICT;
CREATE INDEX datasets_by_source ON datasets (source_id);

CREATE TABLE experiments (
  id            TEXT PRIMARY KEY,
  name          TEXT NOT NULL,
  dataset_id    TEXT NOT NULL REFERENCES datasets (id),
  hypothesis    TEXT NOT NULL CHECK (length(hypothesis) > 0),
  plan_json     TEXT NOT NULL,
  split_json    TEXT NOT NULL,
  cost_json     TEXT NOT NULL,
  config_sha256 TEXT NOT NULL,
  code_version  TEXT NOT NULL,
  status        TEXT NOT NULL DEFAULT 'registered' CHECK (status IN ('registered', 'running', 'finished')),
  verdict       TEXT CHECK (verdict IS NULL OR verdict IN ('supported', 'unsupported', 'inconclusive')),
  created_ms    INTEGER NOT NULL,
  finished_ms   INTEGER,
  CHECK ((status = 'finished') = (verdict IS NOT NULL))
) STRICT;
CREATE INDEX experiments_by_dataset ON experiments (dataset_id);

-- Experiments can only use a complete dataset from a source whose terms allow model work.
CREATE TRIGGER experiments_require_usable_data BEFORE INSERT ON experiments
BEGIN
  SELECT RAISE(ABORT, 'This dataset is not ready.')
   WHERE (SELECT status FROM datasets WHERE id = NEW.dataset_id) IS NOT 'ready';
  SELECT RAISE(ABORT, 'The terms of this data source do not allow model work.')
   WHERE (SELECT s.permitted_use FROM datasets d JOIN data_sources s ON s.id = d.source_id WHERE d.id = NEW.dataset_id)
         NOT IN ('personal_research', 'synthetic_test');
END;

-- What was registered before seeing results can't be changed afterwards.
CREATE TRIGGER experiments_keep_their_plan BEFORE UPDATE ON experiments
WHEN NEW.dataset_id IS NOT OLD.dataset_id OR NEW.hypothesis IS NOT OLD.hypothesis OR NEW.plan_json IS NOT OLD.plan_json
  OR NEW.split_json IS NOT OLD.split_json OR NEW.cost_json IS NOT OLD.cost_json OR NEW.config_sha256 IS NOT OLD.config_sha256
BEGIN
  SELECT RAISE(ABORT, 'A registered experiment''s plan cannot be changed.');
END;

CREATE TABLE audit_events (
  sequence    INTEGER PRIMARY KEY AUTOINCREMENT,
  time_ms     INTEGER NOT NULL,
  actor       TEXT NOT NULL,
  action      TEXT NOT NULL,
  target_type TEXT NOT NULL,
  target_id   TEXT NOT NULL,
  detail_json TEXT
) STRICT;

CREATE TRIGGER audit_events_no_update BEFORE UPDATE ON audit_events
BEGIN
  SELECT RAISE(ABORT, 'Audit events cannot be changed.');
END;

CREATE TRIGGER audit_events_no_delete BEFORE DELETE ON audit_events
BEGIN
  SELECT RAISE(ABORT, 'Audit events cannot be deleted.');
END;

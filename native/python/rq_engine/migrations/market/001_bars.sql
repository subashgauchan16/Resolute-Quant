-- Historical bars only. This file holds no application records, and can be rebuilt from its source.
-- Prices can be negative, as some futures have traded below zero. A missing volume stays NULL.
-- The upper limit refuses infinity, and NOT NULL refuses NaN, which SQLite stores as NULL.

CREATE TABLE bars (
  dataset_id TEXT NOT NULL,
  symbol     TEXT NOT NULL,
  time_ms    INTEGER NOT NULL,
  open       REAL NOT NULL CHECK (abs(open) < 1e300),
  high       REAL NOT NULL CHECK (abs(high) < 1e300),
  low        REAL NOT NULL CHECK (abs(low) < 1e300),
  close      REAL NOT NULL CHECK (abs(close) < 1e300),
  volume     REAL CHECK (volume IS NULL OR (volume >= 0 AND volume < 1e300)),
  PRIMARY KEY (dataset_id, symbol, time_ms),
  CHECK (high >= open AND high >= close AND low <= open AND low <= close)
) STRICT, WITHOUT ROWID;

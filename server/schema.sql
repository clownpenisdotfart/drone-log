-- Aircraft positions reported by fixed nodes. No operator (pilot) location
-- is ever sent to or stored by the server.
CREATE TABLE IF NOT EXISTS tracks (
  id        INTEGER PRIMARY KEY,
  ts        INTEGER NOT NULL,  -- unix ms, from the node's clock offset at upload
  node      TEXT    NOT NULL,
  drone     TEXT    NOT NULL,  -- UAS ID, or transmitter MAC when no ID was heard
  transport TEXT,
  rssi      INTEGER,
  ua_type   TEXT,
  status    TEXT,
  lat       REAL    NOT NULL,
  lon       REAL    NOT NULL,
  alt_geo   REAL,
  height    REAL,
  speed     REAL,
  dir       REAL
);
CREATE INDEX IF NOT EXISTS tracks_ts ON tracks (ts);
CREATE INDEX IF NOT EXISTS tracks_drone_ts ON tracks (drone, ts);

CREATE TABLE IF NOT EXISTS nodes (
  name      TEXT PRIMARY KEY,
  last_seen INTEGER NOT NULL,
  uploads   INTEGER NOT NULL DEFAULT 0
);

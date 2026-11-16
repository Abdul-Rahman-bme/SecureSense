import { mkdirSync } from 'node:fs';
import path from 'node:path';
import { DatabaseSync } from 'node:sqlite';

// Node's built-in SQLite (no native module to compile).
export function openDb(dbPath) {
  mkdirSync(path.dirname(dbPath), { recursive: true });
  const db = new DatabaseSync(dbPath);
  db.exec(`
    PRAGMA journal_mode = WAL;
    PRAGMA foreign_keys = ON;

    CREATE TABLE IF NOT EXISTS users (
      id            INTEGER PRIMARY KEY,
      username      TEXT NOT NULL UNIQUE COLLATE NOCASE,
      password_hash TEXT NOT NULL,
      created_at    INTEGER NOT NULL
    );

    CREATE TABLE IF NOT EXISTS sessions (
      token_hash TEXT PRIMARY KEY,
      user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
      created_at INTEGER NOT NULL,
      expires_at INTEGER NOT NULL
    );

    -- Alarm/arm events published by the hub. received_at = server clock (ms).
    CREATE TABLE IF NOT EXISTS events (
      id          INTEGER PRIMARY KEY,
      received_at INTEGER NOT NULL,
      node        INTEGER NOT NULL,
      type        TEXT NOT NULL,
      cause       TEXT,
      payload     TEXT NOT NULL
    );
    CREATE INDEX IF NOT EXISTS events_received ON events(received_at DESC);

    -- Audit log: who sent which command and what the hub/C3 answered.
    CREATE TABLE IF NOT EXISTS commands (
      id          TEXT PRIMARY KEY,
      user_id     INTEGER REFERENCES users(id) ON DELETE SET NULL,
      cmd         TEXT NOT NULL,
      sent_at     INTEGER NOT NULL,
      result      TEXT,
      result_at   INTEGER,
      payload     TEXT
    );
    CREATE INDEX IF NOT EXISTS commands_sent ON commands(sent_at DESC);
  `);
  return db;
}

import { mkdirSync } from 'node:fs';
import path from 'node:path';

// One small async interface over two databases:
//   - SQLite (Node's built-in node:sqlite) for local use: no setup, a file in data/
//   - PostgreSQL when DATABASE_URL is set, e.g. a free Neon database for cloud hosting
// SQL is written once with `?` placeholders; the Postgres adapter rewrites them to $1, $2...
//
//   await db.get(sql, params)  -> first row or undefined
//   await db.all(sql, params)  -> array of rows
//   await db.run(sql, params)  -> { changes }
//   await db.close()

const SQLITE_SCHEMA = `
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
`;

// Same tables for Postgres. Times stay as millisecond numbers (BIGINT).
const POSTGRES_SCHEMA = `
  CREATE TABLE IF NOT EXISTS users (
    id            BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    username      TEXT NOT NULL,
    password_hash TEXT NOT NULL,
    created_at    BIGINT NOT NULL
  );
  CREATE UNIQUE INDEX IF NOT EXISTS users_username_nocase ON users (lower(username));

  CREATE TABLE IF NOT EXISTS sessions (
    token_hash TEXT PRIMARY KEY,
    user_id    BIGINT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at BIGINT NOT NULL,
    expires_at BIGINT NOT NULL
  );

  CREATE TABLE IF NOT EXISTS events (
    id          BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    received_at BIGINT NOT NULL,
    node        INTEGER NOT NULL,
    type        TEXT NOT NULL,
    cause       TEXT,
    payload     TEXT NOT NULL
  );
  CREATE INDEX IF NOT EXISTS events_received ON events(received_at DESC);

  CREATE TABLE IF NOT EXISTS commands (
    id          TEXT PRIMARY KEY,
    user_id     BIGINT REFERENCES users(id) ON DELETE SET NULL,
    cmd         TEXT NOT NULL,
    sent_at     BIGINT NOT NULL,
    result      TEXT,
    result_at   BIGINT,
    payload     TEXT
  );
  CREATE INDEX IF NOT EXISTS commands_sent ON commands(sent_at DESC);
`;

/** Opens SQLite, or Postgres when config.databaseUrl is set. */
export async function openDb(config, log = console) {
  return config.databaseUrl ? openPostgres(config.databaseUrl, log) : openSqlite(config.dbPath);
}

async function openSqlite(dbPath) {
  const { DatabaseSync } = await import('node:sqlite');
  mkdirSync(path.dirname(dbPath), { recursive: true });
  const db = new DatabaseSync(dbPath);
  db.exec(SQLITE_SCHEMA);
  const cache = new Map();
  const stmt = (sql) => {
    let s = cache.get(sql);
    if (!s) { s = db.prepare(sql); cache.set(sql, s); }
    return s;
  };
  return {
    kind: 'sqlite',
    describe: `SQLite ${dbPath}`,
    async get(sql, params = []) { return stmt(sql).get(...params); },
    async all(sql, params = []) { return stmt(sql).all(...params); },
    async run(sql, params = []) { return { changes: Number(stmt(sql).run(...params).changes) }; },
    async close() { db.close(); },
  };
}

async function openPostgres(url, log) {
  const { default: pg } = await import('pg');
  // BIGINT (int8) and COUNT(*) come back as strings by default; our values are
  // millisecond timestamps and row ids, which fit safely in a JS number.
  pg.types.setTypeParser(20, (v) => Number(v));

  const pool = new pg.Pool({
    connectionString: url,
    max: 5,
    idleTimeoutMillis: 30_000,
    connectionTimeoutMillis: 15_000, // a free Neon database may need a moment to wake up
  });
  // An idle connection dropped by the server must not crash the process.
  pool.on('error', (err) => log.error?.(`[db] idle connection error: ${err.message}`));

  await pool.query(POSTGRES_SCHEMA);

  const toPg = (sql) => { let i = 0; return sql.replace(/\?/g, () => `$${++i}`); };
  const host = (() => { try { return new URL(url).hostname; } catch { return 'postgres'; } })();
  return {
    kind: 'postgres',
    describe: `Postgres ${host}`,
    async get(sql, params = []) { return (await pool.query(toPg(sql), params)).rows[0]; },
    async all(sql, params = []) { return (await pool.query(toPg(sql), params)).rows; },
    async run(sql, params = []) { return { changes: (await pool.query(toPg(sql), params)).rowCount ?? 0 }; },
    async close() { await pool.end(); },
  };
}

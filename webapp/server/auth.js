import { createHash, randomBytes, scrypt, timingSafeEqual } from 'node:crypto';
import { promisify } from 'node:util';

const scryptAsync = promisify(scrypt);
export const SESSION_COOKIE = 'ss_session';

// ---- password hashing (scrypt, built into Node) ----
export async function hashPassword(password) {
  const salt = randomBytes(16);
  const hash = await scryptAsync(password, salt, 64);
  return `scrypt$${salt.toString('hex')}$${hash.toString('hex')}`;
}

export async function verifyPassword(password, stored) {
  const [scheme, saltHex, hashHex] = String(stored).split('$');
  if (scheme !== 'scrypt' || !saltHex || !hashHex) return false;
  const expected = Buffer.from(hashHex, 'hex');
  const actual = await scryptAsync(password, Buffer.from(saltHex, 'hex'), expected.length);
  return timingSafeEqual(actual, expected);
}

// Compared against when the username does not exist, so response time doesn't reveal it.
const DUMMY_HASH = await hashPassword(randomBytes(16).toString('hex'));

export function validatePasswordPolicy(password) {
  if (typeof password !== 'string' || password.length < 10) return 'Password must be at least 10 characters.';
  if (password.length > 200) return 'Password is too long.';
  return null;
}

// ---- users ----
export function createUserStore(db) {
  const byName = db.prepare('SELECT id, username, password_hash FROM users WHERE username = ?');
  const insert = db.prepare('INSERT INTO users (username, password_hash, created_at) VALUES (?, ?, ?)');
  const setPw = db.prepare('UPDATE users SET password_hash = ? WHERE id = ?');
  const count = db.prepare('SELECT COUNT(*) AS n FROM users');
  return {
    find: (username) => byName.get(username),
    count: () => count.get().n,
    async create(username, password) {
      insert.run(username, await hashPassword(password), Date.now());
    },
    async setPassword(id, password) {
      setPw.run(await hashPassword(password), id);
    },
    async checkLogin(username, password) {
      const user = typeof username === 'string' ? byName.get(username) : undefined;
      const ok = await verifyPassword(String(password ?? ''), user?.password_hash ?? DUMMY_HASH);
      return ok && user ? { id: user.id, username: user.username } : null;
    },
  };
}

// ---- sessions: random token in an HttpOnly cookie, only its SHA-256 stored ----
const sha256 = (s) => createHash('sha256').update(s).digest('hex');

export function createSessionStore(db, { sessionDays }) {
  const ttlMs = sessionDays * 24 * 3600 * 1000;
  const insert = db.prepare('INSERT INTO sessions (token_hash, user_id, created_at, expires_at) VALUES (?, ?, ?, ?)');
  const lookup = db.prepare(`SELECT u.id, u.username FROM sessions s JOIN users u ON u.id = s.user_id
                             WHERE s.token_hash = ? AND s.expires_at > ?`);
  const remove = db.prepare('DELETE FROM sessions WHERE token_hash = ?');
  const purge = db.prepare('DELETE FROM sessions WHERE expires_at <= ?');
  const removeForUser = db.prepare('DELETE FROM sessions WHERE user_id = ?');
  purge.run(Date.now());
  return {
    ttlMs,
    create(userId) {
      const token = randomBytes(32).toString('base64url');
      const now = Date.now();
      insert.run(sha256(token), userId, now, now + ttlMs);
      return token;
    },
    userFor(token) {
      if (!token) return null;
      return lookup.get(sha256(token), Date.now()) ?? null;
    },
    destroy(token) {
      if (token) remove.run(sha256(token));
    },
    destroyAllFor(userId) {
      removeForUser.run(userId);
    },
  };
}

// ---- brute-force protection: 5 failures per username+IP per 15 minutes ----
export function createLoginLimiter({ maxFailures = 5, windowMs = 15 * 60 * 1000 } = {}) {
  const failures = new Map();
  const key = (ip, username) => `${ip}|${String(username).toLowerCase()}`;
  return {
    isBlocked(ip, username) {
      const f = failures.get(key(ip, username));
      if (!f) return false;
      if (Date.now() - f.first > windowMs) { failures.delete(key(ip, username)); return false; }
      return f.count >= maxFailures;
    },
    fail(ip, username) {
      const k = key(ip, username);
      const f = failures.get(k);
      if (!f || Date.now() - f.first > windowMs) failures.set(k, { count: 1, first: Date.now() });
      else f.count++;
    },
    reset(ip, username) { failures.delete(key(ip, username)); },
  };
}

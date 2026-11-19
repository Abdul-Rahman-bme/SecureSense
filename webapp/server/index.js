import { existsSync } from 'node:fs';
import Fastify from 'fastify';
import fastifyCookie from '@fastify/cookie';
import fastifyStatic from '@fastify/static';
import fastifyWebsocket from '@fastify/websocket';
import { config } from './config.js';
import { openDb } from './db.js';
import {
  SESSION_COOKIE, createLoginLimiter, createSessionStore, createUserStore, validatePasswordPolicy,
} from './auth.js';
import { COMMANDS, createHubBridge } from './hub.js';

const app = Fastify({
  logger: { level: process.env.LOG_LEVEL ?? 'info' },
  bodyLimit: 16 * 1024,
  trustProxy: process.env.TRUST_PROXY === 'true',
});

const db = await openDb(config, app.log);
app.log.info(`Database: ${db.describe}`);
const users = createUserStore(db);
const sessions = createSessionStore(db, config);
const limiter = createLoginLimiter();
const hub = createHubBridge({ config, db, log: app.log });

// Drop expired sessions now and every 6 hours.
const purgeSessions = () => sessions.purgeExpired().catch((err) => app.log.error(`[db] session purge: ${err.message}`));
purgeSessions();
setInterval(purgeSessions, 6 * 3600 * 1000).unref();

if (await users.count() === 0) {
  if (config.bootstrapUsername && config.bootstrapPassword) {
    const problem = validatePasswordPolicy(config.bootstrapPassword);
    if (problem) throw new Error(`BOOTSTRAP_PASSWORD: ${problem}`);
    await users.create(config.bootstrapUsername, config.bootstrapPassword);
    app.log.info(`Created first user "${config.bootstrapUsername}" from BOOTSTRAP_* settings.`);
  } else {
    app.log.warn('No users yet. Create one with:  npm run user -- add <username>');
  }
}

await app.register(fastifyCookie);
await app.register(fastifyWebsocket, { options: { maxPayload: 4096 } });

// ---- security headers ----
app.addHook('onSend', async (_req, reply) => {
  reply.header('X-Content-Type-Options', 'nosniff');
  reply.header('X-Frame-Options', 'DENY');
  reply.header('Referrer-Policy', 'same-origin');
});

// ---- CSRF: session cookie is SameSite=Strict, and every state-changing API call
// must be application/json (a cross-site form cannot send that without CORS). ----
app.addHook('onRequest', async (req, reply) => {
  if (req.method !== 'GET' && req.method !== 'HEAD' && req.url.startsWith('/api/')) {
    if (!String(req.headers['content-type'] ?? '').startsWith('application/json')) {
      return reply.code(415).send({ error: 'Content-Type must be application/json' });
    }
  }
});

function currentUser(req) {
  return sessions.userFor(req.cookies[SESSION_COOKIE]);
}
async function requireUser(req, reply) {
  const user = await currentUser(req);
  if (!user) return reply.code(401).send({ error: 'Not signed in' });
  req.user = user;
}
function setSessionCookie(reply, token) {
  reply.setCookie(SESSION_COOKIE, token, {
    httpOnly: true,
    sameSite: 'strict',
    secure: config.cookieSecure,
    path: '/',
    maxAge: Math.floor(sessions.ttlMs / 1000),
  });
}

// ---- auth routes ----
app.post('/api/login', {
  schema: {
    body: {
      type: 'object',
      required: ['username', 'password'],
      properties: { username: { type: 'string', maxLength: 64 }, password: { type: 'string', maxLength: 200 } },
    },
  },
}, async (req, reply) => {
  const { username, password } = req.body;
  if (limiter.isBlocked(req.ip, username)) {
    return reply.code(429).send({ error: 'Too many failed attempts. Try again in 15 minutes.' });
  }
  const user = await users.checkLogin(username, password);
  if (!user) {
    limiter.fail(req.ip, username);
    return reply.code(401).send({ error: 'Wrong username or password' });
  }
  limiter.reset(req.ip, username);
  setSessionCookie(reply, await sessions.create(user.id));
  return { user: { username: user.username } };
});

app.post('/api/logout', async (req, reply) => {
  await sessions.destroy(req.cookies[SESSION_COOKIE]);
  reply.clearCookie(SESSION_COOKIE, { path: '/' });
  return { ok: true };
});

app.get('/api/me', { preHandler: requireUser }, async (req) => ({ user: { username: req.user.username } }));

// ---- health check for hosting platforms / uptime monitors (no login, no database) ----
app.get('/healthz', async () => ({ ok: true }));

// ---- data routes ----
app.get('/api/state', { preHandler: requireUser }, async () => hub.snapshot());

app.get('/api/events', {
  preHandler: requireUser,
  schema: {
    querystring: {
      type: 'object',
      properties: {
        limit: { type: 'integer', minimum: 1, maximum: 200, default: 50 },
        before: { type: 'integer', minimum: 1 },
      },
    },
  },
}, async (req) => ({ events: await hub.events(req.query.limit, req.query.before) }));

app.get('/api/commands', { preHandler: requireUser }, async () => ({ commands: await hub.commands(50) }));

app.post('/api/commands', {
  preHandler: requireUser,
  schema: {
    body: {
      type: 'object',
      required: ['cmd'],
      properties: { cmd: { type: 'string', enum: COMMANDS } },
      additionalProperties: false,
    },
  },
}, async (req) => {
  req.log.info({ user: req.user.username, cmd: req.body.cmd }, 'command requested');
  return hub.sendCommand(req.body.cmd, req.user.id);
});

// ---- live updates over WebSocket ----
const sockets = new Set();
function broadcast(msg) {
  const data = JSON.stringify(msg);
  for (const ws of sockets) if (ws.readyState === 1) ws.send(data);
}
hub.on('state', (state) => broadcast({ type: 'state', state }));
hub.on('event', (event) => broadcast({ type: 'event', event }));
hub.on('command', (command) => broadcast({ type: 'command', command }));

app.get('/ws', { websocket: true }, async (socket, req) => {
  socket.on('message', () => {}); // client → server messages are not used
  let user;
  try {
    user = await currentUser(req);
  } catch (err) {
    req.log.error(`[ws] session check failed: ${err.message}`);
    socket.close(1011, 'Server error');
    return;
  }
  if (!user) { socket.close(4401, 'Not signed in'); return; }
  if (socket.readyState !== 1) return; // browser left while we were checking

  // Re-check the session periodically so a logout elsewhere ends the stream.
  const recheck = setInterval(() => {
    currentUser(req)
      .then((u) => { if (!u) socket.close(4401, 'Session ended'); })
      .catch((err) => req.log.warn(`[ws] session re-check failed: ${err.message}`));
  }, 60_000);
  socket.on('close', () => { clearInterval(recheck); sockets.delete(socket); });

  // Subscribe before loading history so no live update is missed in between.
  sockets.add(socket);
  let events = [];
  try {
    events = await hub.events(50);
  } catch (err) {
    req.log.error(`[ws] could not load event history: ${err.message}`);
  }
  if (socket.readyState === 1) {
    socket.send(JSON.stringify({ type: 'snapshot', state: hub.snapshot(), events }));
  }
});

// ---- built frontend (npm run build) ----
if (existsSync(config.distDir)) {
  await app.register(fastifyStatic, { root: config.distDir, wildcard: false });
  app.setNotFoundHandler((req, reply) => {
    if (req.method === 'GET' && !req.url.startsWith('/api/') && !req.url.startsWith('/ws')) {
      return reply.sendFile('index.html');
    }
    return reply.code(404).send({ error: 'Not found' });
  });
}

const shutdown = async () => {
  await app.close();
  await hub.close();
  await db.close();
  process.exit(0);
};
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);

await app.listen({ host: config.host, port: config.port });
app.log.info(`SecureSense web: http://${config.host === '0.0.0.0' ? 'localhost' : config.host}:${config.port}  (MQTT ${config.mqttUrl}, prefix ${config.topicPrefix})`);

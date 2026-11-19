import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

export const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

// Load webapp/.env if present (Node >= 22 built-in; no dotenv dependency).
const envFile = path.join(ROOT, '.env');
if (existsSync(envFile)) process.loadEnvFile(envFile);

const env = process.env;

export const config = {
  host: env.HOST ?? '127.0.0.1',
  port: Number(env.PORT ?? 3000),

  // Broker. Local dev default = `npm run broker` (aedes on 1883).
  // EMQX Cloud: mqtts://<deployment-host>:8883 (Node trusts its public CA by default).
  mqttUrl: env.MQTT_URL ?? 'mqtt://127.0.0.1:1883',
  mqttUsername: env.MQTT_USERNAME || undefined,
  mqttPassword: env.MQTT_PASSWORD || undefined,
  topicPrefix: (env.MQTT_TOPIC_PREFIX ?? 'securesense/hub1').replace(/\/+$/, ''),
  nodeId: Number(env.NODE_ID ?? 2),

  // Database: SQLite file by default; set DATABASE_URL (postgres://...) to use Postgres instead,
  // e.g. a free Neon database when the app is hosted somewhere without a permanent disk.
  databaseUrl: env.DATABASE_URL || undefined,
  dbPath: path.resolve(ROOT, env.DB_PATH ?? 'data/securesense.db'),

  // Set true when served over HTTPS so the session cookie is never sent in clear text.
  cookieSecure: env.COOKIE_SECURE === 'true',
  sessionDays: Number(env.SESSION_DAYS ?? 7),

  // Creates this user on first start if the users table is empty.
  bootstrapUsername: env.BOOTSTRAP_USERNAME || undefined,
  bootstrapPassword: env.BOOTSTRAP_PASSWORD || undefined,

  // Hub waits 4 s for the C3; allow for broker round trips on top.
  commandTimeoutMs: Number(env.COMMAND_TIMEOUT_MS ?? 8000),

  distDir: path.join(ROOT, 'dist'),
};

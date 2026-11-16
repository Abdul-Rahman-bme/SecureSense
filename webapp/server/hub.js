import { EventEmitter } from 'node:events';
import { randomBytes, randomUUID } from 'node:crypto';
import mqtt from 'mqtt';

// Commands the hub firmware accepts over MQTT (LED/buzzer tests are Serial-only).
export const COMMANDS = ['ping', 'status', 'arm', 'disarm', 'silence', 'rearm'];

function parseJson(buf) {
  try {
    const v = JSON.parse(buf.toString('utf8'));
    return v && typeof v === 'object' && !Array.isArray(v) ? v : null;
  } catch {
    return null;
  }
}

/**
 * Bridges the S3 hub's MQTT topics to the web app.
 * Emits: 'state' (snapshot), 'event' (stored event row), 'command' (stored command row).
 */
export function createHubBridge({ config, db, log }) {
  const bus = new EventEmitter();
  const prefix = config.topicPrefix;
  const nodeBase = `${prefix}/node/${config.nodeId}`;
  const topics = {
    hubOnline: `${prefix}/hub/online`,
    hubStatus: `${prefix}/hub/status`,
    nodeOnline: `${nodeBase}/online`,
    nodeStatus: `${nodeBase}/status`,
    nodeEvent: `${nodeBase}/event`,
    nodeCmd: `${nodeBase}/cmd`,
    nodeCmdResult: `${nodeBase}/cmd_result`,
  };

  const state = {
    brokerConnected: false,
    hubOnline: null,        // null = unknown (no retained message yet)
    nodeOnline: null,
    hubStatus: null,
    hubStatusAt: null,
    nodeStatus: null,
    nodeStatusAt: null,     // server receive time (ms)
  };
  const snapshot = () => structuredClone(state);
  const changed = () => bus.emit('state', snapshot());

  const insertEvent = db.prepare(
    'INSERT INTO events (received_at, node, type, cause, payload) VALUES (?, ?, ?, ?, ?) RETURNING *');
  const recentEvents = db.prepare(
    'SELECT id, received_at, node, type, cause FROM events WHERE id < ? ORDER BY id DESC LIMIT ?');
  const insertCommand = db.prepare(
    'INSERT INTO commands (id, user_id, cmd, sent_at) VALUES (?, ?, ?, ?)');
  const finishCommand = db.prepare(
    'UPDATE commands SET result = ?, result_at = ?, payload = ? WHERE id = ? AND result IS NULL');
  const recentCommands = db.prepare(`
    SELECT c.id, c.cmd, c.sent_at, c.result, c.result_at, u.username
    FROM commands c LEFT JOIN users u ON u.id = c.user_id
    ORDER BY c.sent_at DESC LIMIT ?`);

  /** @type {Map<string, {resolve: Function, timer: NodeJS.Timeout}>} */
  const pending = new Map();

  const client = mqtt.connect(config.mqttUrl, {
    username: config.mqttUsername,
    password: config.mqttPassword,
    clientId: `securesense-web-${randomBytes(4).toString('hex')}`,
    clean: true,
    reconnectPeriod: 3000,
    connectTimeout: 10_000,
    keepalive: 30,
  });

  client.on('connect', () => {
    state.brokerConnected = true;
    log.info(`[mqtt] connected to ${config.mqttUrl}`);
    client.subscribe(
      [topics.hubOnline, topics.hubStatus, topics.nodeOnline, topics.nodeStatus, topics.nodeEvent, topics.nodeCmdResult],
      { qos: 1 },
      (err) => err && log.error(`[mqtt] subscribe failed: ${err.message}`),
    );
    changed();
  });
  client.on('close', () => {
    if (state.brokerConnected) log.warn('[mqtt] disconnected; retrying');
    state.brokerConnected = false;
    changed();
  });
  client.on('error', (err) => log.error(`[mqtt] ${err.message}`));

  client.on('message', (topic, payload, packet) => {
    const now = Date.now();
    switch (topic) {
      case topics.hubOnline:
        state.hubOnline = payload.toString() === 'online';
        changed();
        break;
      case topics.nodeOnline:
        state.nodeOnline = payload.toString() === 'online';
        changed();
        break;
      case topics.hubStatus: {
        const j = parseJson(payload);
        if (j) { state.hubStatus = j; state.hubStatusAt = now; changed(); }
        break;
      }
      case topics.nodeStatus: {
        const j = parseJson(payload);
        if (j) { state.nodeStatus = j; state.nodeStatusAt = now; changed(); }
        break;
      }
      case topics.nodeEvent: {
        if (packet.retain) break; // events are never retained by the hub; ignore stray retained junk
        const j = parseJson(payload);
        if (!j || typeof j.type !== 'string') break;
        const row = insertEvent.get(now, Number(j.node ?? config.nodeId), j.type.slice(0, 40),
          typeof j.cause === 'string' ? j.cause.slice(0, 20) : null, JSON.stringify(j));
        bus.emit('event', { id: row.id, received_at: row.received_at, node: row.node, type: row.type, cause: row.cause });
        break;
      }
      case topics.nodeCmdResult: {
        const j = parseJson(payload);
        if (!j || typeof j.id !== 'string' || typeof j.result !== 'string') break;
        settle(j.id, j.result, j);
        break;
      }
    }
  });

  function settle(id, result, payload) {
    const changes = finishCommand.run(result, Date.now(), payload ? JSON.stringify(payload) : null, id).changes;
    const p = pending.get(id);
    if (p) {
      clearTimeout(p.timer);
      pending.delete(id);
      p.resolve({ id, result, state: payload ?? null });
    }
    if (changes) bus.emit('command', { id, result });
  }

  /**
   * Publish a command and wait for the hub's correlated result.
   * Resolves with {id, result, state}. result is a hub/C3 code, or
   * BROKER_OFFLINE / NO_RESPONSE when the backend never heard back.
   */
  function sendCommand(cmd, userId) {
    if (!COMMANDS.includes(cmd)) return Promise.resolve({ id: null, result: 'UNSUPPORTED_COMMAND', state: null });
    const id = randomUUID(); // 36 chars of [0-9a-f-]: matches the hub's id rules
    insertCommand.run(id, userId ?? null, cmd, Date.now());
    if (!client.connected) {
      settle(id, 'BROKER_OFFLINE', null);
      return Promise.resolve({ id, result: 'BROKER_OFFLINE', state: null });
    }
    return new Promise((resolve) => {
      const timer = setTimeout(() => settle(id, 'NO_RESPONSE', null), config.commandTimeoutMs);
      pending.set(id, { resolve, timer });
      // retain MUST be false: the hub ignores retained commands by design.
      client.publish(topics.nodeCmd, JSON.stringify({ id, cmd }), { qos: 1, retain: false }, (err) => {
        if (err) { log.error(`[mqtt] publish failed: ${err.message}`); settle(id, 'BROKER_OFFLINE', null); }
      });
    });
  }

  return {
    on: bus.on.bind(bus),
    snapshot,
    sendCommand,
    events: (limit = 50, beforeId = Number.MAX_SAFE_INTEGER) => recentEvents.all(beforeId, limit),
    commands: (limit = 50) => recentCommands.all(limit),
    close: () => new Promise((r) => client.end(false, {}, r)),
    topics,
  };
}

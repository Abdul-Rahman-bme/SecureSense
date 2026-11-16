// Simulated S3 hub + C3 node. Speaks exactly the Stage 8 MQTT topics/payloads, so the
// web app can be developed and tested without hardware.
//   npm run sim
// Keys (in this terminal): d = open/close door, m = motion pulse, b = toggle node button,
//                          n = node offline/online, h = hub offline/online, q = quit
// Automation: publish to <prefix>/sim/control: door_open | door_close | motion |
//             node_offline | node_online   (simulator only; the real hub ignores it)
import mqtt from 'mqtt';
import { config } from '../server/config.js';

const P = config.topicPrefix;
const N = `${P}/node/${config.nodeId}`;
const PIR_WARMUP_MS = Number(process.env.SIM_PIR_WARMUP_MS ?? 10_000); // real node: 60 s
const REARM_REED_CLOSED_MS = 2000;
const REARM_PIR_CLEAR_MS = 10_000;
const MOTION_PULSE_MS = 3000;

const boot = Date.now();
let seq = 0;
let nodeOnline = true;
const s = {
  armed: false, alarm: false, silenced: false, cause: 'NONE',
  reedEnabled: true, reedClosed: true, pirEnabled: true, pirReady: false, pirMotion: false,
  button: false,
};
let reedClosedSince = Date.now();
let pirClearSince = Date.now();
let motionTimer;

const ts = () => Math.floor(Date.now() / 1000);
const uptimeMs = () => Date.now() - boot;

const client = mqtt.connect(config.mqttUrl, {
  username: config.mqttUsername,
  password: config.mqttPassword,
  clientId: `securesense-sim-${Math.random().toString(16).slice(2, 8)}`,
  will: { topic: `${P}/hub/online`, payload: 'offline', qos: 1, retain: true },
});
const pub = (topic, payload, opts = { qos: 1 }) =>
  client.publish(topic, typeof payload === 'string' ? payload : JSON.stringify(payload), opts);

function statusJson() {
  return {
    node: config.nodeId, seq: ++seq, uptimeMs: uptimeMs(),
    armed: s.armed, alarm: s.alarm, silenced: s.silenced, cause: s.cause,
    reedEnabled: s.reedEnabled, reedClosed: s.reedClosed, pirEnabled: s.pirEnabled, pirReady: s.pirReady,
    pirMotion: s.pirMotion, button: s.button,
    redLed: s.armed || s.alarm, greenLed: !s.armed && !s.alarm, buzzer: s.alarm && !s.silenced,
    rssi: -48 - Math.round(Math.random() * 6), ts: ts(),
  };
}
function publishStatus() {
  if (nodeOnline) pub(`${N}/status`, statusJson(), { qos: 1, retain: true });
}
function publishHubStatus() {
  pub(`${P}/hub/status`, {
    uptimeMs: uptimeMs(), protocol: 2, wifiRssi: -55, channel: 6, nodeKnown: true,
    nodeMac: 'AA:BB:CC:DD:EE:02', nodeLinkAgeMs: nodeOnline ? 400 : 12000, packetsReceived: seq, ts: ts(),
  }, { qos: 0, retain: true });
}
function event(type) {
  pub(`${N}/event`, { node: config.nodeId, type, cause: s.cause, seq: ++seq, uptimeMs: uptimeMs(), ts: ts() });
  console.log(`[sim] EVENT ${type}${type === 'ALARM' ? ` (${s.cause})` : ''}`);
}
function trigger(cause) {
  if (!s.armed || s.alarm) return;
  s.alarm = true; s.silenced = false; s.cause = cause;
  event('ALARM');
}

function runCommand(cmd) {
  const now = Date.now();
  switch (cmd) {
    case 'ping': case 'status': return 'ACCEPTED';
    case 'arm':
      if (s.armed) return 'ALREADY_ARMED';
      if (!s.reedEnabled && !s.pirEnabled) return 'NO_SENSORS_SELECTED';
      if (s.pirEnabled && !s.pirReady) return 'PIR_NOT_READY';
      if (s.reedEnabled && !s.reedClosed) return 'REED_OPEN';
      Object.assign(s, { armed: true, alarm: false, silenced: false, cause: 'NONE' });
      event('ARMED');
      return 'ACCEPTED';
    case 'disarm': {
      const was = s.armed;
      Object.assign(s, { armed: false, alarm: false, silenced: false, cause: 'NONE' });
      if (was) event('DISARMED');
      return 'ACCEPTED';
    }
    case 'silence':
      if (!s.alarm) return 'NO_LATCHED_ALARM';
      if (!s.silenced) { s.silenced = true; event('SILENCED'); }
      return 'ACCEPTED';
    case 'rearm':
      if (!s.armed) return 'NOT_ARMED';
      if (!s.alarm) return 'NO_LATCHED_ALARM';
      if (s.button) return 'LOCAL_BUTTON_HELD';
      if (s.reedEnabled) {
        if (!s.reedClosed) return 'REED_OPEN';
        if (now - reedClosedSince < REARM_REED_CLOSED_MS) return 'REED_NOT_STABLE';
      }
      if (s.pirEnabled) {
        if (!s.pirReady) return 'PIR_NOT_READY';
        if (s.pirMotion) return 'PIR_STILL_ACTIVE';
        if (now - pirClearSince < REARM_PIR_CLEAR_MS) return 'PIR_NOT_CLEAR_LONG_ENOUGH';
      }
      Object.assign(s, { alarm: false, silenced: false, cause: 'NONE' });
      event('ALARM_RESET_REARMED');
      return 'ACCEPTED';
    default: return 'UNKNOWN_COMMAND';
  }
}

let busy = false;
let lastId = '';
function onCommand(buf, packet) {
  if (packet.retain) return; // same rule as the real hub
  let msg;
  try { msg = JSON.parse(buf.toString()); } catch { return; }
  const { id, cmd } = msg ?? {};
  if (typeof id !== 'string' || !/^[A-Za-z0-9_-]{1,40}$/.test(id)) return;
  if (id === lastId) return;
  lastId = id;
  const reply = (result, withState = true) => {
    pub(`${N}/cmd_result`, {
      id, cmd: typeof cmd === 'string' ? cmd : 'none', result,
      ...(withState ? { armed: s.armed, alarm: s.alarm, silenced: s.silenced, cause: s.cause } : {}),
      ts: ts(),
    });
    console.log(`[sim] cmd ${cmd} -> ${result}`);
  };
  if (!['ping', 'status', 'arm', 'disarm', 'silence', 'rearm'].includes(cmd)) return reply('UNSUPPORTED_COMMAND', false);
  if (!nodeOnline) return reply('NODE_OFFLINE');
  if (busy) return reply('HUB_BUSY');
  busy = true;
  setTimeout(() => { // ESP-NOW round trip
    busy = false;
    const result = runCommand(cmd);
    reply(result);
    publishStatus();
  }, 150 + Math.random() * 150);
}

function control(action) {
  switch (action) {
    case 'door_open':
      if (!s.reedClosed) return;
      s.reedClosed = false;
      console.log('[sim] door OPEN');
      if (s.reedEnabled) trigger('REED');
      break;
    case 'door_close':
      if (s.reedClosed) return;
      s.reedClosed = true; reedClosedSince = Date.now();
      console.log('[sim] door CLOSED');
      break;
    case 'motion':
      if (!s.pirReady) { console.log('[sim] PIR still warming up; motion ignored'); return; }
      s.pirMotion = true;
      console.log('[sim] MOTION');
      if (s.pirEnabled) trigger('PIR');
      clearTimeout(motionTimer);
      motionTimer = setTimeout(() => { s.pirMotion = false; pirClearSince = Date.now(); publishStatus(); }, MOTION_PULSE_MS);
      break;
    case 'button':
      s.button = !s.button;
      console.log(`[sim] button ${s.button ? 'PRESSED' : 'released'}`);
      break;
    case 'node_offline':
      nodeOnline = false;
      pub(`${N}/online`, 'offline', { qos: 1, retain: true });
      console.log('[sim] node OFFLINE');
      return;
    case 'node_online':
      nodeOnline = true;
      pub(`${N}/online`, 'online', { qos: 1, retain: true });
      console.log('[sim] node ONLINE');
      break;
    default: return;
  }
  publishStatus();
}

client.on('connect', () => {
  console.log(`[sim] connected to ${config.mqttUrl}, prefix ${P}`);
  client.subscribe([`${N}/cmd`, `${P}/sim/control`], { qos: 1 });
  pub(`${P}/hub/online`, 'online', { qos: 1, retain: true });
  pub(`${N}/online`, nodeOnline ? 'online' : 'offline', { qos: 1, retain: true });
  publishHubStatus();
  publishStatus();
});
client.on('message', (topic, buf, packet) => {
  if (topic === `${N}/cmd`) onCommand(buf, packet);
  else if (topic === `${P}/sim/control`) control(buf.toString().trim());
});
client.on('error', (e) => console.error(`[sim] ${e.message}`));

setTimeout(() => { s.pirReady = true; console.log('[sim] PIR warm-up done'); publishStatus(); }, PIR_WARMUP_MS);
setInterval(publishStatus, 15_000);
setInterval(publishHubStatus, 30_000);

let hubUp = true;
if (process.stdin.isTTY) {
  process.stdin.setRawMode(true);
  process.stdin.setEncoding('utf8');
  process.stdin.on('data', (k) => {
    if (k === 'q' || k === '\u0003') {
      pub(`${P}/hub/online`, 'offline', { qos: 1, retain: true });
      client.end(false, {}, () => process.exit(0));
    } else if (k === 'd') control(s.reedClosed ? 'door_open' : 'door_close');
    else if (k === 'm') control('motion');
    else if (k === 'b') control('button');
    else if (k === 'n') control(nodeOnline ? 'node_offline' : 'node_online');
    else if (k === 'h') {
      hubUp = !hubUp;
      pub(`${P}/hub/online`, hubUp ? 'online' : 'offline', { qos: 1, retain: true });
      console.log(`[sim] hub ${hubUp ? 'ONLINE' : 'OFFLINE'}`);
    }
  });
  console.log('[sim] keys: d=door  m=motion  b=button  n=node on/off  h=hub on/off  q=quit');
}

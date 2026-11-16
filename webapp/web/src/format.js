export function relativeTime(ms, now = Date.now()) {
  if (!ms) return '—';
  const s = Math.max(0, Math.round((now - ms) / 1000));
  if (s < 5) return 'just now';
  if (s < 60) return `${s} s ago`;
  const m = Math.round(s / 60);
  if (m < 60) return `${m} min ago`;
  const h = Math.round(m / 60);
  if (h < 24) return `${h} h ago`;
  return new Date(ms).toLocaleDateString();
}

export const absoluteTime = (ms) => (ms ? new Date(ms).toLocaleString() : '');

export function signalQuality(rssi) {
  if (rssi == null || rssi === 0) return { label: 'Unknown', level: 0 };
  if (rssi >= -60) return { label: 'Excellent', level: 4 };
  if (rssi >= -70) return { label: 'Good', level: 3 };
  if (rssi >= -80) return { label: 'Fair', level: 2 };
  return { label: 'Weak', level: 1 };
}

export function formatUptime(ms) {
  if (!ms) return '—';
  const s = Math.floor(ms / 1000);
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  if (d) return `${d}d ${h}h`;
  if (h) return `${h}h ${m}m`;
  return `${m}m ${s % 60}s`;
}

const CAUSE = { REED: 'door opened', PIR: 'motion detected' };
export const causeText = (cause) => CAUSE[cause] ?? null;

export const EVENT_INFO = {
  ARMED: { title: 'System armed', tone: 'armed', icon: 'shield' },
  DISARMED: { title: 'System disarmed', tone: 'ok', icon: 'unlock' },
  ALARM: { title: 'ALARM', tone: 'alarm', icon: 'bell' },
  ALARM_RESET_REARMED: { title: 'Alarm reset — still armed', tone: 'armed', icon: 'shieldCheck' },
  SILENCED: { title: 'Buzzer silenced', tone: 'muted', icon: 'mute' },
  NODE_REBOOTED: { title: 'Sensor node restarted (now disarmed)', tone: 'warn', icon: 'power' },
};

const SUCCESS = {
  arm: 'System armed.',
  disarm: 'System disarmed.',
  silence: 'Buzzer silenced. The alarm stays latched until reset or disarm.',
  rearm: 'Alarm reset. The system is still armed and monitoring.',
  ping: 'Hub and sensor node both responded.',
  status: 'Status refreshed.',
};

const REASONS = {
  REED_OPEN: 'The door is open. Close it first.',
  REED_NOT_STABLE: 'The door must stay closed for 2 seconds first.',
  PIR_STILL_ACTIVE: 'The motion sensor is still active. Wait until it clears.',
  PIR_NOT_CLEAR_LONG_ENOUGH: 'The motion sensor must be clear for 10 seconds first.',
  PIR_NOT_READY: 'The motion sensor is still warming up (60 s after power-on).',
  DIP_CHANGED_WHILE_ARMED: 'The sensor DIP switches changed since arming. Restore them first.',
  NO_SENSORS_SELECTED: 'No sensors are selected on the DIP switches.',
  ALREADY_ARMED: 'The system is already armed.',
  NOT_ARMED: 'The system is not armed.',
  NO_LATCHED_ALARM: 'There is no alarm to clear.',
  LOCAL_BUTTON_HELD: "The node's button is being held. Release it first.",
  UNKNOWN_COMMAND: 'The node does not support this command.',
  UNSUPPORTED_COMMAND: 'This command is not supported.',
  HUB_BUSY: 'The hub is busy with another command. Try again in a few seconds.',
  NODE_OFFLINE: "The hub can't reach the sensor node.",
  HUB_RADIO_DOWN: "The hub's radio is not ready.",
  SEND_FAILED: "The hub couldn't send to the node. Try again.",
  INVALID_REQUEST: 'The hub rejected the request as invalid.',
  TIMEOUT_UNKNOWN: 'No reply from the sensor node. Check the status before retrying.',
  NO_RESPONSE: 'No reply from the hub. It may be offline; check the status before retrying.',
  BROKER_OFFLINE: "The server isn't connected to the MQTT broker.",
};

/** -> { ok: boolean, uncertain: boolean, text } */
export function describeResult(cmd, result) {
  if (result === 'ACCEPTED') return { ok: true, uncertain: false, text: SUCCESS[cmd] ?? 'Done.' };
  const uncertain = result === 'TIMEOUT_UNKNOWN' || result === 'NO_RESPONSE';
  return { ok: false, uncertain, text: REASONS[result] ?? `Rejected: ${result}` };
}

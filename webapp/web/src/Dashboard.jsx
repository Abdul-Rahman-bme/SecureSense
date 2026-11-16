import { useState } from 'react';
import { api } from './api.js';
import { useLive, useNow } from './useLive.js';
import Icon from './Icon.jsx';
import {
  EVENT_INFO, absoluteTime, causeText, describeResult, formatUptime, relativeTime, signalQuality,
} from './format.js';

function linkProblem(connected, state) {
  if (!connected) return { tone: 'warn', text: 'Reconnecting to the SecureSense server…' };
  if (!state) return { tone: 'muted', text: 'Loading…' };
  if (!state.brokerConnected) return { tone: 'warn', text: "The server can't reach the MQTT broker. Status may be out of date." };
  if (state.hubOnline === false) return { tone: 'warn', text: 'The hub is offline. Status may be out of date.' };
  if (state.nodeOnline === false) return { tone: 'warn', text: "The hub can't hear the sensor node. Status may be out of date." };
  if (!state.nodeStatus) return { tone: 'muted', text: 'Waiting for the first status from the hub…' };
  return null;
}

function Pill({ label, ok }) {
  const cls = ok === true ? 'pill-ok' : ok === false ? 'pill-bad' : 'pill-unknown';
  return (
    <span className={`pill ${cls}`} title={ok === null ? 'Unknown' : ok ? 'Online' : 'Offline'}>
      <span className="dot" />
      {label}
    </span>
  );
}

function Hero({ s, stale, now, nodeStatusAt }) {
  if (!s) {
    return (
      <section className="card hero hero-unknown">
        <Icon name="shield" size={44} />
        <div>
          <h1>No status yet</h1>
          <p>Waiting for the hub to report.</p>
        </div>
      </section>
    );
  }
  const mode = s.alarm ? 'alarm' : s.armed ? 'armed' : 'disarmed';
  const monitoring = [s.reedEnabled && 'door', s.pirEnabled && 'motion'].filter(Boolean).join(' + ') || 'no sensors';
  const content = {
    alarm: {
      icon: 'bell',
      title: 'ALARM',
      sub: `${causeText(s.cause) ? `Triggered: ${causeText(s.cause)}` : 'Triggered'}${s.silenced ? ' · buzzer silenced' : ''}`,
    },
    armed: { icon: 'shieldCheck', title: 'Armed', sub: `Monitoring ${monitoring}` },
    disarmed: { icon: 'unlock', title: 'Disarmed', sub: 'Not monitoring' },
  }[mode];
  return (
    <section className={`card hero hero-${mode}${stale ? ' hero-stale' : ''}`} aria-live="polite">
      <Icon name={content.icon} size={44} />
      <div>
        <h1>{content.title}</h1>
        <p>{content.sub}</p>
      </div>
      <span className="hero-time" title={absoluteTime(nodeStatusAt)}>Updated {relativeTime(nodeStatusAt, now)}</span>
    </section>
  );
}

function Controls({ s, state }) {
  const [pending, setPending] = useState(null);
  const [outcome, setOutcome] = useState(null);
  const canSend = Boolean(state?.brokerConnected) && !pending;

  async function send(cmd) {
    setPending(cmd);
    setOutcome(null);
    try {
      const r = await api.command(cmd);
      setOutcome({ cmd, ...describeResult(cmd, r.result), code: r.result });
    } catch (err) {
      setOutcome({ cmd, ok: false, uncertain: false, text: err.message, code: 'ERROR' });
    } finally {
      setPending(null);
    }
  }

  const buttons = [
    { cmd: 'arm', label: 'Arm', icon: 'shield', cls: 'btn-primary', enabled: s ? !s.armed : true },
    { cmd: 'disarm', label: 'Disarm', icon: 'unlock', cls: 'btn-secondary', enabled: s ? s.armed : true },
    { cmd: 'silence', label: 'Silence', icon: 'mute', cls: 'btn-secondary', enabled: s ? s.alarm && !s.silenced : true },
    { cmd: 'rearm', label: 'Reset & re-arm', icon: 'shieldCheck', cls: 'btn-danger', enabled: s ? s.alarm && s.armed : true },
  ];

  return (
    <section className="card">
      <div className="card-head">
        <h2>Controls</h2>
        <button className="btn btn-ghost btn-sm" disabled={!canSend} onClick={() => send('ping')} title="Check the hub and node respond">
          <Icon name="refresh" size={16} /> {pending === 'ping' ? 'Checking…' : 'Check link'}
        </button>
      </div>
      <div className="controls">
        {buttons.map((b) => (
          <button key={b.cmd} className={`btn ${b.cls}`} disabled={!canSend || !b.enabled} onClick={() => send(b.cmd)}>
            <Icon name={b.icon} size={18} />
            {pending === b.cmd ? 'Waiting for node…' : b.label}
          </button>
        ))}
      </div>
      {outcome && (
        <div
          className={`notice ${outcome.ok ? 'notice-ok' : outcome.uncertain ? 'notice-warn' : 'notice-alarm'}`}
          role="status"
        >
          {outcome.text} <span className="code">{outcome.code}</span>
        </div>
      )}
      {s?.alarm && s.armed && !pending && (
        <p className="hint">
          Reset needs the door closed for 2 s and motion clear for 10 s. A clear sensor does not prove nobody is inside.
        </p>
      )}
    </section>
  );
}

function Sensor({ icon, label, value, tone = '', detail }) {
  return (
    <div className={`sensor sensor-${tone}`}>
      <Icon name={icon} size={22} />
      <div>
        <div className="sensor-label">{label}</div>
        <div className="sensor-value">{value}</div>
        {detail && <div className="sensor-detail">{detail}</div>}
      </div>
    </div>
  );
}

function Sensors({ s, state, now }) {
  const door = !s ? ['—', ''] : !s.reedEnabled ? ['Not monitored', 'muted'] : s.reedClosed ? ['Closed', 'ok'] : ['Open', s.armed ? 'alarm' : 'warn'];
  const motion = !s ? ['—', ''] : !s.pirEnabled ? ['Not monitored', 'muted'] : !s.pirReady ? ['Warming up', 'muted'] : s.pirMotion ? ['Motion', s.armed ? 'alarm' : 'warn'] : ['Clear', 'ok'];
  const q = signalQuality(s?.rssi);
  const hub = state?.hubStatus;
  return (
    <section className="card">
      <h2>Sensors</h2>
      <div className="sensors">
        <Sensor icon="door" label="Door" value={door[0]} tone={door[1]} />
        <Sensor icon="motion" label="Motion" value={motion[0]} tone={motion[1]} />
        <Sensor icon="signal" label="Node signal" value={q.label} detail={s?.rssi ? `${s.rssi} dBm` : null} />
        <Sensor icon="hand" label="Node button" value={s ? (s.button ? 'Pressed' : 'Released') : '—'} tone={s?.button ? 'warn' : ''} />
      </div>
      <dl className="meta">
        <div><dt>Node uptime</dt><dd>{formatUptime(s?.uptimeMs)}</dd></div>
        <div><dt>Hub Wi-Fi</dt><dd>{hub ? `${hub.wifiRssi} dBm · ch ${hub.channel}` : '—'}</dd></div>
        <div><dt>Hub uptime</dt><dd>{formatUptime(hub?.uptimeMs)}</dd></div>
        <div><dt>Last status</dt><dd title={absoluteTime(state?.nodeStatusAt)}>{relativeTime(state?.nodeStatusAt, now)}</dd></div>
      </dl>
    </section>
  );
}

function Activity({ events, setEvents, now }) {
  const [loading, setLoading] = useState(false);
  const [exhausted, setExhausted] = useState(false);

  async function loadMore() {
    const oldest = events.at(-1)?.id;
    if (!oldest) return;
    setLoading(true);
    try {
      const { events: older } = await api.events(oldest);
      if (older.length === 0) setExhausted(true);
      setEvents((ev) => [...ev, ...older.filter((o) => !ev.some((e) => e.id === o.id))]);
    } finally {
      setLoading(false);
    }
  }

  return (
    <section className="card activity">
      <h2>Activity</h2>
      {events.length === 0 ? (
        <p className="muted">No events yet. Arming, alarms and resets will show up here.</p>
      ) : (
        <ul className="events">
          {events.map((e) => {
            const info = EVENT_INFO[e.type] ?? { title: e.type, tone: 'muted', icon: 'clock' };
            const cause = e.type === 'ALARM' ? causeText(e.cause) : null;
            return (
              <li key={e.id} className={`event event-${info.tone}`}>
                <span className="event-icon"><Icon name={info.icon} size={16} /></span>
                <span className="event-title">
                  {info.title}
                  {cause && <span className="event-cause"> — {cause}</span>}
                </span>
                <time title={absoluteTime(e.received_at)}>{relativeTime(e.received_at, now)}</time>
              </li>
            );
          })}
        </ul>
      )}
      {events.length >= 50 && !exhausted && (
        <button className="btn btn-ghost btn-sm" onClick={loadMore} disabled={loading}>
          {loading ? 'Loading…' : 'Load older'}
        </button>
      )}
    </section>
  );
}

export default function Dashboard({ user, onSignedOut }) {
  const { state, events, setEvents, connected } = useLive(onSignedOut);
  const now = useNow(1000);
  const s = state?.nodeStatus ?? null;
  const problem = linkProblem(connected, state);

  async function signOut() {
    try { await api.logout(); } finally { onSignedOut(); }
  }

  return (
    <div className="app">
      <header className="topbar">
        <div className="brand">
          <Icon name="shieldCheck" size={24} />
          <span>SecureSense</span>
        </div>
        <div className="pills">
          <Pill label="Server" ok={connected} />
          <Pill label="Broker" ok={connected ? state?.brokerConnected ?? null : null} />
          <Pill label="Hub" ok={connected ? state?.hubOnline ?? null : null} />
          <Pill label="Node" ok={connected ? state?.nodeOnline ?? null : null} />
        </div>
        <div className="user">
          <span className="muted">{user.username}</span>
          <button className="btn btn-ghost btn-sm" onClick={signOut} title="Sign out">
            <Icon name="logout" size={16} /> <span className="hide-sm">Sign out</span>
          </button>
        </div>
      </header>

      <main className="grid">
        {problem && <div className={`notice notice-${problem.tone} span-all`}>{problem.text}</div>}
        <div className="span-all">
          <Hero s={s} stale={Boolean(problem)} now={now} nodeStatusAt={state?.nodeStatusAt} />
        </div>
        <Controls s={s} state={state} />
        <Sensors s={s} state={state} now={now} />
        <div className="span-all">
          <Activity events={events} setEvents={setEvents} now={now} />
        </div>
      </main>
      <footer className="footer muted">
        Lab prototype: the radio link between node and hub is not yet encrypted. Do not rely on it to protect property.
      </footer>
    </div>
  );
}

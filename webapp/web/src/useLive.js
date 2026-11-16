import { useEffect, useRef, useState } from 'react';

/** Live hub state + events over the backend WebSocket, with auto-reconnect. */
export function useLive(onUnauthorized) {
  const [state, setState] = useState(null);
  const [events, setEvents] = useState([]);
  const [connected, setConnected] = useState(false);
  const unauthorized = useRef(onUnauthorized);
  unauthorized.current = onUnauthorized;

  useEffect(() => {
    let ws;
    let timer;
    let attempt = 0;
    let stopped = false;

    const connect = () => {
      const proto = location.protocol === 'https:' ? 'wss' : 'ws';
      ws = new WebSocket(`${proto}://${location.host}/ws`);
      ws.onopen = () => { attempt = 0; setConnected(true); };
      ws.onmessage = (e) => {
        let m;
        try { m = JSON.parse(e.data); } catch { return; }
        if (m.type === 'snapshot') { setState(m.state); setEvents(m.events); }
        else if (m.type === 'state') setState(m.state);
        else if (m.type === 'event') setEvents((ev) => [m.event, ...ev.filter((x) => x.id !== m.event.id)]);
      };
      ws.onclose = (e) => {
        setConnected(false);
        if (stopped) return;
        if (e.code === 4401) { unauthorized.current?.(); return; }
        timer = setTimeout(connect, Math.min(10_000, 500 * 2 ** attempt++));
      };
    };
    connect();
    return () => { stopped = true; clearTimeout(timer); ws?.close(); };
  }, []);

  return { state, events, setEvents, connected };
}

/** Re-render every `ms` so relative times stay current. */
export function useNow(ms = 1000) {
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), ms);
    return () => clearInterval(t);
  }, [ms]);
  return now;
}

import { useEffect, useState } from 'react';
import { api } from './api.js';
import Login from './Login.jsx';
import Dashboard from './Dashboard.jsx';

export default function App() {
  const [user, setUser] = useState(undefined); // undefined = checking, null = signed out

  useEffect(() => {
    api.me().then((d) => setUser(d.user)).catch(() => setUser(null));
  }, []);

  if (user === undefined) return <div className="splash">Loading…</div>;
  if (!user) return <Login onLogin={setUser} />;
  return <Dashboard user={user} onSignedOut={() => setUser(null)} />;
}

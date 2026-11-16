# SecureSense web app

A dashboard for the SecureSense alarm: live status, Arm / Disarm / Silence / Reset buttons that wait for the node's real answer, and an event history.

```
C3 node ─ESP-NOW─ S3 hub ─MQTT/TLS─ broker ─ backend (this) ─WebSocket─ browser
```

- **Backend** (`server/`): Fastify on Node.js. It handles:
  - logins (scrypt password hashes, HttpOnly session cookie)
  - the MQTT bridge
  - SQLite history, using Node's built-in `node:sqlite`, so there's no native module to build
  - live WebSocket updates
  - an audit log of who sent which command
- **Frontend** (`web/`): React + Vite. Works on a phone.
- **Dev tools** (`tools/`): a local MQTT broker and a hub simulator that speaks exactly the Stage 8 firmware topics, so you can work without hardware or HiveMQ.

Requires **Node.js 22.13+** (tested on 24).

## Quick start (no hardware, no cloud)

Open four terminals in `webapp/`:

```bash
npm install
cp .env.example .env      # Windows: copy .env.example .env
```
Edit `.env` and set `BOOTSTRAP_USERNAME` and `BOOTSTRAP_PASSWORD` (at least 10 characters). That user is created on first start.

| Terminal | Command | What it is |
|---|---|---|
| 1 | `npm run broker` | Local MQTT broker on `127.0.0.1:1883` |
| 2 | `npm run sim` | Pretend S3 hub + C3 node. Keys: `d` door, `m` motion, `b` button, `n` node offline/online, `h` hub offline/online |
| 3 | `npm run dev:server` | Backend on :3000, restarts when you edit it |
| 4 | `npm run dev:web` | Frontend on **http://localhost:5173** |

(`npm run dev` starts terminals 3 and 4 together.)

Sign in, press **Arm**, press `d` in the simulator terminal and watch the alarm appear.

## Using the real hub with HiveMQ Cloud

1. In the HiveMQ console → **Access Management**, create a third login just for the backend, e.g. `securesense-web`. Don't reuse the hub's.
2. In `.env`:
   ```ini
   MQTT_URL=mqtts://<your-cluster-url>:8883
   MQTT_USERNAME=securesense-web
   MQTT_PASSWORD=...
   MQTT_TOPIC_PREFIX=securesense/hub1   # must match secrets.h on the S3
   ```
   Node already trusts HiveMQ's certificate, so no CA file is needed here.
3. Don't run `broker` or `sim`. Just `npm run dev:server` and `npm run dev:web`.

### Using the local broker with the real S3 instead (bench only, no TLS)
Run the broker on your network with a password:
```bash
BROKER_HOST=0.0.0.0 BROKER_USERNAME=hub BROKER_PASSWORD=<pw> npm run broker
```
In PowerShell, set them first: `$env:BROKER_HOST='0.0.0.0'` and so on.
In `secrets.h`, use `MQTT_URI "mqtt://<this-PC's-IP>:1883"` and set `MQTT_CA_CERT = nullptr`. Windows Firewall will ask to allow Node.

## Production-style run
```bash
npm run build     # builds the frontend into dist/
npm start         # serves the app + API on http://127.0.0.1:3000
```
To open it from your phone on the same Wi-Fi, set `HOST=0.0.0.0` and browse to `http://<PC-IP>:3000`. That's plain HTTP, so bench use only. Before putting it on the internet, put it behind HTTPS (for example a Caddy reverse proxy or Cloudflare Tunnel) and set `COOKIE_SECURE=true`.

## Users
```bash
npm run user -- add <username>      # prompts for a password
npm run user -- passwd <username>   # reset; signs that user out everywhere
npm run user -- list
```

## API (all JSON, all need a session except login)
| Method | Path | |
|---|---|---|
| POST | `/api/login` | `{username, password}`. Sets the session cookie. 5 failures per 15 min locks that username+IP |
| POST | `/api/logout` | |
| GET | `/api/me` | |
| GET | `/api/state` | Broker/hub/node online flags, latest node + hub status |
| GET | `/api/events?limit=50&before=<id>` | Event history, newest first |
| POST | `/api/commands` | `{cmd: "arm"\|"disarm"\|"silence"\|"rearm"\|"ping"\|"status"}` → `{id, result, state}` once the hub answers (8 s max) |
| GET | `/api/commands` | Last 50 commands with user and result (audit log) |
| WS | `/ws` | `snapshot`, then `state` / `event` / `command` messages |

`result` is the hub/C3 code (`ACCEPTED`, `REED_OPEN`, `NODE_OFFLINE`, `TIMEOUT_UNKNOWN`, ...). It can also be `NO_RESPONSE`, meaning the hub never answered, or `BROKER_OFFLINE`. Neither of those proves the command didn't run, so the UI says to check the status.

## Security notes
- The browser never talks to MQTT. Only the backend publishes commands, and only after checking the session.
- The session cookie is `HttpOnly` and `SameSite=Strict`. State-changing requests must be `application/json`, which blocks cross-site form posts.
- The database stores only a SHA-256 of each session token, plus scrypt password hashes.
- The ESP-NOW link between C3 and S3 is still unencrypted (see the firmware README).

## Not done yet
- Push notifications when an alarm fires (PWA + Web Push)
- HTTPS deployment
- Multiple hubs/nodes

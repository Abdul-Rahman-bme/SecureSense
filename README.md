# SecureSense — ESP-NOW Intrusion Alarm with Cloud MQTT & Web Dashboard

A university IoT security-system **prototype** with an independently operating **ESP32-C3 sensor node**, an **ESP32-S3 gateway**, **EMQX Cloud MQTT**, and a **React + Fastify dashboard**. The dashboard supports real-time sensor status, event history, and confirmed `arm`, `disarm`, `silence` and `rearm` commands.

> **Status (November 2026):** The team has demonstrated the complete C3 → S3 → EMQX → backend → browser path and control commands on physical hardware. This is still a **lab prototype**, not a certified or deployment-ready burglar-alarm system. ESP-NOW is unauthenticated/unencrypted, a C3 reboot returns DISARMED, and alarms arising during a cloud disconnect are not durably queued for later delivery.

## Contents

1. [System overview](#system-overview)
2. [Repository layout](#repository-layout)
3. [Hardware and pin mapping](#hardware-and-pin-mapping)
4. [Firmware installation](#firmware-installation)
5. [EMQX Cloud setup](#emqx-cloud-setup)
6. [Web dashboard setup — Windows](#web-dashboard-setup--windows)
7. [One-click local startup](#one-click-local-startup)
8. [Private phone access without a paid host](#private-phone-access-without-a-paid-host)
9. [End-to-end acceptance tests](#end-to-end-acceptance-tests)
10. [Troubleshooting](#troubleshooting)
11. [Security, limitations and roadmap](#security-limitations-and-roadmap)

## System overview

```text
Reed switch / HC-SR501 PIR
          │
          ▼
ESP32-C3 sensor node ── ESP-NOW ── ESP32-S3 gateway ── MQTT over TLS ── EMQX Cloud
       │                                                                      │
  Local alarm                                                            MQTT client
  LEDs/buzzer                                                               │
  GPIO9 button                                                  Node.js Fastify backend
  GPIO6/7 DIP switches                                                  │
                                                                  SQLite + WebSocket
                                                                          │
                                                                  React web dashboard
                                                                          │
                                                                   Owner's PC / phone
```

- The C3 detects sensor events and operates its buzzer **without depending on the S3, MQTT, or internet**.
- The S3 joins a **2.4 GHz** router and bridges ESP-NOW packets to EMQX via **TLS on port 8883**.
- Stage 8 C3 firmware scans **Wi-Fi channels 1–13** to discover the hub on the router's channel when the radio link is lost. The older Stage 7 C3 is fixed on channel 4.
- The browser **never stores broker credentials or publishes to MQTT directly**. It talks to the authenticated Fastify backend, which holds the MQTT connection and SQLite event history.
- Currently **one C3 node (ID 2) per S3 hub** is supported.

## Repository layout

```text
SecureSense/
├── 02_C3_SecureSense_Node/
│   ├── 02_C3_SecureSense_Node.ino
│   └── SecureSenseProtocol.h
├── 03_S3_SecureSense_Hub/
│   ├── 03_S3_SecureSense_Hub.ino
│   ├── SecureSenseProtocol.h
│   └── secrets.example.h
├── webapp/
│   ├── .env.example
│   ├── server/               # Fastify, authentication, MQTT, SQLite
│   ├── web/                  # React/Vite frontend
│   ├── tools/                # Local test broker and hub simulator
│   ├── package.json
│   └── package-lock.json
├── start-securesense-local.cmd
├── render.yaml              # OPTIONAL PAID Render hosting blueprint
├── .gitignore
└── README.md
```

Files you create locally, including `secrets.h`, `webapp/.env`, `webapp/data/`, and `webapp/node_modules/`, must **not** be committed.

## Hardware and pin mapping

The current **final PCB/breadboard mapping** for the ESP32-C3 node is:

| C3 GPIO | Function | Expected behaviour |
|---|---|---|
| GPIO0 | Two-wire normally-open Reed contact to GND | Direct breadboard configuration uses `INPUT_PULLUP`: `LOW` = electrically CLOSED; verify magnet alignment |
| GPIO3 | HC-SR501 PIR OUT | PIR powered appropriately; output must be safe for 3.3 V GPIO |
| GPIO2 | Green status LED | Active LOW; LED/resistor to 3.3 V |
| GPIO8 | Red status LED | Active LOW; LED/resistor to 3.3 V |
| GPIO9 | Enclosure button to GND | Hold **3 s during alarm** to silence buzzer only; **boot-strapping pin**, do not hold during reset |
| GPIO10 | Buzzer transistor drive | C3 does **not** drive a 5 V buzzer directly |
| GPIO6 | Reed-enable DIP to GND | DIP ON/closed = Reed enabled |
| GPIO7 | PIR-enable DIP to GND | DIP ON/closed = PIR enabled |

The code defaults to `REED_HAS_PCB_RC=false` for direct breadboard wiring and `DIP_SWITCHES_WIRED=true` for populated DIP switches. For the actual PCB's RC Reed input network, **review the polarity and set `REED_HAS_PCB_RC` appropriately** before flashing.

### Sensor selection

| GPIO6 Reed switch | GPIO7 PIR switch | Detection mode |
|---|---|---|
| ON | ON | Reed **or** PIR triggers |
| ON | OFF | Reed only |
| OFF | ON | PIR only |
| OFF | OFF | No sensors; arming is rejected |

Selection is latched when armed. Changing the DIP switches while armed must not silently disable monitoring; restore the prior setting or disarm before reconfiguration.

### Alarm states

- `DISARMED`: local sensors do not trigger an intrusion alarm.
- `ARMED`: enabled sensors are monitored by the C3 locally.
- `ALARM`: local red LED/buzzer activate and an event is transmitted when connected.
- `SILENCED`: audible output stops but the alarm remains latched and armed.
- `RESET_REARM`: owner-initiated command from a latched alarm; accepted only if enabled Reed is closed and stable **2 s**, enabled PIR is ready and clear **10 s**, the DIP mask is unchanged, and GPIO9 is not held. If accepted, remain armed and ready for another alarm.

An alarm clearing does **not** prove the property is secure. Do not automatically rearm merely because PIR goes LOW.

## Firmware installation

### Prerequisites

- Arduino IDE 2.x
- Espressif **Arduino-ESP32 3.x** board package (Stage 8 development reports compilation with 3.3.12)
- ESP32-C3 SuperMini and ESP32-S3 Dev Module with their working USB cables
- A **2.4 GHz** Wi-Fi network with internet access
- EMQX MQTT credentials created below

### A. ESP32-S3 hub

1. Open `03_S3_SecureSense_Hub/03_S3_SecureSense_Hub.ino`.
2. In that folder, copy `secrets.example.h` to `secrets.h` (do **not** edit/commit the example with actual passwords).
3. Fill these settings in `secrets.h`:

   ```cpp
   constexpr const char* WIFI_SSID = "YOUR_2_4_GHZ_WIFI";
   constexpr const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
   constexpr const char* MQTT_URI = "mqtts://YOUR_EMQX_BROKER:8883";
   constexpr const char* MQTT_USERNAME = "YOUR_S3_MQTT_USER";
   constexpr const char* MQTT_PASSWORD = "YOUR_S3_MQTT_PASSWORD";
   constexpr const char* MQTT_TOPIC_PREFIX = "securesense/hub1";
   constexpr const char* MQTT_CA_CERT = R"PEM(
   -----BEGIN CERTIFICATE-----
   PASTE_COMPLETE_BROKER_CA_PEM_HERE
   -----END CERTIFICATE-----
   )PEM";
   ```

4. In Arduino IDE select **ESP32S3 Dev Module**, **USB CDC On Boot: Disabled** (the tested UART0/COM-port configuration), and Serial Monitor **115200 baud / New Line**.
5. Verify and upload. **No PubSubClient dependency** is needed: the S3 sketch uses the MQTT client built into ESP-IDF under Arduino-ESP32 3.x.
6. Serial Monitor should eventually show Wi-Fi connected, MQTT connected, and C3 discovered. Type `net`, `who`, `ping`, `status` to inspect connections.

For an initial radio-only test, leave `WIFI_SSID=""` and `MQTT_URI=""` to run **offline fixed channel-4 mode**.

### B. ESP32-C3 sensor node

1. Open `02_C3_SecureSense_Node/02_C3_SecureSense_Node.ino`.
2. Confirm correct GPIO wiring and the `REED_HAS_PCB_RC` and `DIP_SWITCHES_WIRED` flags.
3. Select **ESP32C3 Dev Module**, **USB CDC On Boot: Enabled** (the tested USB arrangement), **115200 baud**.
4. Upload the Stage 8 sketch **with its matching `SecureSenseProtocol.h`**. Protocol version is **2** on both boards.
5. Observe channel sweeping, discovery of the S3 and stable readings. The previous tested C3 TX power was set to **8.5 dBm**; preserve that if radio behaviour is sensitive to antenna placement.
6. Keep only **one C3 with Node ID 2** powered for the current hub. A different C3 physical board has a different MAC and the S3 retains its first paired MAC until restarted.

### Initial radio checks

On the S3 Serial Monitor, enter (one line at a time):

```text
net
who
ping
status
```

Confirm the node is discovered, status advances, commands return real C3 results and Wi-Fi and MQTT stay online. When the router is on a channel other than 4, the **Stage 8 channel-scanning C3 sketch is required**; the Stage 7 fixed-channel sketch cannot follow automatically.

## EMQX Cloud setup

The current prototype uses **EMQX Cloud Serverless**, with `mqtts://...:8883`. Use your own deployment broker address; never put authentication secrets in public GitHub files.

1. Create an EMQX Cloud deployment and record the **MQTT TLS hostname / port 8883**. Download its CA certificate.
2. Under **Access Control → Authentication**, create three *separate* username/password pairs:
   - `securesense-s3`: hub publishes telemetry and subscribes to commands.
   - `securesense-web`: authenticated backend subscribes to telemetry and publishes commands.
   - `securesense-monitor`: optional MQTTX read-only observer.
3. Under **Access Control → Authorization**, configure minimum necessary **topic ACLs** for your deployment, for prefix `securesense/hub1`:

   | User | Publish | Subscribe |
   |---|---|---|
   | `securesense-s3` | `hub/#`, `node/2/{online,status,event,cmd_result}` under prefix | `node/2/cmd` |
   | `securesense-web` | `node/2/cmd` | `hub/#`, `node/2/{online,status,event,cmd_result}` |
   | `securesense-monitor` | none | `securesense/hub1/#` |

   **Important:** The curly-brace grouping shown above is conceptual notation, **not an MQTT wildcard**. Add individual exact-topic rules (or supported MQTT `+`/`#` patterns) in the EMQX console. Deny other permissions according to the console's authorization-rule precedence.
4. Put the S3 credentials, broker URI and CA PEM into **S3 `secrets.h`**. Put the backend's **different** credentials into **`webapp/.env`**.
5. Test with MQTTX Desktop: connect to the TLS broker on **8883**, use `securesense-monitor`, and subscribe to `securesense/hub1/#`. Confirm fresh `hub/status` and `node/2/status` messages.

### MQTT topic reference

| Topic suffix (under `securesense/hub1`) | Purpose |
|---|---|
| `hub/online` | Retained hub availability (broker Last Will) |
| `hub/status` | Hub uptime, RSSI, channel, C3 link age |
| `node/2/online` | Retained C3 online/offline indication |
| `node/2/status` | Retained Reed/PIR/armed/alarm/actuator JSON |
| `node/2/event` | Non-retained alarm/arm/disarm/rearm/silence events |
| `node/2/cmd` | Backend → S3 (`{"id":"unique-request","cmd":"arm"}`), **never retain** |
| `node/2/cmd_result` | S3 → backend correlated result (`ACCEPTED`, reason, or timeout) |

A queued/received command is **not** proof that it executed. The web UI must wait for the correlated C3 result. A timeout means the outcome is unknown, not necessarily failure.

## Web dashboard setup — Windows

### 1. Clone and install

Do **not** use GitHub's **Download ZIP** if you intend to run `git commit` or `git push`: ZIPs contain no `.git` history. Instead:

```powershell
git clone https://github.com/Abdul-Rahman-bme/SecureSense.git
cd .\SecureSense\webapp
node --version
npm ci --include=dev
Copy-Item .env.example .env
notepad .env
```

Node.js **22.13+** is required; Node 24 was used in the development workflow.

### 2. Configure `webapp/.env`

```ini
HOST=127.0.0.1
PORT=3000
MQTT_URL=mqtts://YOUR_EMQX_BROKER:8883
MQTT_USERNAME=securesense-web
MQTT_PASSWORD=YOUR_WEB_MQTT_PASSWORD
MQTT_TOPIC_PREFIX=securesense/hub1
NODE_ID=2
DB_PATH=data/securesense.db
SESSION_DAYS=7
COOKIE_SECURE=false
```

This `.env` file is private and git-ignored. Never paste passwords in issues, screenshots or chat. **Use different passwords for the S3, backend and web-login user** and rotate credentials if previously exposed.

### 3. Create the web login

From `webapp/`:

```powershell
npm run user -- add admin
```

The CLI prompts for a password of at least 10 characters. Login accounts are stored in SQLite locally. To change later:

```powershell
npm run user -- passwd admin
```

Alternative first-start bootstrap variables (`BOOTSTRAP_USERNAME`, `BOOTSTRAP_PASSWORD`) should be removed after the first user has been created. Existing users are **not** reset just by changing these variables.

### 4. Run in development

Use two terminals in `webapp/`:

```powershell
# Terminal 1
npm run dev:server
```

```powershell
# Terminal 2
npm run dev:web
```

Open **http://localhost:5173** and log in. The frontend proxies `/api` and `/ws` to Fastify on port 3000 during development.

### 5. Build the single-port application (recommended)

From `webapp/`:

```powershell
npm run build
npm start
```

Open **http://localhost:3000**. Fastify serves the built React frontend, API and WebSocket together; no Vite terminal is needed.

## One-click local startup

From the repository root, double-click `start-securesense-local.cmd` (Windows). It checks for `webapp/.env`, installs dependencies if necessary, rebuilds React, starts Fastify and opens `http://localhost:3000`.

Keep the launcher terminal running. The S3 and C3 continue their own local security functions when your laptop is off, but **the website/backend and SQLite history are unavailable until its host is on again**.

## Private phone access without a paid host

**Recommended for a lab demo:** [Tailscale Personal](https://tailscale.com/pricing) + [Tailscale Serve](https://tailscale.com/docs/features/tailscale-serve). This keeps the authenticated dashboard private to approved devices instead of exposing it to the whole internet. No Render service is needed.

1. Start the single-port site on the host PC with `start-securesense-local.cmd` and confirm **http://localhost:3000** works.
2. Install Tailscale on that PC, sign in, and install Tailscale on the owner's phone using the same tailnet or appropriate tailnet sharing. Verify both are online.
3. On the PC, in an appropriate Windows terminal, run:

   ```powershell
   tailscale serve --bg 3000
   tailscale serve status
   ```

4. Follow any Tailscale prompts to enable MagicDNS / HTTPS certificates. Tailscale displays a **private HTTPS URL** resembling `https://your-pc.your-tailnet.ts.net`.
5. From the phone, connect Tailscale, open that HTTPS address and sign in. Optionally add the page to the phone's home screen.
6. For **HTTPS-only browser access**, set `COOKIE_SECURE=true` in `webapp/.env`, restart Fastify, and verify login over the HTTPS URL. For local HTTP-only testing, use `COOKIE_SECURE=false` instead.
7. To disable Tailscale Serve: `tailscale serve off`.

Tailscale's **Personal plan is free for non-commercial use**, subject to its published limits; the host PC must remain running with Node.js, its MQTT connection and Tailscale active. `Serve` is private to the tailnet; **do not use public Tailscale Funnel** for this security prototype.

For an always-on setup, move the Fastify app and persistent SQLite database to an appropriately secured always-on computer/Raspberry Pi with backups, then use the same private access approach. This is **not production alarm monitoring**: the host, internet and cloud still have failure modes.

### Why not GitHub Pages or Render?

- GitHub Pages hosts static frontends; it **cannot run** the required Fastify/MQTT/SQLite backend. Separating these services would require additional hosting and careful authentication/cross-origin design.
- `render.yaml` in this repository is an **optional paid Render Starter** blueprint with a persistent disk. You **do not need to deploy it or provide card details** to run SecureSense locally or through Tailscale.
- A sleeping free cloud backend or an ephemeral SQLite filesystem is **not appropriate** for dependable security event history or remote controls.

## End-to-end acceptance tests

1. Confirm S3 `net` shows Wi-Fi, MQTT and the correct radio channel; `who` identifies the active C3.
2. Confirm EMQX and MQTTX show the S3 online and **fresh** C3 `node/2/status` telemetry.
3. Confirm dashboard indicates server, broker, hub and node online, and Reed/PIR values match hardware.
4. Close Reed, wait for PIR warm-up if selected; press **Arm** on the dashboard. Confirm `ACCEPTED` and actual C3 `armed=1`.
5. Open the Reed (when selected). C3 sounds its own buzzer, S3 sends an `ALARM` event, and dashboard shows the alarm.
6. While Reed remains open, **Reset & re-arm** must be rejected (`REED_OPEN`); alarm remains latched.
7. Hold enclosure GPIO9 for 3 seconds. Buzzer stops, but `armed=1` and `alarm=1` remain.
8. Close Reed for >=2 s and ensure enabled PIR is ready/clear for >=10 s; press **Reset & re-arm**. Confirm `ACCEPTED`, `armed=1`, `alarm=0`, `silenced=0`.
9. Open Reed again: a **new** alarm must occur, demonstrating re-alert capability.
10. Unplug S3 while C3 is armed; triggering a sensor must still sound the **local C3 alarm**. Restore S3 and check reconnection. Do not claim any missed MQTT event will automatically be replayed.
11. Toggle DIP switches while disarmed and verify Reed-only, PIR-only, both, and neither modes.
12. Check dashboard event history and reboot/disconnect status behaviour. A **C3 power-cycle resets to DISARMED** in the current code and must be visibly treated as a security limitation.

## Troubleshooting

| Symptom | Likely cause / action |
|---|---|
| `fatal: not a git repository` | You are in a GitHub ZIP extraction, not a `git clone` directory. Clone the repo, then commit there. |
| `ECONNREFUSED 127.0.0.1:1883` | Backend is using the local MQTT default. Confirm `webapp/.env` exists, has `MQTT_URL=mqtts://...:8883`, and restart. |
| `vite is not recognized` | Run `npm ci --include=dev` in `webapp/` (Node >=22.13). |
| MQTT authentication fails | Verify the **backend's** separate EMQX username/password and authorization rules. |
| TLS fails right after S3 start | Check CA PEM, broker hostname, internet and NTP time; retry may work after time sync. Do not use `setInsecure()`. |
| S3 MQTT online but C3 offline | Check Wi-Fi 2.4 GHz channel, use Stage 8 C3 channel scanning, antenna positioning, same protocol v2. |
| Arm rejected | Enabled Reed must indicate CLOSED; enabled PIR must have completed warm-up. Check DIP configuration. |
| Reset & re-arm rejected | Observe Reed/PIR stable-time interlocks and unchanged DIP mask. |
| MQTTX connected but no telemetry | Subscribe to `securesense/hub1/#`; verify S3 publishes, correct prefix/ACL and recent timestamps. |
| Browser reports backend offline | Check `npm start` or `dev:server`, `.env`, MQTT broker connection, and browser WebSocket status. |
| `TIMEOUT_UNKNOWN` | Query fresh status; never assume the command failed or succeeded. |

## Security, limitations and roadmap

- **Not production-ready:** ESP-NOW is not encrypted/authenticated. A nearby attacker may potentially interfere with or spoof the local command link; do not deploy this to protect property until authenticated transport and key management are implemented.
- **Reboot safety:** C3 currently boots DISARMED, even if armed before losing power. Define and implement a deliberate, persisted power-loss/recovery policy.
- **Missed events:** MQTT events are not durably queued on the hub; SQLite event history is incomplete if the internet/backend is down. Add persistent storage, replay identifiers, acknowledgments and reliable delivery.
- **Mobile push notifications are not implemented yet**; viewing the dashboard is not the same as receiving phone alerts.
- **One node:** S3 pairs with one C3 MAC/Node ID 2 at a time. Multi-node registration and unique IDs remain future work.
- **Authorization:** Restrict EMQX access with topic ACLs and separate credentials; do not let browsers connect directly to `/cmd` or store MQTT passwords in frontend code.
- **Password hygiene:** Any password ever posted in a chat, screenshot or public repository should be rotated. Never commit `secrets.h`, `.env`, database files, private keys, or backup secrets.
- **Battery/power:** Battery system, charging, enclosure tamper protection and field reliability are separate engineering work; bench USB power does not validate an unattended product.

## GitHub workflow

From a *real cloned repository* (one containing `.git/`):

```powershell
git status --short
git add README.md
git commit -m "Document SecureSense Stage 8 setup and card-free access"
git pull --rebase origin main
git push origin main
```

Before `git add`, always inspect `git status` and never use `git add .` indiscriminately when local credentials exist. The sample `secrets.example.h` and `.env.example` are safe to commit **only with placeholders**.

---

Maintained for the SecureSense EN3251 IoT course project. Source and setup guide: <https://github.com/Abdul-Rahman-bme/SecureSense>.

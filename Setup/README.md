# SecureSense — Complete Setup, Flashing, Cloud MQTT & Dashboard Manual

**Reproduce the working ESP32-C3 → ESP32-S3 → EMQX Cloud → Fastify → React SecureSense system on a new Windows laptop.**

**Source repository:** https://github.com/Abdul-Rahman-bme/SecureSense  
**Documentation checked against:** repository `main`, November 19, 2026, including Stage 8 C3/S3 Arduino sketches and web application.  
**Previously demonstrated on real hardware:** C3 ↔ S3 ESP-NOW, S3 Wi-Fi and MQTT, EMQX/MQTTX telemetry, browser arm/disarm/silence/rearm, Reed/PIR reporting, local alarm operation during S3 disconnection.  
**Scope:** single C3 node (`nodeId=2`) and one S3 hub. **Lab prototype, not a certified security product.**

> **SECURITY FIRST:** Do not copy passwords from screenshots, chat history, GitHub or this manual. Previously shared MQTT and dashboard passwords must be rotated. Use different long, randomly generated passwords for the S3 MQTT account, web-backend MQTT account, MQTTX monitoring account, and dashboard login. Store them in a password manager. Never commit `secrets.h`, `.env`, credential backups or database files.

## Contents

1. [What runs where](#1-what-runs-where)
2. [Exact settings used by the working system](#2-exact-settings-used-by-the-working-system)
3. [Hardware and GPIO wiring](#3-hardware-and-gpio-wiring)
4. [Install tools on a fresh Windows laptop](#4-install-tools-on-a-fresh-windows-laptop)
5. [Clone GitHub correctly and open in VS Code](#5-clone-github-correctly-and-open-in-vs-code)
6. [EMQX Cloud: find or create your deployment](#6-emqx-cloud-find-or-create-your-deployment)
7. [EMQX Authentication: all three accounts](#7-emqx-authentication-all-three-accounts)
8. [EMQX Authorization: publish/subscribe ACLs](#8-emqx-authorization-publishsubscribe-acls)
9. [Download and use the MQTT CA certificate](#9-download-and-use-the-mqtt-ca-certificate)
10. [Configure the S3 `secrets.h`](#10-configure-the-s3-secretsh)
11. [Arduino IDE board package and serial settings](#11-arduino-ide-board-package-and-serial-settings)
12. [Flash and test the S3](#12-flash-and-test-the-s3)
13. [Configure, flash and test the C3](#13-configure-flash-and-test-the-c3)
14. [Use EMQX Monitor and MQTTX](#14-use-emqx-monitor-and-mqttx)
15. [Set up the Fastify backend and React frontend](#15-set-up-the-fastify-backend-and-react-frontend)
16. [Create dashboard users and run locally](#16-create-dashboard-users-and-run-locally)
17. [Complete hardware-to-browser test procedure](#17-complete-hardware-to-browser-test-procedure)
18. [Normal daily startup (without reconfiguration)](#18-normal-daily-startup-without-reconfiguration)
19. [Moving an existing installation to a new laptop](#19-moving-an-existing-installation-to-a-new-laptop)
20. [Common errors and fixes](#20-common-errors-and-fixes)
21. [GitHub updates and secrets hygiene](#21-github-updates-and-secrets-hygiene)
22. [Deployment options, backups and security limitations](#22-deployment-options-backups-and-security-limitations)
23. [Final reproduction checklist](#23-final-reproduction-checklist)
24. [Official reference links](#24-official-reference-links)

---

## 1. What runs where

```text
                 SENSOR NODE (independent local alarm)
   Reed GPIO0 · PIR GPIO3 · DIP GPIO6/7 · GPIO9 button
                LEDs GPIO2/8 · Buzzer GPIO10
                              |
                         ESP32-C3
                              |
                   ESP-NOW (protocol v2)
                              |
                         ESP32-S3
                       2.4 GHz Wi-Fi
                              |
                MQTT over TLS (port 8883)
                              |
                         EMQX Cloud
                    /                   \
          MQTTX Desktop            Node.js/Fastify
         (read-only monitor)       (MQTT client + SQLite)
                                         |
                                    API/WebSocket
                                         |
                                  React/Vite browser
                             (Arm / Disarm / Silence / Rearm)
```

- **C3:** senses intrusion and sounds its alarm locally, even when S3/internet is off. It **does not** use your Wi-Fi SSID or connect to MQTT.
- **S3:** joins your *2.4 GHz* Wi-Fi, uses the router's channel for ESP-NOW, discovers the C3 and bridges events/commands to EMQX. Its MQTT connection uses a dedicated hub account.
- **EMQX Cloud:** broker; **not** the dashboard host or firmware server. It routes topic messages among clients.
- **Fastify backend on your Windows laptop:** separately authenticates to EMQX, stores event/user data in local SQLite, serves the API and live WebSocket.
- **React frontend:** runs in the browser. The browser does not get the MQTT credentials.
- **MQTTX:** optional diagnostic client that *subscribes* to broker topics. It is not necessary for normal operation.

> **Important:** The web application requires a running Node.js backend. GitHub Pages can host static React assets but **cannot run this current Fastify/SQLite/MQTT backend** by itself. Without a separate hosted backend, keep your computer (or another server) running while you want the website available.

## 2. Exact settings used by the working system

These are **public configuration identifiers**, not passwords. Reuse the same deployment only if you still own it and it is active.

| Setting | Previously working value | Meaning |
|---|---|---|
| GitHub repo | `Abdul-Rahman-bme/SecureSense` | Project source code |
| EMQX Cloud deployment | `SecureSense_MateriaPulse` (display name) | Broker deployment |
| EMQX broker host | `oea629d2.ala.asia-southeast1.emqxsl.com` | Hostname only (do not include protocol here) |
| MQTT/TLS port | `8883` | ESP32-S3, Node backend, MQTTX Desktop |
| Secure WebSocket port | `8084` | Optional WSS, **not needed** for existing webapp |
| MQTT URI in firmware / backend | `mqtts://oea629d2.ala.asia-southeast1.emqxsl.com:8883` | Full URI |
| Topic prefix | `securesense/hub1` | Must match in S3 and backend |
| C3 node ID | `2` | Must match protocol and backend `NODE_ID` |
| ESP-NOW protocol | `v2` | Headers in both Arduino folders must match |
| ESP-NOW startup/offline channel | `4` | **Not a required router channel** with Stage 8 C3 scanning |
| Previously observed live router channel | `11` | Example from hardware test; your router may use a different channel |
| S3 MQTT *username* | `securesense-s3` | EMQX Authentication account |
| Web-backend MQTT *username* | `securesense-web` | Different EMQX account |
| MQTTX monitor *username* | `securesense-monitor` | Read-only EMQX account |
| MQTTX *client ID* | `securesense-mqttx-monitor` | Unique MQTT connection identifier |
| C3 serial | 115200, USB CDC On Boot **Enabled** | Proven native USB configuration |
| S3 serial | 115200, USB CDC On Boot **Disabled** | Proven UART0 configuration |
| Local dev site | `http://localhost:5173` | Vite frontend, Fastify at port 3000 |
| Single-port site | `http://localhost:3000` | Built React + Fastify from one process |

### MQTT usernames are **not** Client IDs

The EMQX *Authentication* table stores usernames/passwords. The *Monitor → Clients* table shows **both** a client's username and client ID.

- **S3 username:** `securesense-s3`; **S3 client ID:** automatically generated from its Wi-Fi MAC as `securesense-hub-<last 3 MAC bytes in lowercase hex>`; for the previously tested hub this was `securesense-hub-4b9938`.
- **Backend username:** `securesense-web`; **backend client ID:** automatically generated at server launch as `securesense-web-<8 random hex digits>`; changes when the backend restarts.
- **MQTTX username:** `securesense-monitor`; **MQTTX client ID:** set manually to `securesense-mqttx-monitor`.

Do **not** give two simultaneously connected MQTT clients the same client ID; otherwise the broker may disconnect one.

### Credential inventory — keep this PRIVATE, not in Git

Record the following in a password manager, not in this README:

| Secret | Where entered | Required? |
|---|---|---|
| 2.4 GHz Wi-Fi password | `03_S3_SecureSense_Hub/secrets.h` | Yes, for S3 |
| S3 EMQX password | `secrets.h` | Yes |
| Web EMQX password | `webapp/.env` | Yes |
| MQTTX EMQX password | MQTTX connection profile | Optional for monitoring |
| Dashboard login password | Local SQLite via `npm run user -- ...` | Yes |
| PEM CA certificate | `secrets.h` and optionally MQTTX CA file | Public trust certificate, **not** a secret; must be authentic |

---

## 3. Hardware and GPIO wiring

### C3 wiring used in the project

| C3 pin | Device | Connection / operation |
|---|---|---|
| GPIO0 | Magnetic Reed contact | For **direct breadboard** testing, Reed between GPIO0 and GND; pull-up input. Electrical closed = LOW (`raw=0`), open = HIGH (`raw=1`) after magnet orientation verified |
| GPIO3 | HC-SR501 PIR OUT | PIR VCC → suitable **5 V**, PIR GND → common GND, OUT → GPIO3 (verify OUT never exceeds 3.3 V) |
| GPIO2 | Green LED | Active LOW: 3.3 V → ~1 kΩ → LED → GPIO2 |
| GPIO8 | Red LED | Active LOW: 3.3 V → ~1 kΩ → LED → GPIO8 |
| GPIO9 | Local false-alarm button | Momentary button to GND; hold **3 seconds during a latched alarm** to silence *buzzer only*; **boot-strapping pin**: do not hold during reset/power-on |
| GPIO10 | Buzzer | GPIO drives a transistor/buzzer circuit; **do not connect a 5 V buzzer directly to the GPIO** |
| GPIO6 | Reed DIP | DIP contact between GPIO6 and GND; switch closed/LOW = enabled |
| GPIO7 | PIR DIP | DIP contact between GPIO7 and GND; switch closed/LOW = enabled |

**DIP-selection table:**

| GPIO6 Reed | GPIO7 PIR | Monitoring mode |
|---|---|---|
| ON | ON | Reed **OR** PIR |
| ON | OFF | Reed only |
| OFF | ON | PIR only |
| OFF | OFF | No sensors selected; ARM rejected |

DIP selections are locked when the node is armed. Disarm to change the monitored sensor combination.

**Critical breadboard-vs-PCB difference:** the current C3 `.ino` contains `constexpr bool REED_HAS_PCB_RC=false;`. Keep `false` when the Reed is wired *directly* between GPIO0 and GND. On your populated final PCB, the Reed path includes the R4/R5/C7 conditioning network. Set the flag to the appropriate PCB mode **only after reviewing the exact populated circuit and testing Reed polarity/threshold**, then retest both open/closed states. Do not blindly upload the breadboard option to a different hardware revision.

**Power:** during the verified tests, the boards were independently USB powered. The custom PCB uses a switched regulated 5 V supply. Do **not** connect a bare 18650 Li-ion cell directly to a 5 V input or assume the USB port charges batteries. Battery holder/conversion/charging are separate, unverified design work.

**Physical assembly notes:** keep the C3 antenna away from large metal/breadboard rails/battery, orient the Reed and magnet experimentally, avoid GPIO0 resistor/LED leg shorts (one was found during testing), trim long legs safely, and verify no GPIO receives 5 V.

### 3.1 PCB and enclosure visual references already in the GitHub repo

The current repository includes *design renders and concepts*, not proof that the corresponding production enclosures or battery system were fabricated and validated:

- **ESP32-S3 main hub enclosure:** [isometric](docs/images/securesense_main_hub_enclosure_isometric.png), [top](docs/images/securesense_main_hub_enclosure_top.png), [exploded](docs/images/securesense_main_hub_enclosure_exploded.png), and [USB-side](docs/images/securesense_main_hub_enclosure_usb_side.png) views.
- **ESP32-C3 sensor node:** [sensor-node enclosure render](docs/images/securesense_sensor_node_enclosure.png) and [sensor-node concept](docs/images/securesense_sensor_node_concept.png).

For a replacement PCB/enclosure, use its actual STEP files, mounting-hole coordinates, component clearances, antenna placement, PIR lens opening and Reed/magnet alignment. These mechanical renders are not needed for the Arduino/MQTT installation procedure.

---

## 4. Install tools on a fresh Windows laptop

Install **before** connecting/uploading:

1. **Git for Windows:** https://git-scm.com/install/win — for `git clone`, `git pull`, `git push`.
2. **VS Code:** https://code.visualstudio.com/ — editor for React/Fastify and configuration files.
3. **Node.js:** https://nodejs.org/ — use Node.js **24 LTS** or another release >= **22.13** compatible with the project's `webapp/package.json`. npm is included. The tested website used Node 24.
4. **Arduino IDE 2.x:** https://www.arduino.cc/en/software — the project was operated in Arduino IDE 2.3.4.
5. **MQTTX Desktop (optional but strongly recommended for first setup):** https://mqttx.app/.
6. **Browser** and two USB data-capable cables, one for the C3 and one for the S3; any required USB-serial driver for your specific board.

Close and reopen PowerShell/VS Code after installing tools so PATH changes are loaded. Verify:

```powershell
git --version
node --version
npm --version
```

**Windows advice:** use a short path outside OneDrive, e.g. `C:\Projects\SecureSense`. Long OneDrive-synchronized paths caused confusion during earlier testing. A ZIP download has **no `.git` folder** and cannot be used with `git push` until properly cloned or initialized; use **`git clone`** below.

---

## 5. Clone GitHub correctly and open in VS Code

Open **PowerShell** (not Arduino Serial Monitor):

```powershell
New-Item -ItemType Directory -Force -Path 'C:\Projects' | Out-Null
Set-Location 'C:\Projects'
git clone https://github.com/Abdul-Rahman-bme/SecureSense.git
Set-Location '.\SecureSense'
git remote -v
git status --short
git log -1 --oneline
code .
```

If `code .` is not recognized, open VS Code → **File → Open Folder** → `C:\Projects\SecureSense`.

Expected repository structure:

```text
C:\Projects\SecureSense\
  .git\                           (hidden; present ONLY in a real clone)
  .gitignore
  README.md
  02_C3_SecureSense_Node\
    02_C3_SecureSense_Node.ino
    SecureSenseProtocol.h
  03_S3_SecureSense_Hub\
    03_S3_SecureSense_Hub.ino
    SecureSenseProtocol.h
    secrets.example.h
  webapp\
    .env.example
    package.json
    package-lock.json
    server\
    web\
  start-securesense-local.cmd
  render.yaml                       (free cloud hosting: Render + Neon; optional)
```

**Note:** GitHub does not include your working `secrets.h`, `webapp/.env`, `node_modules`, or local SQLite user database. Their absence from a fresh clone is intentional. Create them in the following steps.

---

## 6. EMQX Cloud: find or create your deployment

### Option A — Reuse the **existing SecureSense EMQX deployment**

1. Visit https://cloud.emqx.com/ (or sign in through the EMQX Cloud console link for your region).
2. Sign in to the EMQX **Cloud account** that owns your project.
3. Open **Projects → SecureSense → SecureSense_MateriaPulse → Deployment Overview**.
4. Confirm the deployment is **Running** and its current *MQTT over TLS address* matches the previous hostname, `oea629d2.ala.asia-southeast1.emqxsl.com`.
5. Check the deployment's status, any free-usage quota, and its **spending limits**. The previously used host may change if the deployment was deleted or recreated.
6. Reuse existing Authentication users if still valid, or rotate their passwords. You do **not** need to create a second deployment simply because you have a new laptop.

### Option B — New deployment (another account/team)

1. EMQX Cloud → **Projects → Create Project** if needed.
2. **Create Deployment → Serverless** if still offered on your account. Review current quota, region, usage limit and billing settings; do not assume perpetual free availability.
3. Choose the region nearest the deployment's clients if available, complete deployment creation, and open **Deployment Overview**.
4. Copy the **MQTT TLS/SSL hostname** and **port 8883**; if available, **Secure WebSocket 8084** is optional and *not* used by the current Node backend.
5. Download the CA certificate offered for that deployment.
6. Follow Sections 7–9 to create users and permissions.
7. Replace the sample hostname consistently in *S3 secrets*, *web `.env`* and *MQTTX*.

> **Never** use an unsecured public test broker for remote arm/disarm. The source code expects a proper TLS broker with user access controls.

---

## 7. EMQX Authentication: all three accounts

**EMQX Cloud** menu path: **Projects → your project → your deployment → Access Control → Authentication**. The default authentication table has a **+ Add** button.

Create one entry at a time:

1. Click **+ Add**.
2. In **Username**, type `securesense-s3`.
3. In **Password**, use a new strong unique password from your password manager.
4. Click **Confirm**. Verify the username appears in the Authentication table.
5. Repeat for `securesense-web` with a **different** password.
6. Repeat for `securesense-monitor` with a **third** password.

| EMQX username | Client | Stored where |
|---|---|---|
| `securesense-s3` | ESP32-S3 firmware | `03_S3_SecureSense_Hub/secrets.h` |
| `securesense-web` | Node/Fastify backend | `webapp/.env` |
| `securesense-monitor` | MQTTX Desktop | Saved in MQTTX's local connection profile |

**These are MQTT accounts, not user logins for the SecureSense website.** The dashboard has its own password stored in SQLite.

A newly connected MQTT client must authenticate with one of those accounts; assigning **Client ID** does *not* create an account. The EMQX Cloud console account (used to manage your deployment) is separate again.

If `Authentication` already lists these usernames, do not accidentally create duplicates. Check that you know their current passwords or reset them. After a reset, update only the corresponding client configuration, then restart/reconnect that client.

---

## 8. EMQX Authorization: publish/subscribe ACLs

Go to **Access Control → Authorization** for the same deployment. EMQX's labels may show **Username**, **All Users**, **Action**, **Permission** and **Topic**.

**Authentication** answers *who is connecting?*; **Authorization** answers *what may that client publish or subscribe?*

Under the **Username** rules, add these **Allow** rules. Enter the **full topic** shown, not a leading slash. Depending on the console version, create one rule per row or use its bulk-rule feature. Use `Publish` or `Subscribe` exactly as stated.

### S3 hub rules — Username `securesense-s3`

| Action | Permission | Topic |
|---|---|---|
| Publish | Allow | `securesense/hub1/hub/#` |
| Publish | Allow | `securesense/hub1/node/2/online` |
| Publish | Allow | `securesense/hub1/node/2/status` |
| Publish | Allow | `securesense/hub1/node/2/event` |
| Publish | Allow | `securesense/hub1/node/2/cmd_result` |
| Subscribe | Allow | `securesense/hub1/node/2/cmd` |

### Web backend rules — Username `securesense-web`

| Action | Permission | Topic |
|---|---|---|
| Subscribe | Allow | `securesense/hub1/hub/#` |
| Subscribe | Allow | `securesense/hub1/node/2/online` |
| Subscribe | Allow | `securesense/hub1/node/2/status` |
| Subscribe | Allow | `securesense/hub1/node/2/event` |
| Subscribe | Allow | `securesense/hub1/node/2/cmd_result` |
| Publish | Allow | `securesense/hub1/node/2/cmd` |

### MQTTX read-only monitoring — Username `securesense-monitor`

| Action | Permission | Topic |
|---|---|---|
| Subscribe | Allow | `securesense/hub1/#` |

The monitor should **not** publish security commands. Only the authenticated backend should publish commands to `/node/2/cmd`.

### Default denial

After allowing those three specific users, add an **All Users** fallback rule:

| Scope | Action | Permission | Topic |
|---|---|---|---|
| All Users | Publish & Subscribe | **Deny** | `#` |

This establishes deny-by-default access for anything not explicitly permitted. EMQX evaluates username-specific authorization before All Users rules; check the current rules and verify clients still connect and publish/subscribe as intended. Never add a blanket Deny that shadows all of your named Allow rules. Existing subscriptions/permissions may need reconnection to reflect changes.

### EMQX monitor verification

Navigate to **Monitor → Clients**. Once everything is running, expect **three connected MQTT clients**, with different client IDs:

```text
Username            Example client ID
--------------------------------------------------------
securesense-s3       securesense-hub-4b9938
securesense-web      securesense-web-<random8hex>
securesense-monitor  securesense-mqttx-monitor
```

The S3 client ID is derived from its MAC and therefore changes when you replace the S3 physical board. The web client ID deliberately changes between backend runs. Neither should be manually set in EMQX Authentication.

---

## 9. Download and use the MQTT CA certificate

1. In **EMQX Deployment Overview**, locate **CA Certificate** / **Download CA Certificate** and download the `.crt` or `.pem` offered for **your current deployment**. In the previous working setup, the download was named `emqxsl-ca.crt`.
2. Open the file in VS Code or Notepad as text.
3. Check the certificate content is a PEM block beginning with `-----BEGIN CERTIFICATE-----` and ending with `-----END CERTIFICATE-----`.
4. This file is the **CA trust certificate** for validating the broker, *not* a private key. Do not enable insecure TLS merely to connect.
5. You will paste the **complete PEM certificate** into `MQTT_CA_CERT` in the S3's `secrets.h` (Section 10). Keep the `.crt` file available for MQTTX Desktop if it requests a CA file.
6. The Node.js backend normally verifies EMQX's publicly trusted certificate using the Node trust store, so the `.env` does **not** have an S3-style `MQTT_CA_CERT` variable.

> A CA certificate from an unrelated or old EMQX deployment may not be the right trust anchor. Confirm it against the current deployment's download. A certificate with `BEGIN PRIVATE KEY` or a client key is **not** the CA certificate required here.

---

## 10. Configure the S3 `secrets.h`

In PowerShell from the **repository root**:

```powershell
Copy-Item '.\03_S3_SecureSense_Hub\secrets.example.h' '.\03_S3_SecureSense_Hub\secrets.h'
notepad '.\03_S3_SecureSense_Hub\secrets.h'
```

Set the following. **Never paste actual passwords into GitHub or a public issue.** Replace all `REPLACE_...` values locally.

```cpp
#pragma once

// 2.4 GHz Wi-Fi network joined by S3
constexpr const char* WIFI_SSID     = "REPLACE_WITH_2_4_GHZ_SSID";
constexpr const char* WIFI_PASSWORD = "REPLACE_WITH_WIFI_PASSWORD";

// EMQX Cloud broker; TLS port 8883
constexpr const char* MQTT_URI =
    "mqtts://oea629d2.ala.asia-southeast1.emqxsl.com:8883";

// Device account created under EMQX Authentication
constexpr const char* MQTT_USERNAME = "securesense-s3";
constexpr const char* MQTT_PASSWORD = "REPLACE_WITH_S3_MQTT_PASSWORD";

// Same value must appear in webapp/.env
constexpr const char* MQTT_TOPIC_PREFIX = "securesense/hub1";

// Paste the entire EMQX deployment CA PEM, not this placeholder.
constexpr const char* MQTT_CA_CERT = R"PEM(
-----BEGIN CERTIFICATE-----
REPLACE_WITH_ACTUAL_CA_PEM_BODY
-----END CERTIFICATE-----
)PEM";
```

**Important:** In the template, the active line is `constexpr const char* MQTT_CA_CERT = nullptr;`. **Replace/comment out that active line** when you add the PEM definition; do not leave two active definitions of `MQTT_CA_CERT`. Likewise retain one definition per setting. The example certificate block inside the template is initially commented out.

**No PubSubClient library is needed.** The GitHub S3 sketch uses the ESP-IDF `mqtt_client.h` bundled with Arduino-ESP32 core 3.x.

### Wi-Fi channel and ESP-NOW coexistence

- S3 has one shared 2.4 GHz radio. When it associates with the router, ESP-NOW operates on that router's channel.
- The **Stage 8 C3** scans channels **1–13**, dwelling ~250 ms per channel, looking for the hub. It starts at channel 4 and locks onto the active hub channel when it receives the reply.
- The previous live test successfully ran on **router channel 11**. You do **not** need to force the router to channel 4 when **both boards run the latest Stage 8 firmware**.
- The router must provide **2.4 GHz Wi-Fi**; the S3 cannot join a 5 GHz-only network. Ensure that your region's channel selection is supported; this project's scanner searches 1–13.

### Optional isolated offline ESP-NOW mode

If you need to test *without* Wi-Fi/MQTT, set `WIFI_SSID = ""` in `secrets.h` and upload the S3; the S3 will use fixed channel 4. Re-enable the SSID before the cloud test. A fresh S3 upload is only needed if its compiled credentials/configuration change.

---

## 11. Arduino IDE board package and serial settings

1. Install/open **Arduino IDE 2.x**.
2. Go to **File → Preferences → Additional Boards Manager URLs**.
3. Add the Espressif stable board index URL (if not already present):

   ```text
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

4. Go to **Tools → Board → Boards Manager** and search **esp32**.
5. Install **esp32 by Espressif Systems**. Use Arduino-ESP32 **3.x**; the Stage 8 contributor reported successful compilation on **3.3.12**, so that version is the most reproducible if offered. Newer 3.x releases may also work but may need regression testing.
6. Use **USB data cables**, not charge-only cables. Connect one board at a time initially and note its COM port (Windows Device Manager → Ports).
7. Never assume a board is on a fixed COM port after switching laptops. The previous system used S3 COM10 and C3 COM11, but a new laptop will likely assign different numbers.

| Arduino menu | ESP32-S3 main hub | ESP32-C3 SuperMini node |
|---|---|---|
| Board | **ESP32S3 Dev Module** | **ESP32C3 Dev Module** |
| USB CDC On Boot | **Disabled** (previous proven UART0 Serial) | **Enabled** (previous proven native USB Serial) |
| Serial Monitor baud | **115200** | **115200** |
| Serial Monitor line ending | **New Line** | New Line (status readout) |
| Port | S3's actual COM port | C3's actual COM port |
| Core | Espressif Arduino-ESP32 **3.x** | Same compatible **3.x** |

**S3 serial caveat:** On the earlier S3 board, with USB CDC On Boot Enabled, the COM port showed only ROM boot lines, not `Serial` application logs. The working configuration was **USB CDC On Boot Disabled** with the UART0 USB connection. On another S3 board with a different USB/USB-UART port, verify which connector is used rather than blindly changing code.

**C3 radio configuration:** firmware requests `WIFI_POWER_8_5dBm`, previously necessary for successful ESP-NOW transmission in the prototype. Keep it unless RF testing justifies a change.

---

## 12. Flash and test the S3

**Do not edit `SecureSenseProtocol.h` during setup.** Both board folders already contain matching protocol-v2 headers.

1. Connect the S3 using its tested USB/UART port and a data cable.
2. In Arduino IDE choose **File → Open** and open:

   ```text
   C:\Projects\SecureSense\03_S3_SecureSense_Hub\03_S3_SecureSense_Hub.ino
   ```

3. Verify there is a `secrets.h` tab/file in the **same** sketch folder and its Wi-Fi/MQTT entries and CA certificate are configured.
4. Select **ESP32S3 Dev Module**, the S3 COM port, **USB CDC On Boot Disabled**.
5. Click **Verify** (check mark). Resolve compile errors first (usually missing `secrets.h`, wrong board core, or incomplete certificate string).
6. Click **Upload**. Do not disconnect during upload.
7. Open S3 **Serial Monitor → 115200 baud → New Line**. Press Reset once if logs were missed.
8. Look for output of this form (IP, RSSI and channel will vary):

   ```text
   === SECURESENSE MAIN HUB: ESP32-S3 (Stage 8: Wi-Fi + MQTT) ===
   [RADIO] S3 HUB READY. Power C3 node and look for C3 HELLO.
   [WIFI] Connected ip=192.168.x.x rssi=-55 dBm channel=11
   [MQTT] Connecting to mqtts://...:8883 as securesense-hub-... ...
   [MQTT] Connected; subscribed to commands.
   ```

9. In the input field above the S3 Serial Monitor type `net`, select **New Line**, press Enter. Expect `Wi-Fi CONNECTED | radio channel=<N> | MQTT CONNECTED | NTP synced` once both services settle.
10. Other S3 diagnostic commands: `help`, `who`, `ping`, `status`. You can use the last three after the C3 is powered and discovered.

**Do not confuse a queued ESP-NOW send with command success**: the hub waits for an actual C3 response and reports `ACCEPTED`, a rejection reason, or `TIMEOUT_UNKNOWN`.

---

## 13. Configure, flash and test the C3

1. Connect the C3 sensor node with the intended Reed/PIR/LED/buzzer/DIP/button wiring.
2. In Arduino IDE open:

   ```text
   C:\Projects\SecureSense\02_C3_SecureSense_Node\02_C3_SecureSense_Node.ino
   ```

3. Review the constants near the top:

   ```cpp
   constexpr uint8_t PIN_REED=0, PIN_PIR=3, PIN_GREEN=2, PIN_RED=8;
   constexpr uint8_t PIN_BUTTON=9, PIN_BUZZER=10, PIN_SW_REED=6, PIN_SW_PIR=7;
   constexpr bool REED_HAS_PCB_RC=false; // direct breadboard Reed; review for PCB
   constexpr bool DIP_SWITCHES_WIRED=true; // if physical selector DIP is wired
   ```

4. Keep `DIP_SWITCHES_WIRED=true` **only if both GPIO6/7 DIP inputs really are installed**. If neither DIP is wired on a temporary test node, choose `false` so both sensors are logically enabled. On the actual PCB, leave `true` and test switch operation.
5. Confirm `REED_CLOSED_LEVEL=LOW` matches the electrical contact and magnet placement. It refers to contact continuity, **not simply whether the physical door looks closed**.
6. Select **ESP32C3 Dev Module**, C3 COM port, **USB CDC On Boot Enabled**, baud 115200.
7. Click **Verify**, then **Upload**.
8. Open C3 Serial Monitor. It should identify `SECURESENSE ESP32-C3 NODE ID 2`, `PROTOCOL v2`, DIP selection and `[RF] C3 TX power request 8.5 dBm: OK`.
9. Allow ~60 seconds for the HC-SR501 PIR initialization before treating its motion output as fully ready.
10. Keep the S3 powered and connected to Wi-Fi. The C3 will broadcast discovery while sweeping Wi-Fi channels; when discovered, its link will settle on the router's channel.
11. Check S3 Serial Monitor for:

   ```text
   *** C3 NODE 2 DISCOVERED: <C3 MAC> on channel <router channel> ***
   [C3 ...] REED GPIO0 raw=... CLOSED=... | PIR GPIO3 raw=... READY=...
   ```

12. In S3 Serial Monitor send `who`, `ping`, then `status`. Check that both `ping` and `status` receive correlated C3 results.
13. Test Reed continuity, DIP modes and PIR signals before arming. If the Reed is shown OPEN unexpectedly, inspect magnet alignment and wiring; in a previous prototype a nearby resistor touched an LED leg and produced a false reading.

**Single-node limit:** the current S3 accepts one C3 MAC/Node ID 2 at a time. Turn off another C3 advertising ID 2 and restart the S3 when switching to a different board; do not assume this code already supports two simultaneous nodes.

---

## 14. Use EMQX Monitor and MQTTX

### A. EMQX browser console

1. In EMQX Cloud open your deployment.
2. Go to **Monitor → Clients**.
3. Confirm the S3 connected with **Username** `securesense-s3` and **Client ID** similar to `securesense-hub-4b9938`. The Client ID does **not** equal the username.
4. Later you should see `securesense-web` (backend) and `securesense-monitor` (MQTTX). Use **Monitor → Subscriptions** and **Retained messages** to investigate topics if needed.

### B. MQTTX Desktop — create exact monitoring connection

1. Download/install/open **MQTTX Desktop** at https://mqttx.app/.
2. Click **+ New Connection** (usually on the Connections screen).
3. Enter these values:

   | Field in MQTTX | Value |
   |---|---|
   | Name | `SecureSense Monitor` |
   | Client ID | `securesense-mqttx-monitor` (must be unique among connected clients) |
   | Protocol | `mqtts` / MQTT over TLS |
   | Host/address | `oea629d2.ala.asia-southeast1.emqxsl.com` (without `mqtts://` if protocol is chosen separately) |
   | Port | `8883` |
   | Username | `securesense-monitor` |
   | Password | Password for this EMQX monitoring account |
   | SSL/TLS | **Enabled** |
   | Server certificate verification | **Enabled** |
   | CA file | Select the genuine downloaded `emqxsl-ca.crt` **if MQTTX requires one** |
   | Client certificate / client key | **Not required** for this normal username/password TLS setup |
   | Keepalive | e.g. `60` seconds |
   | Clean Start / Clean Session | Default (`true`) is appropriate for this monitoring client |

4. Click **Connect**. The MQTTX connection should display a connected/green status.
5. Click **New Subscription**. Enter topic filter `securesense/hub1/#`, QoS **1**, then confirm.
6. You should receive retained online/status messages even if no new alarm has just happened. Trigger sensor changes to see new status messages.
7. Set the received payload viewer to **JSON** if desired. A typical status looks like:

   ```json
   {
     "node": 2,
     "armed": false,
     "alarm": false,
     "reedEnabled": true,
     "reedClosed": true,
     "pirEnabled": true,
     "pirReady": true,
     "pirMotion": false,
     "rssi": -45
   }
   ```

   Actual values depend on your sensors; the firmware's actual status contains more fields including `seq`, `uptimeMs`, `silenced`, LEDs, buzzer and timestamp.

### Topic-by-topic guide

| Topic (`securesense/hub1/` prefix omitted below) | Who publishes | Retained? | Meaning |
|---|---|---|---|
| `hub/online` | S3 | Yes | Online/offline with broker Last Will |
| `hub/status` | S3 | Yes | Router Wi-Fi channel, RSSI, node MAC, uptime, etc. |
| `node/2/online` | S3 | Yes | C3 online/offline estimate |
| `node/2/status` | S3 | Yes | Reed, PIR, armed, alarm, silenced etc. |
| `node/2/event` | S3 | No | ARMED, DISARMED, ALARM, SILENCED, ALARM_RESET_REARMED, NODE_REBOOTED |
| `node/2/cmd` | Authenticated backend | **Never** | Security command request |
| `node/2/cmd_result` | S3 | No | Correlated command outcome |

**Do not publish commands from the read-only monitor account.** In the designed architecture, the authenticated web backend is the command publisher. Avoid enabling a broad publish ACL just to test MQTTX.

**Why 8084 is not used here:** MQTTX Desktop and Node.js connect to MQTT/TLS at 8883; 8084 is for MQTT **over secure WebSockets**, a different transport. The current React frontend uses a WebSocket connection **to Fastify**, not to the EMQX WebSocket endpoint.

---

## 15. Set up the Fastify backend and React frontend

The repo contains *both* backend and frontend in one `webapp` package. You don't need separate GitHub repos.

### A. Install npm packages

In a **VS Code terminal** (Terminal → New Terminal) or PowerShell:

```powershell
Set-Location 'C:\Projects\SecureSense\webapp'
node --version
npm --version
npm ci --include=dev
```

`npm ci` uses the checked-in `package-lock.json`, installing Fastify, MQTT.js, React, Vite etc. The `--include=dev` ensures Vite is present. If `npm ci` reports a lockfile/package mismatch, review repo changes and then use `npm install --include=dev` only if necessary.

If you see `'vite' is not recognized`, first verify you're inside the **same** `webapp` folder and run `npm ci --include=dev` again. Do not try to run Vite from the repository root.

### B. Create the private `.env`

Inside `C:\Projects\SecureSense\webapp`:

```powershell
Copy-Item '.env.example' '.env'
notepad .env
```

Edit to the following. **Do not copy these placeholders literally for passwords.**

```ini
# Fastify listening address for this Windows PC only
HOST=127.0.0.1
PORT=3000

# EMQX Cloud — web backend MQTT account (different from S3 account)
MQTT_URL=mqtts://oea629d2.ala.asia-southeast1.emqxsl.com:8883
MQTT_USERNAME=securesense-web
MQTT_PASSWORD=REPLACE_WITH_WEB_BACKEND_MQTT_PASSWORD
MQTT_TOPIC_PREFIX=securesense/hub1
NODE_ID=2

# Local SQLite database and login sessions
DB_PATH=data/securesense.db
SESSION_DAYS=7
COOKIE_SECURE=false

# Optional first-run bootstrap: prefer using the CLI in Section 16
BOOTSTRAP_USERNAME=
BOOTSTRAP_PASSWORD=
```

**Rules:**

- `MQTT_URL` is the **full** `mqtts://...:8883` URI, *not* `localhost:1883`.
- Use **web account** `securesense-web`, not `securesense-s3` or `securesense-monitor`.
- `MQTT_TOPIC_PREFIX` must match the S3 `MQTT_TOPIC_PREFIX` **exactly** (`securesense/hub1`).
- `NODE_ID=2` must match the C3 packet's node ID (the current single-node application expects 2).
- `HOST=127.0.0.1` is intentionally local-only. `COOKIE_SECURE=false` is appropriate for this local HTTP test; set secure cookies with a correctly configured HTTPS deployment.
- Keep `BOOTSTRAP_USERNAME`/`BOOTSTRAP_PASSWORD` empty if creating users with the safer interactive CLI. Don't leave a reusable bootstrap password in an `.env` when not needed.
- The filename must be exactly **`.env`**, not `.env.example` or `.env.txt`.

### C. Verify the backend reads the right config **before starting it**

From the `webapp` directory:

```powershell
Get-Location
Get-ChildItem -Force -Name .env*
Test-Path -LiteralPath '.\.env'
node --input-type=module -e "import {config} from './server/config.js'; console.log('MQTT URL:',config.mqttUrl,'Prefix:',config.topicPrefix,'Node:',config.nodeId)"
```

Expected:

```text
.env
.env.example
True
MQTT URL: mqtts://oea629d2.ala.asia-southeast1.emqxsl.com:8883 Prefix: securesense/hub1 Node: 2
```

If it prints `mqtt://127.0.0.1:1883`, your `.env` file was not loaded or an existing **PowerShell environment variable** is overriding it. Check (do not print any passwords):

```powershell
Get-Item Env:MQTT_URL -ErrorAction SilentlyContinue
Remove-Item Env:MQTT_URL -ErrorAction SilentlyContinue
```

Then re-run the verification command. Also check that you have not accidentally launched a second copy from another `SecureSense-main\SecureSense-main\webapp` folder: the project previously had both ZIP-extracted and Git-cloned copies on separate drives, causing conflicting `.env` configurations.

---

## 16. Create dashboard users and run locally

### A. Create the dashboard account

From `C:\Projects\SecureSense\webapp`:

```powershell
npm run user -- add admin
```

The CLI prompts `New password (min 10 chars):`. Type a **new unique password** (characters may not echo) and press Enter. This password is for the **SecureSense website login**, not for EMQX. The account is stored in **`webapp/data/securesense.db`**.

Useful commands:

```powershell
npm run user -- list
npm run user -- passwd admin
```

If you already have `admin` in this database, use `passwd` to change its password; don't try to add a duplicate. The experimental SQLite warning under Node 24 is not inherently a connection failure.

### B. Development mode — two terminals

Open **two** VS Code/PowerShell terminals in the **same `webapp` directory**:

**Terminal 1: Fastify + MQTT + SQLite backend**

```powershell
npm run dev:server
```

Expected messages include:

```text
Server listening at http://127.0.0.1:3000
SecureSense web: http://127.0.0.1:3000 (MQTT mqtts://...:8883, prefix securesense/hub1)
[mqtt] connected to mqtts://...:8883
```

**Terminal 2: React/Vite frontend**

```powershell
npm run dev:web
```

Expected:

```text
VITE ... ready
Local: http://localhost:5173/
```

Open **http://localhost:5173**. The frontend proxies `/api` and `/ws` requests to Fastify on `127.0.0.1:3000` according to the repo's `vite.config.js`. Sign in using the **dashboard** username/password created above.

**What a working dashboard should show:** green indicators for **Server, Broker, Hub and Node**, live Reed/PIR and signal data, and responsive Arm/Disarm/Silence/Reset & Rearm controls with accepted/rejected results. Refresh the browser after a transient connection disruption if needed.

### C. One-port / one-command mode — recommended for demos

Stop the Vite and Fastify dev processes first (`Ctrl+C`), then:

```powershell
npm run build
npm start
```

Open **http://localhost:3000**. `npm run build` puts the frontend into `webapp/dist`; Fastify serves it together with API/WebSocket. **Vite is not required to be running after the build.**

### D. One-click Windows launcher

From the Git repo root, double-click:

```text
C:\Projects\SecureSense\start-securesense-local.cmd
```

It changes into `webapp`, checks for `.env`, installs dependencies if missing, builds React, opens `http://localhost:3000`, and starts Fastify. Keep its terminal window running. If the browser opens before Fastify is ready, refresh the page.

The launcher **does not automatically start the hardware, create EMQX accounts, or put your laptop online while powered off**. Those services must be available independently.

---

## 17. Complete hardware-to-browser test procedure

Perform these in order on a newly configured laptop. Avoid treating an MQTT connection alone as proof of correct alarm logic.

| Test | Procedure | Expected result |
|---|---|---|
| 1. S3 Wi-Fi | S3 Serial Monitor: `net` | `Wi-Fi CONNECTED`, current router channel, MQTT CONNECTED |
| 2. ESP-NOW discovery | Power C3, S3: `who` | Node 2 discovered and recent packets shown |
| 3. Link commands | S3: `ping`, `status` | C3 accepts commands; readings reflect hardware |
| 4. MQTTX | Connect monitor, subscribe `securesense/hub1/#` | Retained `hub/online`, `node/2/online`, `node/2/status` arrive |
| 5. Dashboard login | Open `localhost:5173` or `localhost:3000` | All four connectivity indicators online |
| 6. Reed contact | Move magnet; watch C3, S3, MQTTX, dashboard | `reedClosed` toggles matching actual contact state |
| 7. PIR readiness | Wait ≥60 s; walk in detection zone | `pirReady=true`; `pirMotion` responds to new motion |
| 8. DIP selection | Disarmed; choose both / Reed-only / PIR-only | Enabled indicators follow DIP, disabled sensor won't trigger alarm |
| 9. Arm | Return enabled Reed to closed state, wait for selected PIR to be ready; click **Arm** | Result `ACCEPTED`, armed=true, local armed indication |
| 10. Intrusion | Open monitored Reed or trigger a new PIR motion event | C3 locally sounds; alarm=true; MQTT event; dashboard alarm state |
| 11. Remote Silence | Click **Silence** | Buzzer stops, but `armed=true`, `alarm=true`, `silenced=true` |
| 12. Local Silence | In a separate alarm incident, hold GPIO9 ~3 s | Same buzzer-silence behaviour; does not disarm |
| 13. Unsafe Rearm | While enabled Reed remains OPEN, click Reset & Rearm | C3 rejects `REED_OPEN`; alarm remains latched |
| 14. Safe Rearm | Close Reed ≥2 s; if PIR enabled, wait ready and CLEAR ≥10 s; click Reset & Rearm | `ACCEPTED`, `armed=true`, `alarm=false`; protection restored |
| 15. Re-alert | Open Reed again without disarming | New alarm occurs; shows system resumed monitoring |
| 16. Disarm | Click **Disarm** | `armed=false`, `alarm=false`, buzzer off |
| 17. S3 outage | With C3 armed, power down S3; trigger Reed | **C3's local alarm still works**; cloud live updates may stop |
| 18. Reconnect | Restore S3 and Wi-Fi/MQTT | Channel scan/re-discovery, current state republished; online status returns |

**Recovery interlocks** in current C3 code: Reed closed continuously for 2 s if selected; PIR warm-up of ~60 s and clear continuously for 10 s if selected; DIP configuration must match the latched mode; GPIO9 not held; node must already be armed with a latched alarm. Rearm is **an explicit authorized request**, not an automatic assertion that the area is safe. Relevant source constants are `REARM_REED_CLOSED_MS=2000`, `REARM_PIR_CLEAR_MS=10000`, `PIR_WARMUP_MS=60000`, and `BUTTON_HOLD_TO_SILENCE_MS=3000`.

**Check results carefully:** web backend HTTP status `200` means the HTTP request completed; its JSON `result` may still be a rejection code. A timeout such as `TIMEOUT_UNKNOWN` or `NO_RESPONSE` does not prove the command failed or succeeded; check fresh node state before retrying.

**No push notifications yet:** the current repository records and shows web dashboard alarm events; it does not yet implement guaranteed mobile Web Push/Telegram notifications.

---

## 18. Normal daily startup (without reconfiguration)

Once devices are flashed and your `.env` is saved, **you do not need to repeat installation or change settings every time**.

1. Supply power to ESP32-S3 and ESP32-C3.
2. Ensure the configured **2.4 GHz Wi-Fi** and the EMQX Cloud deployment are available.
3. On your PC, double-click **`start-securesense-local.cmd`** from the cloned repo, *or* run `npm start` from `webapp` if the frontend was already built.
4. Open **http://localhost:3000**, sign into SecureSense.
5. MQTTX is optional: open it only when you want to debug telemetry.
6. Keep the PC awake while you need the website and backend; the C3 local alarm still runs if you close the website or power off the PC/S3.

Changes that **do** require additional action:

- **Wi-Fi name/password changed:** edit S3 `secrets.h`, recompile/reflash **S3**.
- **Broker hostname/CA or S3 MQTT credentials changed:** edit S3 `secrets.h`, reflash **S3**.
- **Backend MQTT credentials changed:** edit `webapp/.env`, restart backend; **no hardware flashing**.
- **MQTTX password changed:** update and reconnect MQTTX; **no firmware changes**.
- **Dashboard password changed:** `npm run user -- passwd <username>`; no firmware changes.
- **C3 GPIO mapping, radio/ESP-NOW algorithm, protocol changed:** reflash affected board(s), often **both** if protocol changes; match headers.
- **GitHub source updated:** `git pull`, `npm ci --include=dev` if dependency lockfile changed, rebuild frontend, and test before flashing updated firmware.

---

## 19. Moving an existing installation to a new laptop

There are **two distinct options**:

### Option 1 — Fresh laptop, same EMQX deployment, fresh dashboard login (simplest)

1. `git clone` repository on new laptop.
2. Install Arduino ESP32 core, Node/npm, MQTTX.
3. Recreate `03_S3_SecureSense_Hub/secrets.h` using existing 2.4 GHz Wi-Fi info, **rotated or confirmed S3 MQTT credential**, existing EMQX broker and fresh CA certificate. The S3 does *not* need reflashing if the **same already-flashed board** is still using compatible Wi-Fi/broker settings.
4. Create `webapp/.env` using the web MQTT credentials and the same prefix `securesense/hub1`.
5. `npm ci --include=dev`; `npm run user -- add admin` to create a new dashboard login.
6. Start the website and test. Historical local event data and old dashboard users are **not carried over**.

### Option 2 — Move users/history from old laptop

1. Stop the old Fastify server **cleanly** before copying the SQLite database.
2. The database by default lives at `old-project\webapp\data\securesense.db`. Copy it securely to the new clone's `webapp\data\securesense.db` **while the app is stopped**. If SQLite WAL sidecar files are present, use SQLite's backup API or perform a clean checkpoint/shutdown rather than copying a live database without coordination.
3. Preserve confidentiality: the SQLite database includes users, password hashes, sessions, events and audit logs; treat it as a private backup.
4. Recreate the new laptop's `webapp/.env` separately (never get passwords from GitHub), install packages and start server.
5. Existing dashboard credentials may work because users are in the copied database, but consider password resets to invalidate old sessions.

**If the hardware already has the Stage 8 firmware flashed and the Wi-Fi/EMQX settings are unchanged, moving laptops does not require flashing either board.** Only the web backend needs to run from the new PC with its own credentials.

---

## 20. Common errors and fixes

| Error or symptom | Likely cause / exact check |
|---|---|
| `fatal: not a git repository` | You downloaded a ZIP, or are in the wrong folder. `git clone ...`; run `git remote -v` in the clone containing `.git`. `cd ..` alone won't create `.git`. |
| S3 compile: `Copy secrets.example.h to secrets.h` | Create `03_S3_SecureSense_Hub/secrets.h` **beside** `.ino`; file must be named exactly `secrets.h`. |
| S3 TLS certificate error | Confirm current EMQX hostname, CA PEM, certificate validity, NTP/system time, username/password, and port 8883. Do not call `setInsecure()`. Initial NTP timing can cause transient TLS failures; review reconnect logs. |
| Wi-Fi connects but C3 not found | Latest Stage 8 C3 scanner required when router not channel 4; verify both headers protocol v2, TX power 8.5 dBm, C3 antenna placement, S3 powered, and `who` link age. |
| S3 Serial Monitor only shows ROM boot banner | For the **previously tested S3 USB/UART connection**, set USB CDC On Boot **Disabled**, use correct UART COM port, 115200 and reflash/reset. |
| C3 USB Serial Monitor empty | Previous C3 native USB setup required USB CDC On Boot **Enabled**; verify correct COM port, 115200, data cable. |
| C3 Reed reports `raw=1` while physically near magnet | `raw=1` = HIGH/open for direct pull-up wiring. Check Reed-to-GND continuity, magnet orientation, power/LED leg shorts and `REED_HAS_PCB_RC`. |
| Can't ARM; `NO_SENSORS_SELECTED` | Both DIP inputs read OFF, or `DIP_SWITCHES_WIRED=true` but no physical switches fitted. |
| ARM rejected; Reed open | Close electrical Reed contact; verify `REED_CLOSED_LEVEL`, magnet and wiring. |
| ARM rejected; PIR not ready | Wait ~60 seconds; selected PIR must be ready. |
| RESET_REARM rejected | Follow the exact C3 result: Reed stable 2 s; PIR clear 10 s; unchanged DIP configuration; GPIO9 released. |
| Backend logs `ECONNREFUSED 127.0.0.1:1883` | `.env` is missing, edited wrong copy, or `MQTT_URL` overridden. Verify via `node --input-type=module -e ...` in Section 15. If EMQX is intended, `MQTT_URL` must start with `mqtts://` and end `:8883`. |
| Backend logs `connack timeout` then succeeds | A transient connection delay can occur; if it persists, inspect Internet/TLS/auth and EMQX clients; do not assume the app is broken after one timeout. |
| `vite` is not recognized | In `webapp`, run `npm ci --include=dev`, then `npm run dev:web`; install correct Node version. |
| `/api/me` returns HTTP 401 on login screen | Expected when an unauthenticated browser probes login status. Sign in with valid dashboard account. |
| `No users yet` | From correct `webapp`, run `npm run user -- add admin`, or use bootstrap fields **only on first empty DB**. |
| MQTTX connected but no telemetry | Check topic subscription `securesense/hub1/#`, broker, ACLs, S3 online and node status; retained messages only if published. |
| Only S3 and MQTTX in EMQX Clients | Fastify backend not running or its `.env`/web account wrong. EMQX Client ID and Username are distinct. |
| EMQX blocks publish/subscribe | Authentication may succeed but **Authorization** rules are missing or Deny shadows Allow. Review Section 8 and logs. |
| Dashboard says Broker/Hub/Node offline | Check if broker, S3 and C3 each actually online. A browser connecting to Fastify alone does not prove S3/C3 health. |
| Browser page doesn't load on :5173 | Start `npm run dev:web`, or instead build with `npm run build` and use `npm start` at :3000. |
| Browser page shows old content | Rebuild frontend (`npm run build`), restart Fastify, hard-refresh; don't serve two old copies from different folders. |
| `COOKIE_SECURE=true` on normal HTTP | Use `false` for local plain HTTP development; enable secure cookie only for correctly configured HTTPS deployment. |
| Port 3000/5173 already used | Stop duplicate dev processes. Use `Get-NetTCPConnection -LocalPort 3000,5173 -ErrorAction SilentlyContinue` to inspect listeners. |
| MQTT connection drops after Wi-Fi scan | S3's shared radio can interrupt ESP-NOW during reconnection; C3 continues local alarm and rescans channels until link restored. |

Useful command to check environment **without printing passwords**:

```powershell
# From webapp:
node --input-type=module -e "import {config} from './server/config.js'; console.log(config.mqttUrl, config.topicPrefix, config.nodeId)"
```

---

## 21. GitHub updates and secrets hygiene

### Save code safely

From the Git **repository root** (the one with `.git`):

```powershell
git status --short
git remote -v
git pull --ff-only origin main
```

For a deliberate documentation or code change, review the paths **before staging**:

```powershell
git add README.md
git status --short
git diff --cached
git commit -m "Improve SecureSense reproducible setup guide"
git push origin main
```

**Never use `git add .` blindly** if your working folder contains configuration backups, certificates/keys or database exports.

### Check tracked or ignored sensitive files

The repository's current `.gitignore` includes `secrets.h`, `webapp/.env`, `webapp/data/`, `webapp/node_modules/`, `webapp/dist/` but may **not** exclude alternate names such as `secrets_backup.h`, `settings-private.h`, `.env.backup` or copied database files.

Before pushing:

```powershell
git status --short --untracked-files=all
git check-ignore -v -- '03_S3_SecureSense_Hub/secrets.h' 'webapp/.env'
git ls-files | Select-String -Pattern 'secrets|\.env|\.db|backup|password|key'
```

Add extra ignore patterns for local secret backup files as necessary. If a secret was already **tracked**, `.gitignore` alone cannot untrack it. Remove it from Git history appropriately and **rotate the credential immediately**.

### GitHub source versus live settings

- GitHub stores code, public templates and documentation.
- Your private `.env` and `secrets.h` belong only on your trusted devices/secure backups.
- EMQX Authentication/Authorization live in the EMQX deployment, **not GitHub**.
- User accounts/event history live in your configured `DB_PATH` (SQLite), or in Postgres when `DATABASE_URL` is set (cloud hosting), **not GitHub**.
- S3 and C3 uploaded firmware persist in flash until replaced; they do not need reflashing to start the dashboard on another PC.

---

## 22. Deployment options, backups and security limitations

### No-card / local deployment (currently working)

- Keep EMQX Cloud as broker within the chosen deployment plan/limits.
- Run Fastify and the built React app on your Windows laptop using `start-securesense-local.cmd`.
- `localhost:3000` works while that laptop runs; MQTTX is optional.
- Back up the SQLite database and your password manager entries.

### Private phone access (optional, after local test)

A VPN-based private-access tool such as **Tailscale Serve** can proxy your local backend privately over HTTPS to your enrolled phone. This does **not** mean GitHub Pages hosts the backend. The PC must remain powered and connected; configure the secure cookie and trusted proxy appropriately and validate login/WebSocket functionality from the phone. Review Tailscale's current docs before enabling.

### Render/GitHub Pages

`render.yaml` deploys the dashboard on Render's **free** plan (no card), with a free **Neon Postgres** database through `DATABASE_URL`, because a free Render service has no permanent disk for SQLite. Step-by-step instructions are in the main [README, section 33](../README.md#33-free-cloud-hosting-render--neon). GitHub Pages alone supports only the static frontend, not the Fastify/MQTT backend.

### Known prototype security and reliability limits

- ESP-NOW pairing/command packets are currently **unencrypted and unauthenticated**; attacker/spoofing risk remains.
- The C3 firmware starts **DISARMED after restart**; power interruption is a security gap.
- Single C3 node paired at a time; no multi-node deployment yet.
- Alarm events while MQTT is disconnected are **not durably queued**, so notifications/event logs can miss historical incidents.
- **No implemented mobile push/Telegram notification service** in the checked code.
- No validated battery runtime/charger or production enclosure protection.
- **No guarantee of continuous protection** from a laptop-hosted backend that may sleep or shut down.
- MQTT/TLS secures the **S3 ↔ broker** and **backend ↔ broker** legs, not the unencrypted ESP-NOW leg.

Do **not** deploy this prototype as the sole protection for real property without addressing those limitations and validating hardware safety, RF performance, restart behaviour, permissions, and long-term event delivery.

---

## 23. Final reproduction checklist

Tick only when observed on the **new laptop and actual devices**.

- [ ] Git, VS Code, Node/npm, Arduino IDE, ESP32 core 3.x and MQTTX installed
- [ ] Repository cloned with actual `.git` history
- [ ] S3/C3 wiring checked (including sensor voltage levels and power)
- [ ] EMQX project/deployment active; correct hostname and port copied
- [ ] Three EMQX users created/verified with **unique** passwords
- [ ] S3, backend and monitor ACL Allow rules created; All Users Deny fallback verified
- [ ] EMQX CA downloaded and correctly pasted in S3 `secrets.h`
- [ ] Wi-Fi SSID/password and S3 MQTT account saved in private `secrets.h`
- [ ] Arduino C3 vs S3 board, USB CDC, COM ports, 115200 settings checked
- [ ] S3 uploaded and reports Wi-Fi + MQTT connected
- [ ] C3 uploaded (correct breadboard/PCB constants) and radio scanner finds S3
- [ ] `who`, `ping`, `status`, `net` tested on S3
- [ ] EMQX Monitor → Clients shows S3 connection
- [ ] MQTTX monitor connects with unique client ID, TLS/CA and `securesense/hub1/#` subscription
- [ ] MQTTX receives fresh C3 status and alarm events
- [ ] `npm ci --include=dev` completed in `webapp`
- [ ] `webapp/.env` created with **web** MQTT account, correct TLS URL/prefix/node ID
- [ ] Config verification prints `mqtts://...:8883` (not localhost 1883)
- [ ] Dashboard user created in SQLite
- [ ] Fastify logs `[mqtt] connected...`
- [ ] React Vite page loads at `localhost:5173`
- [ ] `npm run build` and `npm start` serve one-port site at `localhost:3000`
- [ ] Dashboard shows Server, Broker, Hub and Node connected
- [ ] Reed, PIR and DIP selection modes checked
- [ ] ARM → alarm → silence → unsafe rearm rejection → successful rearm → re-alert → DISARM checked
- [ ] C3 local alarm verified with S3 powered off
- [ ] Private credentials absent from Git and never pasted into shared logs/screenshots
- [ ] Team members know local backend **must remain running** for live web access

---

## 24. Official reference links

- SecureSense source: https://github.com/Abdul-Rahman-bme/SecureSense
- Arduino IDE: https://www.arduino.cc/en/software
- Espressif Arduino-ESP32 installation: https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html
- Espressif board manager index: https://espressif.github.io/arduino-esp32/package_esp32_index.json
- EMQX Cloud: https://cloud.emqx.com/
- EMQX Serverless creation: https://docs.emqx.com/en/cloud/latest/create/serverless.html
- EMQX default Authentication: https://docs.emqx.com/en/cloud/latest/deployments/default_auth.html
- EMQX default Authorization: https://docs.emqx.com/en/cloud/latest/deployments/default_authz.html
- EMQX MQTTX connection guide: https://docs.emqx.com/en/cloud/latest/connect_to_deployments/mqttx.html
- MQTTX Desktop: https://mqttx.app/
- Node.js: https://nodejs.org/
- Git for Windows: https://git-scm.com/install/win
- VS Code: https://code.visualstudio.com/
- Tailscale Serve: https://tailscale.com/kb/1312/serve

---

**End of guide.** The normal operating sequence is: **power C3/S3 → allow Wi-Fi/ESP-NOW/MQTT to connect → run the Fastify-backed website → sign in**. Reconfigure only when the network, MQTT credentials, deployment, hardware or firmware changes.

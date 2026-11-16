# SecureSense Stage 8 — S3 Wi-Fi + MQTT bridge (C3 + S3)

## Stage 8 scope
The S3 hub joins a 2.4 GHz router and bridges the C3 node to an MQTT broker, so a backend/web app can show live status and send commands. Packet format is unchanged (protocol **v2**). The C3 alarm logic is unchanged.
It is still a lab prototype: the ESP-NOW link between C3 and S3 is still unencrypted.

### What changed
- **S3 hub** (`03_S3_SecureSense_Hub/03_S3_SecureSense_Hub.ino`): Wi-Fi with auto-reconnect, NTP time, and MQTT via the ESP-IDF `esp-mqtt` client that ships with Arduino-ESP32 3.x. **No extra library to install.** MQTT runs in its own task, so a slow broker never blocks ESP-NOW handling.
- **C3 node** (`02_C3_SecureSense_Node/02_C3_SecureSense_Node.ino`): when it can't hear the hub, the node sweeps channels 1–13 (250 ms per channel, starting at 4), broadcasting HELLO until the hub answers. This is needed because once the S3 is on Wi-Fi it must use the **router's** channel. ESP-NOW peers now use channel `0` ("current channel").
- **Offline mode**: leave `WIFI_SSID` empty and the hub behaves like Stage 7 (fixed channel 4, Serial only).

### Setup
1. Sketch folders are named exactly like their `.ino` files, so the Arduino IDE opens them directly (File → Open the `.ino`).
   In `03_S3_SecureSense_Hub/`, copy `secrets.example.h` to `secrets.h` and fill in the Wi-Fi details, broker URI, hub username/password, topic prefix and broker CA certificate. `secrets.h` is git-ignored.
2. Flash the S3 first, then the C3. Both need the Stage 8 sketches. (A Stage 7 C3 only works if your router happens to be on channel 4.)
3. Open the S3 Serial Monitor (115200, Newline). Expected: `[WIFI] Connected ... channel=N`, `[MQTT] Connected`, then `*** C3 NODE 2 DISCOVERED ... on channel N ***`. Type `net` to see the Wi-Fi/MQTT state at any time.
4. All Stage 7 Serial commands still work alongside MQTT.

### MQTT topics (prefix e.g. `securesense/hub1`)
| Topic | Dir | Retained | Payload |
|---|---|---|---|
| `<p>/hub/online` | hub → | yes | `online` / `offline` (broker Last Will, ~22 s after the hub dies) |
| `<p>/hub/status` | hub → | yes | JSON: uptime, Wi-Fi RSSI, channel, node MAC/link age, every 30 s |
| `<p>/node/2/online` | hub → | yes | `online` / `offline` (C3 telemetry younger than 8 s) |
| `<p>/node/2/status` | hub → | yes | JSON node state, on meaningful change + every 15 s |
| `<p>/node/2/event` | hub → | no | `ARMED`, `DISARMED`, `ALARM` (+cause), `ALARM_RESET_REARMED`, `SILENCED`, `NODE_REBOOTED` |
| `<p>/node/2/cmd` | → hub | **never** | `{"id":"abc123","cmd":"arm"}` |
| `<p>/node/2/cmd_result` | hub → | no | `{"id":"abc123","cmd":"arm","result":"ACCEPTED","armed":true,...}` |

Example status:
```json
{"node":2,"seq":812,"uptimeMs":503112,"armed":true,"alarm":false,"silenced":false,"cause":"NONE",
 "reedEnabled":true,"reedClosed":true,"pirEnabled":true,"pirReady":true,"pirMotion":false,
 "button":false,"redLed":true,"greenLed":false,"buzzer":false,"rssi":-48,"ts":1763280000}
```
`ts` is Unix seconds after NTP sync, otherwise `null`. The backend should still timestamp messages when it receives them.

### Command rules
- `cmd` can be `ping`, `status`, `arm`, `disarm`, `silence` or `rearm`. LED/buzzer tests stay Serial-only.
- `id` must be 1–40 characters from `A-Z a-z 0-9 _ -`. It is echoed back in `cmd_result`. Use a fresh id per request. A repeated id (QoS 1 redelivery) is ignored.
- Each command with a valid id gets **exactly one** result:
  - a C3 result (`ACCEPTED`, `REED_OPEN`, `PIR_STILL_ACTIVE`, ...)
  - a hub-side rejection: `HUB_BUSY`, `NODE_OFFLINE`, `HUB_RADIO_DOWN`, `SEND_FAILED`, `INVALID_REQUEST`, `UNSUPPORTED_COMMAND`
  - or `TIMEOUT_UNKNOWN` after 4 s. This does **not** mean the command failed; check `status`.
- One command at a time. Retained messages on `/cmd` are ignored, so a stale command can never replay on reconnect.

### Security requirements before leaving the bench
- Use `mqtts://` with the CA certificate. Plain `mqtt://` is for a private bench network only.
- Give the hub its own broker user with an ACL limited to its topics (see `secrets.example.h`).
- **Browsers must not publish to `/cmd`.** The web app's backend authenticates the user, then publishes the command.
- ESP-NOW encryption/authentication between C3 and S3 is still TODO.

### Web app
`webapp/` contains the dashboard backend and frontend, plus a hub simulator for testing without hardware. See [webapp/README.md](webapp/README.md).

### Known limitations
- While the S3's Wi-Fi is disconnected and rescanning, ESP-NOW may drop out briefly. The C3 then loses the hub, re-sweeps channels and relocks. The local alarm on the C3 is unaffected.
- Events that happen while MQTT is disconnected are not queued. Retained `status` is republished on reconnect, so the current state is always correct.
- Not compiled against the Arduino-ESP32 3.x core or flashed here. Hardware testing is required.

---

# Stage 7 — RESET_REARM (C3 + S3)

## Scope
A single-node, bench-only update to the working Stage 6 SecureSense C3/S3 firmware.
Adds a safe, C3-authorized RESET_REARM command and explicit command outcomes. **Does not add Wi-Fi router access, MQTT, a website, or mobile notifications yet.** This is the first building block for future authenticated MQTT control.

IMPORTANT: This is an unencrypted and unauthenticated ESP-NOW laboratory prototype. Do not use it to secure property.

## Files
- `02_C3_SecureSense_Node/02_C3_SecureSense_Node.ino`
- `02_C3_SecureSense_Node/SecureSenseProtocol.h`
- `03_S3_SecureSense_Hub/03_S3_SecureSense_Hub.ino`
- `03_S3_SecureSense_Hub/SecureSenseProtocol.h`

**Upload BOTH boards**. Protocol version is now **2**: packet size/layout and explicit command outcomes changed. An older v1 sketch on either board will not exchange telemetry with the new sketch.

## Wiring (unchanged; final PCB mapping)
- C3 GPIO0: Reed contact (to GND), with internal pull-up for direct breadboard wiring (`REED_HAS_PCB_RC=false`). If the PCB R4/R5/C7 circuit is populated, set that flag true.
- C3 GPIO3: HC-SR501 OUT. Supply the PIR properly and verify its output is within the ESP32-C3's 3.3V GPIO limits.
- C3 GPIO6: Reed selector DIP to GND; `ON` means LOW and enabled.
- C3 GPIO7: PIR selector DIP to GND; `ON` means LOW and enabled.
- C3 GPIO9: normally-open enclosure push button to GND. A 3s hold silences a latched alarm only. Do not hold during reset/boot; GPIO9 is a strapping pin.
- C3 GPIO2: green LED active LOW. GPIO8: red LED active LOW. GPIO10: transistor-driven buzzer.
- Keep tested C3 USB power arrangement. Never join independent 5V sources without checking for backfeeding.

`DIP_SWITCHES_WIRED=true` assumes the physical DIP switches are wired. If testing on a C3 without the DIP board, set it to false; with both inputs floating and the flag true, arming is rejected because both switches read OFF.

## Working radio/Serial settings preserved
- C3: ESP32C3 Dev Module, USB CDC On Boot **Enabled** for the working USB configuration, 115200 baud, Wi-Fi TX power **8.5 dBm**, ESP-NOW channel **4**, node ID **2**.
- S3: ESP32S3 Dev Module, USB CDC On Boot **Disabled** for your proven UART0/COM10 arrangement, 115200 baud, ESP-NOW channel **4**.
- This S3 hub supports **one C3 MAC at a time**. Turn off any other C3 advertising Node ID 2, then reset the S3 if switching hardware nodes.

## Upload and first link check
1. Save a copy of the known-good Stage 6 firmware.
2. Upload Stage 7 S3 sketch and its matching `SecureSenseProtocol.h` first.
3. Upload Stage 7 C3 sketch and its matching `SecureSenseProtocol.h` next.
4. Verify S3: `[RADIO] S3 HUB READY`, `*** C3 NODE 2 DISCOVERED ***` and one status line per second.
5. In S3 Serial Monitor type `who`, then `ping`, then `status`. It must report `[HUB RESULT] ... ACCEPTED` for ping/status. Use **Newline** mode, 115200 baud.
6. If discovery fails after upload, check that both boards use protocol v2, and each .ino and its header are in the same sketch folder.

## New command: `rearm` (alias `reset_rearm`)
Expected behaviour:
- Starts only from `isArmed==true` and `alarmLatched==true`.
- No enabled sensors is rejected.
- DIP state must match the mask latched at the previous ARM command; switches can't silently weaken protection during alarm recovery.
- If Reed is enabled: electrical contact must be CLOSED at the pin, stable for >=2 seconds.
- If PIR is enabled: it must have completed 60s startup, be CLEAR (LOW), continuously clear for >=10 seconds.
- GPIO9 must not be held.
- When accepted: clear alarm and silence latch; remain ARMED with the same locked sensor mask; red LED returns to steady; buzzer stops; future NEW violations trigger another alarm.
- When rejected: remain ARMED, keep alarm latch and any existing buzzer-silence state unchanged. Reply with explicit reason.
- The check is owner-initiated, NOT automatically run when the sensors become clear. PIR LOW does NOT establish that the property is free of intruders.

**If you have just cleared a PIR trigger, its HC-SR501 output may stay HIGH for a long time. The 10-second clear timer starts only when it falls LOW.**

## Suggested functional test (both DIP switches ON)
1. Ensure no C3 is in an alarm; enter `disarm` on S3.
2. Close Reed, wait for PIR READY=1, ideally clear PIR; enter `arm`.
3. Open Reed; C3 must sound local alarm, S3 must show `ARMED=1 ALARM=1`.
4. While door is still open, enter `rearm`. Expected: `[HUB RESULT] ... REED_OPEN`, still `ALARM=1`.
5. Close Reed and immediately enter `rearm`. If within 2 sec: `REED_NOT_STABLE`.
6. Keep door closed for >=2 sec; if PIR HIGH, expect `PIR_STILL_ACTIVE`. Wait until PIR is LOW for >=10 sec, then send `rearm` again.
7. Expected: `[HUB RESULT] ... ACCEPTED | C3 armed=1 alarm=0` and next status `ARMED=1 ALARM=0 SILENCED=0`. Red LED steady (armed), buzzer off.
8. Open Reed again WITHOUT disarming. Must trigger a **new** local alarm. This proves the system is again monitoring.
9. Trigger another alarm, hold GPIO9 for 3s to silence it. The alarm stays latched. `rearm` should still work after sensor recovery.
10. While armed, toggle a DIP. A `rearm` request must return `DIP_CHANGED_WHILE_ARMED` until the original switch positions are restored.
11. Try PIR-only and Reed-only modes. Recovery must ignore whichever sensor is not selected. With both OFF, `arm` must be rejected.
12. Verify local alarm continues with S3 unplugged. When S3 reconnects, it can send `rearm` after the conditions are safe.

## Main S3 Serial commands
`help`, `who`, `ping`, `status`, `arm`, `disarm`, `silence`, `rearm` / `reset_rearm`, `red`, `green`, `off`, `beep`.

## Result semantics (IMPORTANT for website/MQTT stage)
The C3 command-result ACK includes `responseToSequence`, `commandResult`, `armed`, `alarm`, `alarmSilenced`, and `alarmCause`.
S3 logs `[HUB RESULT] request=<seq> cmd=<n>: ACCEPTED/REASON` **only if** the returned sequence and command match the pending request. It also prints continuing STATUS telemetry.
If no result arrives within 4 seconds, S3 prints `Outcome UNKNOWN` and does **not** claim success. Check the actual status before retrying.
The command ACK can get lost; a timeout does NOT prove the C3 rejected the command.

## Future MQTT/web stage
The hub will authenticate an API/backend request, forward `RESET_REARM` to C3, correlate the response, and publish success or rejection. Do not directly expose the S3 Serial interface or raw unencrypted ESP-NOW commands as remote home-security controls. An authenticated backend, TLS-protected MQTT and authenticated ESP-NOW are required for deployment. Connect the S3 to a 2.4 GHz router/channel configuration compatible with C3 ESP-NOW channel 4, or redesign channel management before enabling internet access.

## Verification
- Both sketches were syntax-checked with local C++ mocks of Arduino/ESP interfaces (NOT a full Arduino core build).
- The C3 reset/rearm logic passed 11 host-side scenarios covering normal recovery, unsafe sensors, DIP change, button held, and sensor-only selection modes.
- Not compiled using the installed Arduino-ESP32 3.x board package or flashed to your physical boards here. Hardware testing is required.

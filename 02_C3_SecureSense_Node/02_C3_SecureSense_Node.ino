/*
 SecureSense ESP32-C3 sensor node.
 Transport: ESP-NOW, node ID 2; no AP/router/cloud on the C3 itself.
 Stage 8: the S3 hub now joins a Wi-Fi router and must use the ROUTER's channel.
 Whenever the hub link is not live, this node sweeps channels 1-13 (starting at
 SECURESENSE_CHANNEL) broadcasting HELLO until the hub answers, then stays there.
 Board package: Espressif Arduino-ESP32 3.x (receiver callback API).
 LAB PROTOTYPE ONLY: pairing and command packets are not authenticated.
 GPIO9: hold for 3 seconds during alarm to silence the buzzer only.
 RESET_REARM: safety-gated alarm reset while keeping ARMED. Result is explicit ACK.
 DIP switches GPIO6/GPIO7: sensor selection is locked at ARM time.

 Workflow:
 1. First finish sketch 01 local IO test and calibrate REED_CLOSED_LEVEL.
 2. Flash S3 hub sketch 03; flash this C3 sketch.
 3. Watch discovery and STATUS on S3; send commands from S3 Serial Monitor.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "esp_arduino_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "SecureSenseProtocol.h"

#if !defined(ESP_ARDUINO_VERSION_MAJOR) || ESP_ARDUINO_VERSION_MAJOR < 3
#error Use Espressif Arduino-ESP32 boards package version 3.x
#endif

constexpr uint8_t PIN_REED=0, PIN_PIR=3, PIN_GREEN=2, PIN_RED=8;
constexpr uint8_t PIN_BUTTON=9, PIN_BUZZER=10, PIN_SW_REED=6, PIN_SW_PIR=7;
constexpr bool REED_HAS_PCB_RC=false;  // false=direct GPIO0-reed-GND; true=R4/R5/C7 populated
constexpr bool DIP_SWITCHES_WIRED=true;  // user final PCB/breadboard: BOTH switches GPIO6/GPIO7 wired to GND
constexpr uint8_t REED_CLOSED_LEVEL=LOW; // VERIFY USING LOCAL TEST AND MAGNET
constexpr uint32_t REED_DEBOUNCE_MS=25; // provisional after Stage 1; adjust to measurement
constexpr uint32_t BUTTON_DEBOUNCE_MS=25;
constexpr uint32_t BUTTON_HOLD_TO_SILENCE_MS=3000;
constexpr uint32_t DIP_DEBOUNCE_MS=40;
constexpr uint32_t PIR_WARMUP_MS=60000;
constexpr uint32_t STATUS_INTERVAL_MS=1000;
constexpr uint32_t HELLO_INTERVAL_MS=5000;
constexpr uint32_t HUB_LOST_TIMEOUT_MS=8000;
// Bench-only recovery interlocks. Tune from your sensor timing measurements.
constexpr uint32_t REARM_REED_CLOSED_MS=2000;
constexpr uint32_t REARM_PIR_CLEAR_MS=10000;
// Hub discovery channel sweep (Stage 8). A full 1-13 sweep takes ~3.3 s.
constexpr uint8_t CHANNEL_MIN=1, CHANNEL_MAX=13;
constexpr uint32_t CHANNEL_SCAN_DWELL_MS=250;

const uint8_t BROADCAST[6]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
uint8_t hubMac[6]={0};
bool hubKnown=false;
bool radioReady=false;
uint8_t currentChannel=SECURESENSE_CHANNEL;
uint32_t lastScanStepMs=0;
bool announcedOnCurrentChannel=false;
bool hubWasLive=false;
uint32_t lastHubSeen=0, lastHello=0, lastStatus=0, packetCount=0;
uint32_t reedEvents=0,pirEvents=0,buttonEvents=0;

struct Inbound { SecureSensePacket packet; uint8_t mac[6]; };
QueueHandle_t incoming=nullptr;

bool reedRaw, reedStable, pirRaw, pirReady=false, buttonRaw, buttonStable;
// DIP selection states are debounced; effective selection is latched at ARM time.
bool dipReedRaw=true, dipPirRaw=true, dipReedStable=true, dipPirStable=true;
bool armedReedEnabled=false, armedPirEnabled=false;
bool buttonHoldHandled=false;
uint32_t buttonPressedSince=0;
uint32_t dipReedChangedMs=0, dipPirChangedMs=0;
bool everPirClearedSinceArm=false;
uint32_t reedChangedMs=0,buttonChangedMs=0,pirHighSince=0;
bool isArmed=false, alarmLatched=false, alarmSilenced=false;
uint8_t lastAlarmCause=CAUSE_NONE;
uint32_t reedClosedSinceMs=0, pirClearSinceMs=0;
uint32_t successfulRearms=0;
bool redOn=false,greenOn=false,buzzerOn=false;
uint32_t testLedUntil=0, testBuzzerUntil=0;
uint8_t testLedMode=0; // 0 normal; 1 red; 2 green; 3 off

// The switch configuration may be changed while DISARMED only.
// Once armed, the active sensor mask does NOT change, preventing a DIP
// from silently bypassing a sensor mid-protection.
bool configuredReed(){return !DIP_SWITCHES_WIRED || dipReedStable;}
bool configuredPir(){return !DIP_SWITCHES_WIRED || dipPirStable;}
bool reedEnabled(){return isArmed ? armedReedEnabled : configuredReed();}
bool pirEnabled(){return isArmed ? armedPirEnabled : configuredPir();}
const char* selectionName(bool r,bool p){
  return r && p ? "BOTH" : r ? "REED_ONLY" : p ? "PIR_ONLY" : "NONE";
}
void printSelection(){
  Serial.printf("[MODE] selected=%s | effective=%s | active Reed=%u PIR=%u | armed=%u\n",
    selectionName(configuredReed(),configuredPir()),
    selectionName(reedEnabled(),pirEnabled()),
    (unsigned)reedEnabled(),(unsigned)pirEnabled(),(unsigned)isArmed);
}
bool reedClosed(){return reedStable==REED_CLOSED_LEVEL;}
void setLEDs(bool green,bool red){
  greenOn=green;redOn=red;
  digitalWrite(PIN_GREEN,green?LOW:HIGH);
  digitalWrite(PIN_RED,red?LOW:HIGH);
}
void setBuzzer(bool on){
  if(on==buzzerOn) return;
  if(on) { if(ledcWriteTone(PIN_BUZZER,2000)==0){Serial.println("PWM start failed");return;} }
  else ledcWriteTone(PIN_BUZZER,0);
  buzzerOn=on;
}
void updateOutputs();  // called after each command, before its result packet
void printMac(const uint8_t *m){
  for(int i=0;i<6;i++){if(i)Serial.print(':');if(m[i]<16)Serial.print('0');Serial.print(m[i],HEX);}
}

SecureSensePacket fillPacket(uint8_t type, uint8_t command=CMD_NONE) {
  SecureSensePacket p=makePacket(type,++packetCount);
  p.command=command; p.uptimeMs=millis();
  p.reedRaw=reedRaw; p.reedClosed=reedClosed();
  p.pirRaw=pirRaw; p.pirReady=pirReady;
  p.buttonPressed=buttonStable;
  p.armed=isArmed; p.alarm=alarmLatched;
  p.redOn=redOn; p.greenOn=greenOn; p.buzzerOn=buzzerOn;
  p.reedEnabled=reedEnabled();p.pirEnabled=pirEnabled();
  p.alarmSilenced=alarmSilenced;
  p.alarmCause=lastAlarmCause;
  return p;
}
bool sendPacket(const uint8_t *dest,SecureSensePacket &p) {
  if(!radioReady) return false;
  esp_err_t e=esp_now_send(dest,(const uint8_t*)&p,sizeof(p));
  if(e!=ESP_OK) Serial.printf("ESP-NOW queued-send error=%d\n",(int)e);
  return e==ESP_OK;
}
void announce(bool log=true){
  SecureSensePacket p=fillPacket(PK_HELLO);
  sendPacket(BROADCAST,p);
  if(log)Serial.println("[RADIO] HELLO broadcast (hub discovery)");
}
bool hubLinkLive(){return hubKnown && (uint32_t)(millis()-lastHubSeen)<HUB_LOST_TIMEOUT_MS;}
// Peers are registered with channel 0 ("current channel"), so they follow the hop.
void hopChannel(){
  currentChannel=currentChannel>=CHANNEL_MAX ? CHANNEL_MIN : currentChannel+1;
  esp_err_t e=esp_wifi_set_channel(currentChannel,WIFI_SECOND_CHAN_NONE);
  if(e!=ESP_OK)Serial.printf("[RADIO] set channel %u failed: %s\n",(unsigned)currentChannel,esp_err_to_name(e));
  if(currentChannel==SECURESENSE_CHANNEL)
    Serial.printf("[RADIO] Hub not found; sweeping channels %u-%u...\n",(unsigned)CHANNEL_MIN,(unsigned)CHANNEL_MAX);
}
void publish(const char *why){
  SecureSensePacket p=fillPacket(PK_STATUS);
  const uint8_t *to=(hubKnown && (uint32_t)(millis()-lastHubSeen)<HUB_LOST_TIMEOUT_MS) ? hubMac : BROADCAST;
  sendPacket(to,p);
  Serial.printf("[TX] %s seq=%lu reed=%s pir=%s ready=%d armed=%d alarm=%d to=%s\n",
    why,(unsigned long)p.sequence,reedClosed()?"CLOSED":"OPEN",
    pirRaw?"MOTION":"CLEAR",(int)pirReady,(int)isArmed,(int)alarmLatched,
    to==BROADCAST?"BROADCAST":"HUB");
}
void onRecv(const esp_now_recv_info_t *info,const uint8_t *data,int len) {
  if(!incoming || !info || !info->src_addr || len!=(int)sizeof(SecureSensePacket))return;
  Inbound item={};
  memcpy(&item.packet,data,sizeof(item.packet));
  memcpy(item.mac,info->src_addr,6);
  (void)xQueueSend(incoming,&item,0); // no printing or GPIO operations in Wi-Fi task
}
bool addHubPeer(const uint8_t *m){
  if(esp_now_is_peer_exist(m))return true;
  esp_now_peer_info_t peer={}; memcpy(peer.peer_addr,m,6);
  peer.channel=0;peer.ifidx=WIFI_IF_STA;peer.encrypt=false; // 0 = current channel
  esp_err_t e=esp_now_add_peer(&peer);
  if(e!=ESP_OK) Serial.printf("Cannot add HUB peer: %d\n",(int)e);
  return e==ESP_OK;
}
// A local button press only silences the current latched alarm.
// It deliberately does NOT disarm the node or clear the alarm latch.
void silenceAlarmLocally(){
  if(!alarmLatched || alarmSilenced) return;
  alarmSilenced=true;
  testBuzzerUntil=0; // a pending manual beep must not override silence
  setBuzzer(false);
  Serial.println("[LOCAL] GPIO9 held 3s: buzzer SILENCED; ALARM remains LATCHED and ARMED");
  publish("button_local_silence"); // show ALARM=1 and BUZZ=0 on S3
}
void disarm(){
  isArmed=false;
  alarmLatched=false;
  alarmSilenced=false;
  lastAlarmCause=CAUSE_NONE;
  armedReedEnabled=false;
  armedPirEnabled=false;
  testBuzzerUntil=0;
  setBuzzer(false);
  Serial.println("STATE => DISARMED. DIP switch changes are now allowed.");
  printSelection();
}
// Normal ARM starts from DISARMED. RESET_REARM is a separate command below.
CommandResult arm(){
  if(isArmed){Serial.println("[ARM] Already armed");return RESULT_ALREADY_ARMED;}
  const bool wantReed=configuredReed();
  const bool wantPir=configuredPir();
  if(!wantReed && !wantPir){
    Serial.println("ARM rejected: BOTH DIP switches OFF (no active sensors)");return RESULT_NO_SENSORS;
  }
  if(wantPir && !pirReady){
    Serial.println("ARM rejected: selected PIR is still warming");return RESULT_PIR_NOT_READY;
  }
  if(wantReed && !reedClosed()){
    Serial.println("ARM rejected: selected Reed indicates OPEN; check magnet/polarity");return RESULT_REED_OPEN;
  }
  armedReedEnabled=wantReed;
  armedPirEnabled=wantPir;
  isArmed=true;alarmLatched=false;alarmSilenced=false;
  lastAlarmCause=CAUSE_NONE;
  // Clear temporary manual test overrides when arming.
  testLedUntil=0;testBuzzerUntil=0;testLedMode=0;
  everPirClearedSinceArm=armedPirEnabled && !pirRaw;
  Serial.printf("STATE => ARMED; monitoring %s; DIP changes ignored until DISARM\n",
    selectionName(armedReedEnabled,armedPirEnabled));
  return RESULT_OK;
}

// Do not clear an alarm merely because a request was received.
// The C3 independently validates its currently latched sensor configuration,
// fresh GPIO values, and a stable-clear dwell. The request is one-shot:
// a rejected request must be resent after resolving the reason.
CommandResult resetRearm(){
  uint32_t now=millis();
  if(!isArmed) return RESULT_NOT_ARMED;
  if(!alarmLatched) return RESULT_NO_LATCHED_ALARM;
  if(!armedReedEnabled && !armedPirEnabled) return RESULT_NO_SENSORS;

  // A physical DIP override while armed must not silently weaken protection.
  if(DIP_SWITCHES_WIRED &&
     (configuredReed()!=armedReedEnabled || configuredPir()!=armedPirEnabled ||
      (digitalRead(PIN_SW_REED)==LOW)!=armedReedEnabled ||
      (digitalRead(PIN_SW_PIR)==LOW)!=armedPirEnabled))
    return RESULT_DIP_CHANGED;
  if(buttonStable || digitalRead(PIN_BUTTON)==LOW) return RESULT_BUTTON_HELD;

  if(armedReedEnabled){
    if(!reedClosed() || digitalRead(PIN_REED)!=REED_CLOSED_LEVEL)
      return RESULT_REED_OPEN;
    if((uint32_t)(now-reedClosedSinceMs)<REARM_REED_CLOSED_MS)
      return RESULT_REED_NOT_SETTLED;
  }
  if(armedPirEnabled){
    if(!pirReady) return RESULT_PIR_NOT_READY;
    if(pirRaw || digitalRead(PIN_PIR)==HIGH) return RESULT_PIR_ACTIVE;
    if((uint32_t)(now-pirClearSinceMs)<REARM_PIR_CLEAR_MS)
      return RESULT_PIR_NOT_SETTLED;
  }

  // Preconditions satisfied. Remain ARMED with exactly the original sensor mask.
  alarmLatched=false;
  alarmSilenced=false;
  lastAlarmCause=CAUSE_NONE;
  testLedUntil=0;testBuzzerUntil=0;testLedMode=0;
  everPirClearedSinceArm=armedPirEnabled && !pirRaw;
  setBuzzer(false);
  ++successfulRearms;
  Serial.printf("[RECOVERY] RESET_REARM ACCEPTED #%lu; still ARMED; mode=%s\n",
    (unsigned long)successfulRearms,selectionName(armedReedEnabled,armedPirEnabled));
  return RESULT_OK;
}

void triggerAlarm(const char *reason, uint8_t cause){
  if(!isArmed || alarmLatched) return;
  alarmLatched=true;alarmSilenced=false;
  lastAlarmCause=cause;
  // If the button was already pressed before the alarm, require release/repress.
  if(buttonStable)buttonHoldHandled=true;
  Serial.printf("!!! LOCAL ALARM: %s @ %lu ms !!!\n",reason,(unsigned long)millis());
  publish(reason);
}

CommandResult runCommand(uint8_t cmd){
  switch(cmd){
    case CMD_PING: Serial.println("HUB PING received");return RESULT_OK;
    case CMD_RED: testLedMode=1;testLedUntil=millis()+3000;Serial.println("TEST RED 3s");return RESULT_OK;
    case CMD_GREEN:testLedMode=2;testLedUntil=millis()+3000;Serial.println("TEST GREEN 3s");return RESULT_OK;
    case CMD_LEDS_OFF:testLedMode=3;testLedUntil=millis()+3000;Serial.println("TEST LEDS OFF 3s");return RESULT_OK;
    case CMD_BEEP:testBuzzerUntil=millis()+300;Serial.println("TEST BUZZER 300ms");return RESULT_OK;
    case CMD_GET_STATUS:Serial.println("HUB requested status");return RESULT_OK;
    case CMD_ARM:return arm();
    case CMD_DISARM:disarm();return RESULT_OK;
    case CMD_SILENCE:
      if(!alarmLatched){Serial.println("SILENCE rejected: no alarm");return RESULT_NO_LATCHED_ALARM;}
      alarmSilenced=true;testBuzzerUntil=0;setBuzzer(false);
      Serial.println("ALARM SOUND SILENCED (alarm stays latched)");return RESULT_OK;
    case CMD_RESET_REARM:return resetRearm();
    default: Serial.println("Unknown command");return RESULT_UNKNOWN_COMMAND;
  }
}
void handleRadio(){
  Inbound in;
  while(xQueueReceive(incoming,&in,0)==pdTRUE){
    if(!validPacket(in.packet))continue;
    if(in.packet.type!=PK_ACK && in.packet.type!=PK_COMMAND)continue;
    // Lock to first hub replying to our discovery. Not cryptographic authentication.
    if(hubKnown && memcmp(hubMac,in.mac,6)!=0)continue;
    if(!hubKnown){
      if(!addHubPeer(in.mac))continue;
      memcpy(hubMac,in.mac,6);hubKnown=true;
      Serial.print("[RADIO] HUB discovered: ");printMac(hubMac);Serial.println();
    }
    lastHubSeen=millis();
    if(in.packet.type==PK_COMMAND){
      Serial.printf("[RADIO] Command %u received\n",(unsigned)in.packet.command);
      CommandResult result=runCommand(in.packet.command);
      updateOutputs();  // status/ACK reflect outputs after the state transition
      SecureSensePacket ack=fillPacket(PK_ACK,in.packet.command);
      ack.responseToSequence=in.packet.sequence;
      ack.commandResult=result;
      sendPacket(hubMac,ack);
      Serial.printf("[COMMAND RESULT] cmd=%u request=%lu result=%s armed=%u alarm=%u\n",
        (unsigned)in.packet.command,(unsigned long)in.packet.sequence,
        commandResultName(result),(unsigned)isArmed,(unsigned)alarmLatched);
      publish("command_result");
    }
  }
}
void scanInputs(){
  uint32_t now=millis();
  if(DIP_SWITCHES_WIRED){
    bool nr=(digitalRead(PIN_SW_REED)==LOW);
    bool np=(digitalRead(PIN_SW_PIR)==LOW);
    if(nr!=dipReedRaw){dipReedRaw=nr;dipReedChangedMs=now;}
    if(np!=dipPirRaw){dipPirRaw=np;dipPirChangedMs=now;}
    bool changed=false;
    if(dipReedStable!=dipReedRaw && (uint32_t)(now-dipReedChangedMs)>=DIP_DEBOUNCE_MS){
      dipReedStable=dipReedRaw;changed=true;
    }
    if(dipPirStable!=dipPirRaw && (uint32_t)(now-dipPirChangedMs)>=DIP_DEBOUNCE_MS){
      dipPirStable=dipPirRaw;changed=true;
    }
    if(changed){
      printSelection();
      if(isArmed)Serial.println("[MODE] DIP changed while ARMED: active sensors remain LOCKED until DISARM");
      publish("dip_change");
    }
  }
  bool newRaw=digitalRead(PIN_REED);
  if(newRaw!=reedRaw){
    reedRaw=newRaw;reedChangedMs=now;
    // A raw excursion away from CLOSED restarts the continuous-safe dwell.
    if(reedRaw!=REED_CLOSED_LEVEL) reedClosedSinceMs=now;
    Serial.printf("[%lu] REED raw=%d\n",(unsigned long)now,(int)reedRaw);
  }
  if(reedStable!=reedRaw && (uint32_t)(now-reedChangedMs)>=REED_DEBOUNCE_MS){
    reedStable=reedRaw;reedEvents++;
    Serial.printf("[%lu] REED_%s (stable) count=%lu\n",(unsigned long)now,
      reedClosed()?"CLOSED":"OPEN",(unsigned long)reedEvents);
    if(reedClosed())reedClosedSinceMs=now;
    if(reedEnabled() && !reedClosed())triggerAlarm("REED_OPEN",CAUSE_REED);
    if(reedEnabled())publish("reed_event");
    else Serial.println("[REED] Transition ignored: Reed not selected");
  }
  bool newPir=digitalRead(PIN_PIR);
  if(newPir!=pirRaw){
    pirRaw=newPir;
    Serial.printf("[%lu] PIR raw=%d%s\n",(unsigned long)now,(int)pirRaw,pirReady?"":" (warmup, ignored)");
    if(pirRaw)pirHighSince=now;
    else {
      pirClearSinceMs=now;
      if(pirHighSince)Serial.printf("   PIR HIGH duration=%lu ms\n",(unsigned long)(now-pirHighSince));
      if(isArmed)everPirClearedSinceArm=true;
    }
    if(pirReady && pirEnabled() && pirRaw){
      pirEvents++;
      Serial.printf("[%lu] PIR_MOTION count=%lu\n",(unsigned long)now,(unsigned long)pirEvents);
      if(everPirClearedSinceArm)triggerAlarm("PIR_MOTION",CAUSE_PIR);
    }
    if(pirReady && pirEnabled())publish("pir_edge");
    else if(pirReady)Serial.println("[PIR] Transition ignored: PIR not selected");
  }
  if(!pirReady && now>=PIR_WARMUP_MS){
    pirReady=true;
    Serial.println("[PIR] 60 second warm-up completed (confirm stability experimentally)");
    publish("pir_ready");
  }
  bool newButton=(digitalRead(PIN_BUTTON)==LOW);
  if(newButton!=buttonRaw){buttonRaw=newButton;buttonChangedMs=now;}
  if(buttonStable!=buttonRaw && (uint32_t)(now-buttonChangedMs)>=BUTTON_DEBOUNCE_MS){
    buttonStable=buttonRaw;
    if(buttonStable){
      ++buttonEvents;
      buttonPressedSince=now;
      buttonHoldHandled=false;
      Serial.printf("[BUTTON] PRESS #%lu; hold >=3s during ALARM to silence\n", (unsigned long)buttonEvents);
    } else {
      if(!buttonHoldHandled)Serial.println("[BUTTON] Released: no action (short press)");
      buttonHoldHandled=false;
    }
    publish("button_edge");
  }
  if(buttonStable && !buttonHoldHandled && alarmLatched && !alarmSilenced &&
     (uint32_t)(now-buttonPressedSince)>=BUTTON_HOLD_TO_SILENCE_MS){
    buttonHoldHandled=true;
    silenceAlarmLocally();
  }
}
void updateOutputs(){
  uint32_t now=millis();
  bool green=false,red=false,buzz=false;
  if(alarmLatched){red=((now/300)%2)==0;buzz=!alarmSilenced && ((now/300)%2)==0;}
  else if(isArmed){red=true;} else {green=true;}
  if((int32_t)(testLedUntil-now)>0){
    if(testLedMode==1){red=true;green=false;}
    if(testLedMode==2){red=false;green=true;}
    if(testLedMode==3){red=false;green=false;}
  }
  if((int32_t)(testBuzzerUntil-now)>0 && !(alarmLatched && alarmSilenced)) buzz=true;
  // A locally silenced alarm stays silent even if another test-beep is queued.
  if(alarmLatched && alarmSilenced) buzz=false;
  setLEDs(green,red);setBuzzer(buzz);
}
void setup(){
  Serial.begin(115200);delay(750);
  pinMode(PIN_REED,REED_HAS_PCB_RC?INPUT:INPUT_PULLUP);
  pinMode(PIN_PIR,INPUT_PULLDOWN);
  pinMode(PIN_BUTTON,INPUT_PULLUP);
  pinMode(PIN_SW_REED,INPUT_PULLUP);pinMode(PIN_SW_PIR,INPUT_PULLUP);
  dipReedRaw=dipReedStable=(!DIP_SWITCHES_WIRED || digitalRead(PIN_SW_REED)==LOW);
  dipPirRaw=dipPirStable=(!DIP_SWITCHES_WIRED || digitalRead(PIN_SW_PIR)==LOW);
  dipReedChangedMs=dipPirChangedMs=millis();
  digitalWrite(PIN_GREEN,HIGH);pinMode(PIN_GREEN,OUTPUT);
  digitalWrite(PIN_RED,HIGH);pinMode(PIN_RED,OUTPUT);
  digitalWrite(PIN_BUZZER,LOW);pinMode(PIN_BUZZER,OUTPUT);
  if(!ledcAttach(PIN_BUZZER,2000,8)) Serial.println("ERROR: buzzer PWM attach failed");
  reedRaw=digitalRead(PIN_REED);reedStable=reedRaw;
  reedClosedSinceMs=millis();
  pirRaw=digitalRead(PIN_PIR);
  pirClearSinceMs=millis();
  if(pirRaw)pirHighSince=millis();
  buttonRaw=(digitalRead(PIN_BUTTON)==LOW);buttonStable=buttonRaw;
  buttonPressedSince=millis();
  reedChangedMs=buttonChangedMs=millis();
  setLEDs(true,false);
  Serial.println("\n=== SECURESENSE ESP32-C3 NODE ID 2 ===");
  Serial.printf("GPIO: reed=0 pir=3 green=2 red=8 button=9 buzzer=10 | start channel=%u (sweeps %u-%u to find hub)\n",
    SECURESENSE_CHANNEL,(unsigned)CHANNEL_MIN,(unsigned)CHANNEL_MAX);
  Serial.println("State DISARMED. No alarm will run until S3 sends ARM.");
  Serial.println("GPIO9 LOCAL BUTTON: hold 3s during ALARM to SILENCE (not DISARM).");
  Serial.printf("RESET_REARM: Reed closed >=%lu ms, PIR clear >=%lu ms (if selected).\n",
    (unsigned long)REARM_REED_CLOSED_MS,(unsigned long)REARM_PIR_CLEAR_MS);
  Serial.println("PROTOCOL v2: both C3 and S3 must be reflashed; command result is explicit.");
  Serial.println("DIP: GPIO6=REED, GPIO7=PIR; ON=connected to GND; both OFF cannot ARM.");
  printSelection();
  incoming=xQueueCreate(12,sizeof(Inbound));
  if(!incoming){Serial.println("ERROR: radio RX queue allocation failed");return;}
  Serial.println("[RADIO] Starting Wi-Fi STA for ESP-NOW...");
  if(!WiFi.mode(WIFI_STA)){Serial.println("[FATAL] WiFi.mode(WIFI_STA) failed");return;}
  uint32_t start=millis();
  while(!WiFi.STA.started() && (uint32_t)(millis()-start)<4000) delay(20);
  if(!WiFi.STA.started()){Serial.println("[FATAL] Wi-Fi STA did not start");return;}
  // RF fix verified on the user's C3 off-breadboard: default TX power gave
  // unicast MAC-layer FAIL; 8.5 dBm gave SUCCESS and S3 application ACKs.
  // This does not yet establish that the breadboard layout is RF-safe.
  bool txPowerOK = WiFi.setTxPower(WIFI_POWER_8_5dBm);
  Serial.printf("[RF] C3 TX power request 8.5 dBm: %s\n",
                txPowerOK ? "OK" : "FAILED");
  if(!txPowerOK){
    Serial.println("[FATAL] C3 RF power configuration failed; radio not started");
    return;
  }
  esp_err_t ps=esp_wifi_set_ps(WIFI_PS_NONE);
  if(ps!=ESP_OK) Serial.printf("[WARN] esp_wifi_set_ps: %s (%d)\n",esp_err_to_name(ps),(int)ps);
  // Arduino-ESP32 WiFi.setChannel returns 0 (ESP_OK) on SUCCESS.
  // Use the native ESP-IDF API and check the real error code instead.
  esp_err_t channelErr = esp_wifi_set_channel(SECURESENSE_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if(channelErr != ESP_OK){
    Serial.printf("[FATAL] esp_wifi_set_channel: %s (%d)\n", esp_err_to_name(channelErr), (int)channelErr);
    return;
  }
  Serial.print("[RADIO] C3 WiFi STA MAC: ");Serial.println(WiFi.macAddress());
  Serial.printf("[RADIO] Wi-Fi channel=%d\n",WiFi.channel());
  esp_err_t err=esp_now_init();
  if(err!=ESP_OK){Serial.printf("[FATAL] esp_now_init: %s (%d)\n",esp_err_to_name(err),(int)err);return;}
  err=esp_now_register_recv_cb(onRecv);
  if(err!=ESP_OK){Serial.printf("[FATAL] esp_now_register_recv_cb: %s (%d)\n",esp_err_to_name(err),(int)err);return;}
  esp_now_peer_info_t peer={};
  memcpy(peer.peer_addr,BROADCAST,6);
  peer.channel=0;peer.ifidx=WIFI_IF_STA;peer.encrypt=false; // 0 = current channel
  err=esp_now_add_peer(&peer);
  if(err!=ESP_OK){Serial.printf("[FATAL] broadcast peer: %s (%d)\n",esp_err_to_name(err),(int)err);return;}
  radioReady=true;
  currentChannel=SECURESENSE_CHANNEL;
  Serial.println("[RADIO] C3 ESP-NOW READY. Discovering S3 hub...");
  lastHello=millis()-HELLO_INTERVAL_MS;
  lastStatus=millis()-STATUS_INTERVAL_MS;
  lastScanStepMs=millis()-CHANNEL_SCAN_DWELL_MS;
}
void loop(){
  scanInputs();
  if(incoming)handleRadio();
  updateOutputs();
  if(!radioReady){delay(100);return;} // allow local sensors/outputs, but do not flood ESP-NOW errors
  uint32_t now=millis();
  const bool hubLive=hubLinkLive();
  if(hubLive!=hubWasLive){
    hubWasLive=hubLive;
    if(hubLive)Serial.printf("[RADIO] HUB link live on channel %u\n",(unsigned)currentChannel);
    else Serial.println("[RADIO] HUB link lost; local alarm continues. Re-scanning channels.");
  }
  if(!hubLive){
    // Discovery: HELLO on this channel, wait one dwell for the hub's ACK, then hop.
    // The alarm logic above runs every loop regardless of radio state.
    if((uint32_t)(now-lastScanStepMs)>=CHANNEL_SCAN_DWELL_MS){
      lastScanStepMs=now;
      if(announcedOnCurrentChannel)hopChannel();
      announce(false);
      announcedOnCurrentChannel=true;
      lastHello=now;
    }
  } else {
    announcedOnCurrentChannel=false;
    if((uint32_t)(now-lastHello)>=HELLO_INTERVAL_MS){lastHello=now;announce();}
  }
  if((uint32_t)(now-lastStatus)>=STATUS_INTERVAL_MS){lastStatus=now;publish("periodic");}
  delay(5);
}

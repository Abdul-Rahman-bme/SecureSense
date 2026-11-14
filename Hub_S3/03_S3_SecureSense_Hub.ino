/*
 SecureSense ESP32-S3 serial main hub for C3 prototype.
 Arduino-ESP32 ESP32S3 Dev Module, board package 3.x.
 Works over ESP-NOW channel 4, WITHOUT router, MQTT or cloud.
 Node auto-discovery: listen for C3 HELLO / STATUS; pair with source MAC.
 LAB PROTOTYPE ONLY: unencrypted ESP-NOW is not an authenticated security link.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "SecureSenseProtocol.h"
#include "esp_arduino_version.h"

#if !defined(ESP_ARDUINO_VERSION_MAJOR) || ESP_ARDUINO_VERSION_MAJOR < 3
#error Use Espressif Arduino-ESP32 boards package version 3.x
#endif

struct Inbound {SecureSensePacket packet;uint8_t mac[6];};
QueueHandle_t incoming=nullptr;
uint8_t nodeMac[6]={0};
bool nodeKnown=false;
bool radioReady=false;
uint32_t lastNodeSeen=0,hubSequence=0;
uint32_t lastAckMs=0, lastTimeoutWarnMs=0;
uint32_t packetsReceived=0;
uint32_t lastHubHeartbeatMs=0;
String inputLine;
// One outstanding command at a time. The C3 returns a correlated result,
// not merely an ACK proving reception. Do not claim success on enqueue.
bool commandPending=false;
uint8_t pendingCommand=CMD_NONE;
uint32_t pendingSequence=0, pendingSentMs=0;
constexpr uint32_t COMMAND_RESULT_TIMEOUT_MS=4000;

void printMac(const uint8_t *m){
  for(int i=0;i<6;i++){if(i)Serial.print(':');if(m[i]<16)Serial.print('0');Serial.print(m[i],HEX);}
}
void onRecv(const esp_now_recv_info_t *info,const uint8_t *data,int len){
  if(!incoming || !info || !info->src_addr || len!=(int)sizeof(SecureSensePacket)) return;
  Inbound item={};
  memcpy(&item.packet,data,sizeof(item.packet));
  memcpy(item.mac,info->src_addr,6);
  (void)xQueueSend(incoming,&item,0); // Wi-Fi callback: do NOT print here
}
bool addNodePeer(const uint8_t *mac){
  if(esp_now_is_peer_exist(mac))return true;
  esp_now_peer_info_t peer={};
  memcpy(peer.peer_addr,mac,6);
  peer.channel=SECURESENSE_CHANNEL;peer.encrypt=false;peer.ifidx=WIFI_IF_STA;
  esp_err_t e=esp_now_add_peer(&peer);
  if(e!=ESP_OK) Serial.printf("Cannot register node peer: %d\n",(int)e);
  return e==ESP_OK;
}
bool send(uint8_t type,uint8_t command=CMD_NONE,uint32_t *issuedSequence=nullptr){
  if(!radioReady){Serial.println("ESP-NOW not ready on S3; inspect startup [FATAL] log");return false;}
  if(!nodeKnown){Serial.println("No C3 discovered yet. Start C3 and verify both are on channel 4.");return false;}
  SecureSensePacket p=makePacket(type,++hubSequence);
  p.command=command;p.uptimeMs=millis();
  esp_err_t e=esp_now_send(nodeMac,(const uint8_t*)&p,sizeof(p));
  if(e!=ESP_OK){Serial.printf("ESP-NOW enqueue error %d\n",(int)e);return false;}
  if(issuedSequence)*issuedSequence=p.sequence;
  return true; // merely queued; only matching C3 result confirms execution
}
void sendCommand(uint8_t cmd,const char *label){
  if(commandPending){
    Serial.println("[HUB] Prior command still awaiting a result; wait or retry after timeout.");
    return;
  }
  if((uint32_t)(millis()-lastNodeSeen)>8000){
    Serial.println("[HUB] C3 status stale >8s; refusing command until live telemetry returns.");
    return;
  }
  uint32_t seq=0;
  if(send(PK_COMMAND,cmd,&seq)){
    commandPending=true;pendingCommand=cmd;
    pendingSequence=seq;pendingSentMs=millis();
    Serial.printf("[HUB TX] %s queued; request_seq=%lu; WAIT for C3 result.\n",
      label,(unsigned long)seq);
  }
}
void help(){
  Serial.println("\nHUB SERIAL COMMANDS (115200, Newline):");
  Serial.println("  help      - show commands");
  Serial.println("  status    - request status from C3");
  Serial.println("  ping      - verify two-way radio link");
  Serial.println("  red       - test C3 red LED for 3 seconds");
  Serial.println("  green     - test C3 green LED for 3 seconds");
  Serial.println("  off       - test both LEDs off for 3 seconds");
  Serial.println("  beep      - test C3 buzzer for 300 ms");
  Serial.println("  arm       - C3 enters ARMED only when Reed closed & PIR ready");
  Serial.println("  disarm    - turn off C3 alarm and restore green LED");
  Serial.println("  silence   - silence alarm sound; alarm remains latched");
  Serial.println("  rearm     - safely RESET_REARM a latched alarm, stay ARMED (C3 checks sensors)");
  Serial.println("  reset_rearm - alias for rearm");
  Serial.println("  who       - display discovered C3 MAC and link age\n");
}
void command(String s){
  s.trim();s.toLowerCase();
  if(s=="help"||s=="?")help();
  else if(s=="status")sendCommand(CMD_GET_STATUS,"STATUS");
  else if(s=="ping")sendCommand(CMD_PING,"PING");
  else if(s=="red")sendCommand(CMD_RED,"RED");
  else if(s=="green")sendCommand(CMD_GREEN,"GREEN");
  else if(s=="off")sendCommand(CMD_LEDS_OFF,"LEDS OFF");
  else if(s=="beep")sendCommand(CMD_BEEP,"BEEP");
  else if(s=="arm")sendCommand(CMD_ARM,"ARM");
  else if(s=="disarm")sendCommand(CMD_DISARM,"DISARM");
  else if(s=="silence")sendCommand(CMD_SILENCE,"SILENCE");
  else if(s=="rearm"||s=="reset_rearm")sendCommand(CMD_RESET_REARM,"RESET_REARM");
  else if(s=="who"){
    if(nodeKnown){Serial.print("C3 MAC: ");printMac(nodeMac);Serial.printf(" | last seen %lu ms ago | packets=%lu\n",(unsigned long)(millis()-lastNodeSeen),(unsigned long)packetsReceived);}
    else Serial.println("No C3 discovered yet");
  }
  else if(s.length())Serial.println("Unknown command; type help");
}
void printStatus(const SecureSensePacket &p){
  Serial.printf("[C3 +%lu ms seq=%lu] REED GPIO0 raw=%u CLOSED=%u | PIR GPIO3 raw=%u READY=%u | BTN=%u | SW Reed=%u PIR=%u | ARMED=%u ALARM=%u SILENCED=%u CAUSE=%s | LED G=%u R=%u BUZZ=%u\n",
    (unsigned long)p.uptimeMs,(unsigned long)p.sequence,
    p.reedRaw,p.reedClosed,p.pirRaw,p.pirReady,p.buttonPressed,
    p.reedEnabled,p.pirEnabled,p.armed,p.alarm,p.alarmSilenced,
    alarmCauseName(p.alarmCause),p.greenOn,p.redOn,p.buzzerOn);
}
void processRadio(){
  Inbound in;
  while(xQueueReceive(incoming,&in,0)==pdTRUE){
    SecureSensePacket &p=in.packet;
    if(!validPacket(p))continue;
    if(p.type!=PK_HELLO && p.type!=PK_STATUS && p.type!=PK_ACK)continue;
    if(nodeKnown && memcmp(in.mac,nodeMac,6)!=0){
      Serial.println("[HUB] Another device claims same node ID 2; ignored");
      continue;
    }
    if(!nodeKnown){
      if(!addNodePeer(in.mac))continue;
      memcpy(nodeMac,in.mac,6);nodeKnown=true;
      Serial.print("\n*** C3 NODE 2 DISCOVERED: ");printMac(nodeMac);
      Serial.println(" ***\n");
    }
    lastNodeSeen=millis();packetsReceived++;
    if(p.type==PK_HELLO){
      Serial.printf("[C3 HELLO] seq=%lu; reply hub ACK\n",(unsigned long)p.sequence);
      if(send(PK_ACK))lastAckMs=millis();
    } else if(p.type==PK_STATUS){
      printStatus(p);
      if((uint32_t)(millis()-lastAckMs)>2500){
        if(send(PK_ACK))lastAckMs=millis();
      }
    } else if(p.type==PK_ACK){
      // Hub's outgoing HELLO/status ACKs use CMD_NONE. Node command-result
      // ACKs include a request sequence so the UI can match an action.
      if(p.command!=CMD_NONE && p.responseToSequence!=0){
        if(commandPending && p.responseToSequence==pendingSequence &&
           p.command==pendingCommand){
          Serial.printf("[HUB RESULT] request=%lu cmd=%u: %s | C3 armed=%u alarm=%u silenced=%u cause=%s\n",
            (unsigned long)p.responseToSequence,p.command,commandResultName(p.commandResult),
            p.armed,p.alarm,p.alarmSilenced,alarmCauseName(p.alarmCause));
          commandPending=false;
          // The next STATUS confirms the state reported in this ACK.
        } else {
          Serial.printf("[HUB] Unmatched/late result request=%lu cmd=%u result=%s (not used to confirm pending action)\n",
            (unsigned long)p.responseToSequence,p.command,commandResultName(p.commandResult));
        }
      }
    }
  }
}
void setup(){
  Serial.begin(115200);delay(900);
  Serial.println("\n=== SECURESENSE MAIN HUB: ESP32-S3 ===");
  Serial.printf("[HUB] Reset reason code=%d (use to diagnose unexpected reboots)\n",(int)esp_reset_reason());
  Serial.printf("Node expected: ID %u | ESP-NOW channel %u | protocol v%u\n",
    SECURESENSE_NODE_ID,SECURESENSE_CHANNEL,SECURESENSE_VERSION);
  incoming=xQueueCreate(16,sizeof(Inbound));
  if(!incoming){Serial.println("ERROR: RX queue allocation failed");return;}
  Serial.println("[RADIO] Starting Wi-Fi STA for ESP-NOW...");
  if(!WiFi.mode(WIFI_STA)){Serial.println("[FATAL] WiFi.mode(WIFI_STA) failed");return;}
  uint32_t start=millis();
  while(!WiFi.STA.started() && (uint32_t)(millis()-start)<4000) delay(20);
  if(!WiFi.STA.started()){Serial.println("[FATAL] Wi-Fi STA did not start");return;}
  esp_err_t ps=esp_wifi_set_ps(WIFI_PS_NONE);
  if(ps!=ESP_OK) Serial.printf("[WARN] esp_wifi_set_ps: %s (%d)\n",esp_err_to_name(ps),(int)ps);
  // Arduino-ESP32 WiFi.setChannel returns 0 (ESP_OK) on SUCCESS.
  // Use the native ESP-IDF API and check the real error code instead.
  esp_err_t channelErr = esp_wifi_set_channel(SECURESENSE_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if(channelErr != ESP_OK){
    Serial.printf("[FATAL] esp_wifi_set_channel: %s (%d)\n", esp_err_to_name(channelErr), (int)channelErr);
    return;
  }
  Serial.print("[RADIO] S3 WiFi STA MAC: ");Serial.println(WiFi.macAddress());
  Serial.printf("[RADIO] Wi-Fi channel=%d\n",WiFi.channel());
  esp_err_t err=esp_now_init();
  if(err!=ESP_OK){Serial.printf("[FATAL] esp_now_init: %s (%d)\n",esp_err_to_name(err),(int)err);return;}
  err=esp_now_register_recv_cb(onRecv);
  if(err!=ESP_OK){Serial.printf("[FATAL] esp_now_register_recv_cb: %s (%d)\n",esp_err_to_name(err),(int)err);return;}
  radioReady=true;
  Serial.println("[RADIO] S3 HUB READY. Power C3 node and look for C3 HELLO (5s interval).");
  help();
}
void loop(){
  if(incoming)processRadio();
  while(Serial.available()){
    char c=(char)Serial.read();
    if(c=='\r'||c=='\n'){
      if(inputLine.length()){command(inputLine);inputLine="";}
    } else if(inputLine.length()<64)inputLine+=c;
  }
  if(commandPending && (uint32_t)(millis()-pendingSentMs)>COMMAND_RESULT_TIMEOUT_MS){
    Serial.printf("[HUB TIMEOUT] No C3 result for request=%lu cmd=%u. Outcome UNKNOWN; check STATUS before retrying.\n",
      (unsigned long)pendingSequence,(unsigned)pendingCommand);
    commandPending=false;
  }
  if(nodeKnown && (uint32_t)(millis()-lastNodeSeen)>8000 &&
     (uint32_t)(millis()-lastTimeoutWarnMs)>8000){
    Serial.println("[HUB] C3 telemetry timeout >8s. Check node power, RF channel, or reboot.");
    lastTimeoutWarnMs=millis();
  }
  if((uint32_t)(millis()-lastHubHeartbeatMs)>=10000){
    lastHubHeartbeatMs=millis();
    Serial.printf("[HUB HEARTBEAT] uptime=%lu ms | node=%s | received=%lu\n",
                  (unsigned long)millis(), nodeKnown ? "DISCOVERED" : "WAITING",
                  (unsigned long)packetsReceived);
  }
  delay(10);
}

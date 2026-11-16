/*
 SecureSense ESP32-S3 main hub — Stage 8: Wi-Fi + MQTT bridge.
 Arduino-ESP32 ESP32S3 Dev Module, board package 3.x.

 Radio:  C3 node <-> S3 hub over ESP-NOW (unchanged packet format, protocol v2).
 Uplink: S3 joins your 2.4 GHz router and talks MQTT (TLS) to a broker using the
         ESP-IDF esp-mqtt client that ships with Arduino-ESP32 3.x (no extra library).

 CHANNEL: ESP-NOW and Wi-Fi share one radio. Once the S3 joins the router it MUST
 use the router's channel. The Stage 8 C3 sketch scans channels 1-13 to find the
 hub, so the router may use any channel. If WIFI_SSID is empty the hub runs in the
 old offline mode on fixed channel SECURESENSE_CHANNEL (4) with no MQTT.

 MQTT topics (prefix = MQTT_TOPIC_PREFIX from secrets.h, e.g. securesense/hub1):
   <prefix>/hub/online          retained  "online" / "offline" (broker Last Will)
   <prefix>/hub/status          retained  JSON, every 30 s
   <prefix>/node/2/online       retained  "online" / "offline" (C3 telemetry <= 8 s old)
   <prefix>/node/2/status       retained  JSON, on meaningful change + every 15 s
   <prefix>/node/2/event        JSON: ARMED, DISARMED, ALARM, ALARM_RESET_REARMED,
                                SILENCED, NODE_REBOOTED
   <prefix>/node/2/cmd          SUBSCRIBED: {"id":"<1-40 chars A-Z a-z 0-9 _ ->","cmd":"arm"}
                                cmd = ping | status | arm | disarm | silence | rearm
   <prefix>/node/2/cmd_result   JSON with the same id; exactly one final result per
                                accepted id (C3 result, hub rejection or TIMEOUT_UNKNOWN)

 LAB PROTOTYPE ONLY: the ESP-NOW hop is still unencrypted/unauthenticated. MQTT access
 must be restricted with broker credentials + ACLs; never let browsers publish to /cmd
 directly — route commands through an authenticated backend.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <time.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mqtt_client.h"
#include "SecureSenseProtocol.h"
#include "esp_arduino_version.h"

#if !defined(ESP_ARDUINO_VERSION_MAJOR) || ESP_ARDUINO_VERSION_MAJOR < 3
#error Use Espressif Arduino-ESP32 boards package version 3.x
#endif

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy secrets.example.h to secrets.h in this sketch folder and fill in your Wi-Fi/MQTT details (secrets.h is git-ignored)."
#endif

#define JBOOL(x) ((x) ? "true" : "false")

// ---------------------------------------------------------------- ESP-NOW state
struct Inbound {SecureSensePacket packet;uint8_t mac[6];int8_t rssi;};
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
enum CommandSource : uint8_t { SRC_SERIAL=0, SRC_MQTT=1 };
bool commandPending=false;
uint8_t pendingCommand=CMD_NONE;
uint32_t pendingSequence=0, pendingSentMs=0;
CommandSource pendingSource=SRC_SERIAL;
char pendingRequestId[41]="";
constexpr uint32_t COMMAND_RESULT_TIMEOUT_MS=4000;
constexpr uint32_t NODE_STALE_MS=8000;

// Latest node state, used for MQTT status, change detection and events.
SecureSensePacket lastNodeState={};
bool haveNodeState=false;
int8_t lastNodeRssi=0;

// ---------------------------------------------------------------- Wi-Fi / MQTT state
bool wifiEnabled=false;      // false => offline mode (fixed channel, no MQTT)
bool wifiUp=false;
bool ntpStarted=false;
esp_mqtt_client_handle_t mqtt=nullptr;
bool mqttDisabled=false;
volatile bool mqttConnected=false;     // written by the esp-mqtt task
volatile bool mqttConnectedEdge=false; // loop() republishes retained state
volatile bool mqttErrorPending=false;
volatile int mqttLastErrorType=0, mqttLastTlsErr=0, mqttLastConnRefused=0;
bool mqttWasConnected=false;
bool nodeOnlinePublished=false, nodeOnlineKnown=false;
uint32_t lastNodeStatusPublishMs=0, lastHubStatusPublishMs=0;
char lastMqttRequestId[41]="";
constexpr uint32_t NODE_STATUS_HEARTBEAT_MS=15000;
constexpr uint32_t HUB_STATUS_INTERVAL_MS=30000;

struct MqttCmd { char payload[161]; };
QueueHandle_t mqttCmdQueue=nullptr;
char mqttClientId[40];
char topicHubOnline[96], topicHubStatus[96], topicNodeOnline[96], topicNodeStatus[96];
char topicNodeEvent[96], topicNodeCmd[96], topicNodeCmdResult[96];

// ---------------------------------------------------------------- helpers
void printMac(const uint8_t *m){
  for(int i=0;i<6;i++){if(i)Serial.print(':');if(m[i]<16)Serial.print('0');Serial.print(m[i],HEX);}
}
void macToString(const uint8_t *m,char *out,size_t n){
  snprintf(out,n,"%02X:%02X:%02X:%02X:%02X:%02X",m[0],m[1],m[2],m[3],m[4],m[5]);
}
// Unix seconds once NTP has synced, otherwise JSON null. Backend should still
// stamp messages on receipt; this is only a hint for ordering.
void tsJson(char *out,size_t n){
  time_t t=time(nullptr);
  if(t>1700000000) snprintf(out,n,"%lld",(long long)t); else snprintf(out,n,"null");
}
bool nodeLive(){return nodeKnown && (uint32_t)(millis()-lastNodeSeen)<=NODE_STALE_MS;}

const char* commandApiName(uint8_t c){
  switch(c){
    case CMD_PING:return "ping"; case CMD_GET_STATUS:return "status";
    case CMD_ARM:return "arm"; case CMD_DISARM:return "disarm";
    case CMD_SILENCE:return "silence"; case CMD_RESET_REARM:return "rearm";
    case CMD_RED:return "red"; case CMD_GREEN:return "green";
    case CMD_LEDS_OFF:return "off"; case CMD_BEEP:return "beep";
    default:return "none";
  }
}
// Remote (MQTT) commands are limited to the security-relevant set plus link checks.
// LED/buzzer test commands stay Serial-only.
uint8_t mqttCommandCode(const char *s){
  if(!strcmp(s,"ping"))return CMD_PING;
  if(!strcmp(s,"status"))return CMD_GET_STATUS;
  if(!strcmp(s,"arm"))return CMD_ARM;
  if(!strcmp(s,"disarm"))return CMD_DISARM;
  if(!strcmp(s,"silence"))return CMD_SILENCE;
  if(!strcmp(s,"rearm")||!strcmp(s,"reset_rearm"))return CMD_RESET_REARM;
  return CMD_NONE;
}

// Minimal extractor for a flat JSON string field: "key" : "value".
// Rejects escapes and over-long values. Enough for our own backend's messages.
bool jsonStringField(const char *json,const char *key,char *out,size_t n){
  char pat[24]; snprintf(pat,sizeof(pat),"\"%s\"",key);
  const size_t patLen=strlen(pat);
  for(const char *k=strstr(json,pat);k;k=strstr(k+1,pat)){
    const char *c=k+patLen;
    while(isspace((unsigned char)*c))c++;
    if(*c!=':')continue;           // matched a value, not a key; keep looking
    c++; while(isspace((unsigned char)*c))c++;
    if(*c!='"')return false;
    c++;
    size_t i=0;
    while(*c && *c!='"'){
      if(*c=='\\' || i+1>=n)return false;
      out[i++]=*c++;
    }
    if(*c!='"')return false;
    out[i]=0;
    return i>0;
  }
  return false;
}
bool validRequestId(const char *s){
  size_t n=strlen(s);
  if(n<1||n>40)return false;
  for(size_t i=0;i<n;i++){
    char c=s[i];
    if(!(isalnum((unsigned char)c)||c=='-'||c=='_'))return false;
  }
  return true;
}

// ---------------------------------------------------------------- MQTT
bool mqttPublish(const char *topic,const char *payload,int qos,bool retain){
  if(!mqtt||!mqttConnected)return false;
  // enqueue = non-blocking; the esp-mqtt task does the network write.
  int id=esp_mqtt_client_enqueue(mqtt,topic,payload,0,qos,retain?1:0,true);
  return id>=0;
}

void publishNodeStatus(){
  if(!haveNodeState)return;
  const SecureSensePacket &p=lastNodeState;
  char ts[24]; tsJson(ts,sizeof(ts));
  char buf[512];
  snprintf(buf,sizeof(buf),
    "{\"node\":%u,\"seq\":%lu,\"uptimeMs\":%lu,\"armed\":%s,\"alarm\":%s,\"silenced\":%s,"
    "\"cause\":\"%s\",\"reedEnabled\":%s,\"reedClosed\":%s,\"pirEnabled\":%s,\"pirReady\":%s,"
    "\"pirMotion\":%s,\"button\":%s,\"redLed\":%s,\"greenLed\":%s,\"buzzer\":%s,\"rssi\":%d,\"ts\":%s}",
    (unsigned)SECURESENSE_NODE_ID,(unsigned long)p.sequence,(unsigned long)p.uptimeMs,
    JBOOL(p.armed),JBOOL(p.alarm),JBOOL(p.alarmSilenced),alarmCauseName(p.alarmCause),
    JBOOL(p.reedEnabled),JBOOL(p.reedClosed),JBOOL(p.pirEnabled),JBOOL(p.pirReady),
    JBOOL(p.pirRaw),JBOOL(p.buttonPressed),JBOOL(p.redOn),JBOOL(p.greenOn),JBOOL(p.buzzerOn),
    (int)lastNodeRssi,ts);
  if(mqttPublish(topicNodeStatus,buf,1,true))lastNodeStatusPublishMs=millis();
}

void publishHubStatus(){
  char ts[24]; tsJson(ts,sizeof(ts));
  char mac[18]="";
  if(nodeKnown)macToString(nodeMac,mac,sizeof(mac));
  char buf[320];
  snprintf(buf,sizeof(buf),
    "{\"uptimeMs\":%lu,\"protocol\":%u,\"wifiRssi\":%d,\"channel\":%d,\"nodeKnown\":%s,"
    "\"nodeMac\":\"%s\",\"nodeLinkAgeMs\":%lu,\"packetsReceived\":%lu,\"ts\":%s}",
    (unsigned long)millis(),(unsigned)SECURESENSE_VERSION,(int)WiFi.RSSI(),(int)WiFi.channel(),
    JBOOL(nodeKnown),mac,nodeKnown?(unsigned long)(millis()-lastNodeSeen):0UL,
    (unsigned long)packetsReceived,ts);
  if(mqttPublish(topicHubStatus,buf,0,true))lastHubStatusPublishMs=millis();
}

void publishNodeOnline(bool online){
  if(mqttPublish(topicNodeOnline,online?"online":"offline",1,true)){
    nodeOnlinePublished=online;nodeOnlineKnown=true;
  }
}

void publishEvent(const char *type,const SecureSensePacket &p){
  char ts[24]; tsJson(ts,sizeof(ts));
  char buf[200];
  snprintf(buf,sizeof(buf),
    "{\"node\":%u,\"type\":\"%s\",\"cause\":\"%s\",\"seq\":%lu,\"uptimeMs\":%lu,\"ts\":%s}",
    (unsigned)SECURESENSE_NODE_ID,type,alarmCauseName(p.alarmCause),
    (unsigned long)p.sequence,(unsigned long)p.uptimeMs,ts);
  bool sent=mqttPublish(topicNodeEvent,buf,1,false);
  Serial.printf("[EVENT] %s cause=%s%s\n",type,alarmCauseName(p.alarmCause),
    sent?" -> MQTT":(wifiEnabled?" (MQTT offline, not delivered)":""));
}

// state==nullptr for hub-side outcomes that have no fresh C3 state.
void publishCommandResult(const char *id,uint8_t cmd,const char *result,const SecureSensePacket *state){
  char ts[24]; tsJson(ts,sizeof(ts));
  char stateJson[160]="";
  if(state){
    snprintf(stateJson,sizeof(stateJson),
      ",\"armed\":%s,\"alarm\":%s,\"silenced\":%s,\"cause\":\"%s\"",
      JBOOL(state->armed),JBOOL(state->alarm),JBOOL(state->alarmSilenced),
      alarmCauseName(state->alarmCause));
  }
  char buf[320];
  snprintf(buf,sizeof(buf),"{\"id\":\"%s\",\"cmd\":\"%s\",\"result\":\"%s\"%s,\"ts\":%s}",
    id,commandApiName(cmd),result,stateJson,ts);
  bool sent=mqttPublish(topicNodeCmdResult,buf,1,false);
  Serial.printf("[MQTT RESULT] id=%s cmd=%s result=%s%s\n",id,commandApiName(cmd),result,
    sent?"":" (MQTT offline, not delivered)");
}

// Runs in the esp-mqtt task: no Serial printing, no ESP-NOW; hand work to loop().
static void onMqttEvent(void*,esp_event_base_t,int32_t eventId,void *eventData){
  esp_mqtt_event_handle_t e=(esp_mqtt_event_handle_t)eventData;
  switch((esp_mqtt_event_id_t)eventId){
    case MQTT_EVENT_CONNECTED:
      mqttConnected=true;mqttConnectedEdge=true;
      esp_mqtt_client_subscribe(e->client,topicNodeCmd,1);
      esp_mqtt_client_publish(e->client,topicHubOnline,"online",0,1,1);
      break;
    case MQTT_EVENT_DISCONNECTED:
      mqttConnected=false;
      break;
    case MQTT_EVENT_DATA: {
      // Never act on a retained command: it would replay on every reconnect.
      if(e->retain)break;
      if(e->current_data_offset!=0 || e->data_len!=e->total_data_len)break; // fragmented
      if(e->data_len<=0 || e->data_len>=(int)sizeof(MqttCmd::payload))break;
      if(e->topic_len!=(int)strlen(topicNodeCmd) || memcmp(e->topic,topicNodeCmd,e->topic_len)!=0)break;
      MqttCmd c={};
      memcpy(c.payload,e->data,e->data_len);
      c.payload[e->data_len]=0;
      (void)xQueueSend(mqttCmdQueue,&c,0);
      break;
    }
    case MQTT_EVENT_ERROR:
      if(e->error_handle){
        mqttLastErrorType=e->error_handle->error_type;
        mqttLastTlsErr=e->error_handle->esp_tls_last_esp_err;
        mqttLastConnRefused=e->error_handle->connect_return_code;
      }
      mqttErrorPending=true;
      break;
    default: break;
  }
}

void startMqtt(){
  if(mqtt||mqttDisabled)return;
  if(!strlen(MQTT_URI)){
    Serial.println("[MQTT] MQTT_URI empty in secrets.h; MQTT disabled (Wi-Fi only).");
    mqttDisabled=true;return;
  }
  const bool tls=strncmp(MQTT_URI,"mqtts://",8)==0;
  if(tls && (MQTT_CA_CERT==nullptr || !strstr(MQTT_CA_CERT,"BEGIN CERTIFICATE"))){
    Serial.println("[MQTT] mqtts:// requires the broker's CA certificate in MQTT_CA_CERT (secrets.h); MQTT disabled.");
    mqttDisabled=true;return;
  }
  if(!tls)Serial.println("[MQTT][WARN] Plain mqtt:// (no TLS). Use ONLY on a private bench network.");

  esp_mqtt_client_config_t cfg={};
  cfg.broker.address.uri=MQTT_URI;
  if(tls)cfg.broker.verification.certificate=MQTT_CA_CERT;
  cfg.credentials.client_id=mqttClientId;
  if(strlen(MQTT_USERNAME))cfg.credentials.username=MQTT_USERNAME;
  if(strlen(MQTT_PASSWORD))cfg.credentials.authentication.password=MQTT_PASSWORD;
  cfg.session.keepalive=15;                 // broker notices a dead hub in ~22 s
  cfg.session.last_will.topic=topicHubOnline;
  cfg.session.last_will.msg="offline";
  cfg.session.last_will.msg_len=7;
  cfg.session.last_will.qos=1;
  cfg.session.last_will.retain=1;
  cfg.network.reconnect_timeout_ms=5000;
  cfg.network.timeout_ms=8000;
  cfg.buffer.size=1024;

  mqtt=esp_mqtt_client_init(&cfg);
  if(!mqtt){Serial.println("[MQTT] esp_mqtt_client_init failed");mqttDisabled=true;return;}
  esp_mqtt_client_register_event(mqtt,(esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,onMqttEvent,nullptr);
  esp_err_t e=esp_mqtt_client_start(mqtt);
  if(e!=ESP_OK){Serial.printf("[MQTT] start failed: %s\n",esp_err_to_name(e));return;}
  Serial.printf("[MQTT] Connecting to %s as %s ...\n",MQTT_URI,mqttClientId);
}

// ---------------------------------------------------------------- ESP-NOW
void onRecv(const esp_now_recv_info_t *info,const uint8_t *data,int len){
  if(!incoming || !info || !info->src_addr || len!=(int)sizeof(SecureSensePacket)) return;
  Inbound item={};
  memcpy(&item.packet,data,sizeof(item.packet));
  memcpy(item.mac,info->src_addr,6);
  item.rssi=info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
  (void)xQueueSend(incoming,&item,0); // Wi-Fi callback: do NOT print here
}
bool addNodePeer(const uint8_t *mac){
  if(esp_now_is_peer_exist(mac))return true;
  esp_now_peer_info_t peer={};
  memcpy(peer.peer_addr,mac,6);
  // channel 0 = "whatever channel the radio is on now", so the peer follows the
  // router channel once Wi-Fi connects (a fixed channel would fail with ESPNOW_CHAN).
  peer.channel=0;peer.encrypt=false;peer.ifidx=WIFI_IF_STA;
  esp_err_t e=esp_now_add_peer(&peer);
  if(e!=ESP_OK) Serial.printf("Cannot register node peer: %d\n",(int)e);
  return e==ESP_OK;
}
bool send(uint8_t type,uint8_t command=CMD_NONE,uint32_t *issuedSequence=nullptr){
  if(!radioReady){Serial.println("ESP-NOW not ready on S3; inspect startup [FATAL] log");return false;}
  if(!nodeKnown){Serial.println("No C3 discovered yet. Start the C3 and wait for it to find the hub channel.");return false;}
  SecureSensePacket p=makePacket(type,++hubSequence);
  p.command=command;p.uptimeMs=millis();
  esp_err_t e=esp_now_send(nodeMac,(const uint8_t*)&p,sizeof(p));
  if(e!=ESP_OK){Serial.printf("ESP-NOW enqueue error %d\n",(int)e);return false;}
  if(issuedSequence)*issuedSequence=p.sequence;
  return true; // merely queued; only matching C3 result confirms execution
}

// Returns nullptr when queued, else a hub-side rejection reason.
const char* sendCommand(uint8_t cmd,const char *label,CommandSource src,const char *requestId){
  const char *reject=nullptr;
  if(commandPending){
    Serial.println("[HUB] Prior command still awaiting a result; wait or retry after timeout.");
    reject="HUB_BUSY";
  } else if(!radioReady){
    Serial.println("[HUB] ESP-NOW not ready.");
    reject="HUB_RADIO_DOWN";
  } else if(!nodeLive()){
    Serial.println("[HUB] C3 status stale >8s; refusing command until live telemetry returns.");
    reject="NODE_OFFLINE";
  } else {
    uint32_t seq=0;
    if(send(PK_COMMAND,cmd,&seq)){
      commandPending=true;pendingCommand=cmd;
      pendingSequence=seq;pendingSentMs=millis();
      pendingSource=src;
      snprintf(pendingRequestId,sizeof(pendingRequestId),"%s",requestId?requestId:"");
      Serial.printf("[HUB TX] %s queued; request_seq=%lu; source=%s; WAIT for C3 result.\n",
        label,(unsigned long)seq,src==SRC_MQTT?"MQTT":"Serial");
    } else reject="SEND_FAILED";
  }
  if(reject && src==SRC_MQTT)publishCommandResult(requestId,cmd,reject,haveNodeState?&lastNodeState:nullptr);
  return reject;
}

// ---------------------------------------------------------------- Serial UI
void printNetStatus(){
  if(!wifiEnabled){Serial.printf("NET: offline mode (WIFI_SSID empty) | ESP-NOW channel %u\n",SECURESENSE_CHANNEL);return;}
  Serial.printf("NET: Wi-Fi %s",wifiUp?"CONNECTED":"DISCONNECTED");
  if(wifiUp){Serial.print(" ip=");Serial.print(WiFi.localIP());Serial.printf(" rssi=%d dBm",(int)WiFi.RSSI());}
  Serial.printf(" | radio channel=%d | MQTT %s | NTP %s\n",(int)WiFi.channel(),
    mqttDisabled?"DISABLED":(mqttConnected?"CONNECTED":"not connected"),
    time(nullptr)>1700000000?"synced":"not synced");
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
  Serial.println("  who       - display discovered C3 MAC and link age");
  Serial.println("  net       - Wi-Fi / MQTT / channel status\n");
}
void command(String s){
  s.trim();s.toLowerCase();
  if(s=="help"||s=="?")help();
  else if(s=="status")sendCommand(CMD_GET_STATUS,"STATUS",SRC_SERIAL,nullptr);
  else if(s=="ping")sendCommand(CMD_PING,"PING",SRC_SERIAL,nullptr);
  else if(s=="red")sendCommand(CMD_RED,"RED",SRC_SERIAL,nullptr);
  else if(s=="green")sendCommand(CMD_GREEN,"GREEN",SRC_SERIAL,nullptr);
  else if(s=="off")sendCommand(CMD_LEDS_OFF,"LEDS OFF",SRC_SERIAL,nullptr);
  else if(s=="beep")sendCommand(CMD_BEEP,"BEEP",SRC_SERIAL,nullptr);
  else if(s=="arm")sendCommand(CMD_ARM,"ARM",SRC_SERIAL,nullptr);
  else if(s=="disarm")sendCommand(CMD_DISARM,"DISARM",SRC_SERIAL,nullptr);
  else if(s=="silence")sendCommand(CMD_SILENCE,"SILENCE",SRC_SERIAL,nullptr);
  else if(s=="rearm"||s=="reset_rearm")sendCommand(CMD_RESET_REARM,"RESET_REARM",SRC_SERIAL,nullptr);
  else if(s=="net")printNetStatus();
  else if(s=="who"){
    if(nodeKnown){Serial.print("C3 MAC: ");printMac(nodeMac);Serial.printf(" | last seen %lu ms ago | packets=%lu | rssi=%d dBm\n",(unsigned long)(millis()-lastNodeSeen),(unsigned long)packetsReceived,(int)lastNodeRssi);}
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

// ---------------------------------------------------------------- node state tracking
// Only these fields trigger an immediate MQTT status publish. LEDs/buzzer blink at
// 300 ms during an alarm and would otherwise cause a publish every second.
bool significantChange(const SecureSensePacket &a,const SecureSensePacket &b){
  return a.armed!=b.armed||a.alarm!=b.alarm||a.alarmSilenced!=b.alarmSilenced||
         a.alarmCause!=b.alarmCause||a.reedClosed!=b.reedClosed||a.reedEnabled!=b.reedEnabled||
         a.pirEnabled!=b.pirEnabled||a.pirReady!=b.pirReady||a.pirRaw!=b.pirRaw||
         a.buttonPressed!=b.buttonPressed;
}
// Every C3 packet (HELLO, STATUS, command ACK) carries the full node state.
void observeNodeState(const SecureSensePacket &p,int8_t rssi){
  lastNodeRssi=rssi;
  if(!haveNodeState){
    lastNodeState=p;haveNodeState=true;
    publishNodeStatus();
    return;
  }
  const SecureSensePacket prev=lastNodeState;
  const bool rebooted=(uint32_t)p.uptimeMs+2000<(uint32_t)prev.uptimeMs;
  if(!rebooted && p.sequence<=prev.sequence)return; // late/duplicate packet
  lastNodeState=p;
  if(rebooted){
    // A C3 reboot always comes back DISARMED. Flag it rather than emit a
    // misleading DISARMED event.
    publishEvent("NODE_REBOOTED",p);
  } else {
    if(!prev.armed && p.armed)publishEvent("ARMED",p);
    if(prev.armed && !p.armed)publishEvent("DISARMED",p);
    if(!prev.alarm && p.alarm)publishEvent("ALARM",p);
    if(prev.alarm && !p.alarm && p.armed)publishEvent("ALARM_RESET_REARMED",p);
    if(!prev.alarmSilenced && p.alarmSilenced)publishEvent("SILENCED",p);
  }
  if(rebooted || significantChange(prev,p))publishNodeStatus();
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
      Serial.printf(" on channel %d ***\n\n",(int)WiFi.channel());
    }
    lastNodeSeen=millis();packetsReceived++;
    observeNodeState(p,in.rssi);
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
          if(pendingSource==SRC_MQTT)
            publishCommandResult(pendingRequestId,p.command,commandResultName(p.commandResult),&p);
          // The next STATUS confirms the state reported in this ACK.
        } else {
          Serial.printf("[HUB] Unmatched/late result request=%lu cmd=%u result=%s (not used to confirm pending action)\n",
            (unsigned long)p.responseToSequence,p.command,commandResultName(p.commandResult));
        }
      }
    }
  }
}

void processMqttCommands(){
  if(!mqttCmdQueue)return;
  MqttCmd m;
  while(xQueueReceive(mqttCmdQueue,&m,0)==pdTRUE){
    char id[41]={0},cmdText[16]={0};
    if(!jsonStringField(m.payload,"id",id,sizeof(id)) || !validRequestId(id)){
      Serial.println("[MQTT CMD] Ignored: missing/invalid \"id\" (1-40 chars A-Z a-z 0-9 _ -)");
      continue; // cannot correlate a reply without a valid id
    }
    if(!jsonStringField(m.payload,"cmd",cmdText,sizeof(cmdText))){
      publishCommandResult(id,CMD_NONE,"INVALID_REQUEST",nullptr);continue;
    }
    for(char *c=cmdText;*c;c++)*c=(char)tolower((unsigned char)*c);
    // QoS 1 may redeliver the same message; never execute one id twice in a row.
    if(!strcmp(id,lastMqttRequestId)){
      Serial.printf("[MQTT CMD] Duplicate id=%s ignored\n",id);continue;
    }
    snprintf(lastMqttRequestId,sizeof(lastMqttRequestId),"%s",id);
    uint8_t cmd=mqttCommandCode(cmdText);
    if(cmd==CMD_NONE){publishCommandResult(id,CMD_NONE,"UNSUPPORTED_COMMAND",nullptr);continue;}
    Serial.printf("[MQTT CMD] id=%s cmd=%s\n",id,cmdText);
    sendCommand(cmd,commandApiName(cmd),SRC_MQTT,id);
  }
}

// ---------------------------------------------------------------- Wi-Fi service
void serviceNetwork(){
  if(!wifiEnabled)return;
  const bool up=WiFi.isConnected();
  if(up!=wifiUp){
    wifiUp=up;
    if(up){
      Serial.print("[WIFI] Connected ip=");Serial.print(WiFi.localIP());
      Serial.printf(" rssi=%d dBm channel=%d\n",(int)WiFi.RSSI(),(int)WiFi.channel());
      if((int)WiFi.channel()!=SECURESENSE_CHANNEL)
        Serial.printf("[RADIO] ESP-NOW now follows the router on channel %d; the Stage 8 C3 will scan to find it.\n",(int)WiFi.channel());
      if(!ntpStarted){configTime(0,0,"pool.ntp.org","time.google.com");ntpStarted=true;}
      startMqtt();
    } else {
      Serial.println("[WIFI] Disconnected; auto-reconnecting. While Wi-Fi scans, ESP-NOW to the C3 may drop out briefly (the C3 alarm keeps working locally).");
    }
  }
  const bool mc=mqttConnected;
  if(mc!=mqttWasConnected){
    mqttWasConnected=mc;
    Serial.println(mc?"[MQTT] Connected; subscribed to commands.":"[MQTT] Disconnected; esp-mqtt will retry.");
  }
  if(mqttErrorPending){
    mqttErrorPending=false;
    Serial.printf("[MQTT] Error type=%d tls_err=0x%x connect_refused=%d (TLS errors: check URI, port, CA cert, clock; refused: check username/password/ACL)\n",
      mqttLastErrorType,mqttLastTlsErr,mqttLastConnRefused);
  }
  if(mqttConnectedEdge){
    mqttConnectedEdge=false;
    // Refresh every retained topic after (re)connecting.
    publishHubStatus();
    publishNodeStatus();
    publishNodeOnline(nodeLive());
  }
  if(mqttConnected){
    const bool live=nodeLive();
    if(!nodeOnlineKnown || live!=nodeOnlinePublished)publishNodeOnline(live);
    if(haveNodeState && live && (uint32_t)(millis()-lastNodeStatusPublishMs)>=NODE_STATUS_HEARTBEAT_MS)
      publishNodeStatus();
    if((uint32_t)(millis()-lastHubStatusPublishMs)>=HUB_STATUS_INTERVAL_MS)publishHubStatus();
  }
}

void buildTopics(){
  snprintf(topicHubOnline,sizeof(topicHubOnline),"%s/hub/online",MQTT_TOPIC_PREFIX);
  snprintf(topicHubStatus,sizeof(topicHubStatus),"%s/hub/status",MQTT_TOPIC_PREFIX);
  snprintf(topicNodeOnline,sizeof(topicNodeOnline),"%s/node/%u/online",MQTT_TOPIC_PREFIX,(unsigned)SECURESENSE_NODE_ID);
  snprintf(topicNodeStatus,sizeof(topicNodeStatus),"%s/node/%u/status",MQTT_TOPIC_PREFIX,(unsigned)SECURESENSE_NODE_ID);
  snprintf(topicNodeEvent,sizeof(topicNodeEvent),"%s/node/%u/event",MQTT_TOPIC_PREFIX,(unsigned)SECURESENSE_NODE_ID);
  snprintf(topicNodeCmd,sizeof(topicNodeCmd),"%s/node/%u/cmd",MQTT_TOPIC_PREFIX,(unsigned)SECURESENSE_NODE_ID);
  snprintf(topicNodeCmdResult,sizeof(topicNodeCmdResult),"%s/node/%u/cmd_result",MQTT_TOPIC_PREFIX,(unsigned)SECURESENSE_NODE_ID);
}

// ---------------------------------------------------------------- setup / loop
void setup(){
  Serial.begin(115200);delay(900);
  Serial.println("\n=== SECURESENSE MAIN HUB: ESP32-S3 (Stage 8: Wi-Fi + MQTT) ===");
  Serial.printf("[HUB] Reset reason code=%d (use to diagnose unexpected reboots)\n",(int)esp_reset_reason());
  wifiEnabled=strlen(WIFI_SSID)>0;
  Serial.printf("Node expected: ID %u | protocol v%u | mode=%s\n",
    SECURESENSE_NODE_ID,SECURESENSE_VERSION,wifiEnabled?"Wi-Fi+MQTT (router channel)":"OFFLINE (fixed channel)");
  incoming=xQueueCreate(16,sizeof(Inbound));
  mqttCmdQueue=xQueueCreate(4,sizeof(MqttCmd));
  if(!incoming||!mqttCmdQueue){Serial.println("ERROR: queue allocation failed");return;}
  buildTopics();

  Serial.println("[RADIO] Starting Wi-Fi STA for ESP-NOW...");
  WiFi.setSleep(false);  // ESP-NOW reception needs the radio awake
  if(!WiFi.mode(WIFI_STA)){Serial.println("[FATAL] WiFi.mode(WIFI_STA) failed");return;}
  uint32_t start=millis();
  while(!WiFi.STA.started() && (uint32_t)(millis()-start)<4000) delay(20);
  if(!WiFi.STA.started()){Serial.println("[FATAL] Wi-Fi STA did not start");return;}
  esp_err_t ps=esp_wifi_set_ps(WIFI_PS_NONE);
  if(ps!=ESP_OK) Serial.printf("[WARN] esp_wifi_set_ps: %s (%d)\n",esp_err_to_name(ps),(int)ps);

  uint8_t mac[6];WiFi.macAddress(mac);
  snprintf(mqttClientId,sizeof(mqttClientId),"securesense-hub-%02x%02x%02x",mac[3],mac[4],mac[5]);

  if(wifiEnabled){
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
    Serial.printf("[WIFI] Joining \"%s\" (2.4 GHz only)...\n",WIFI_SSID);
  } else {
    // Arduino-ESP32 WiFi.setChannel returns 0 (ESP_OK) on SUCCESS.
    // Use the native ESP-IDF API and check the real error code instead.
    esp_err_t channelErr = esp_wifi_set_channel(SECURESENSE_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if(channelErr != ESP_OK){
      Serial.printf("[FATAL] esp_wifi_set_channel: %s (%d)\n", esp_err_to_name(channelErr), (int)channelErr);
      return;
    }
  }
  Serial.print("[RADIO] S3 WiFi STA MAC: ");Serial.println(WiFi.macAddress());
  Serial.printf("[RADIO] Wi-Fi channel=%d (changes to the router's channel once connected)\n",WiFi.channel());
  esp_err_t err=esp_now_init();
  if(err!=ESP_OK){Serial.printf("[FATAL] esp_now_init: %s (%d)\n",esp_err_to_name(err),(int)err);return;}
  err=esp_now_register_recv_cb(onRecv);
  if(err!=ESP_OK){Serial.printf("[FATAL] esp_now_register_recv_cb: %s (%d)\n",esp_err_to_name(err),(int)err);return;}
  radioReady=true;
  Serial.println("[RADIO] S3 HUB READY. Power C3 node and look for C3 HELLO.");
  help();
}
void loop(){
  if(incoming)processRadio();
  processMqttCommands();
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
    if(pendingSource==SRC_MQTT)
      publishCommandResult(pendingRequestId,pendingCommand,"TIMEOUT_UNKNOWN",nullptr);
  }
  if(nodeKnown && (uint32_t)(millis()-lastNodeSeen)>NODE_STALE_MS &&
     (uint32_t)(millis()-lastTimeoutWarnMs)>8000){
    Serial.println("[HUB] C3 telemetry timeout >8s. Check node power, or that the C3 runs the Stage 8 channel-scan sketch.");
    lastTimeoutWarnMs=millis();
  }
  serviceNetwork();
  if((uint32_t)(millis()-lastHubHeartbeatMs)>=10000){
    lastHubHeartbeatMs=millis();
    Serial.printf("[HUB HEARTBEAT] uptime=%lu ms | node=%s | received=%lu | wifi=%s | mqtt=%s\n",
                  (unsigned long)millis(), nodeKnown ? "DISCOVERED" : "WAITING",
                  (unsigned long)packetsReceived,
                  !wifiEnabled?"off":(wifiUp?"up":"down"),
                  (!wifiEnabled||mqttDisabled)?"off":(mqttConnected?"up":"down"));
  }
  delay(10);
}

#pragma once
#include <stdint.h>

// SecureSense Stage 7 lab protocol. BOTH boards must use this v2 header.
// Not encrypted/authenticated: for isolated bench tests ONLY, not deployment.
constexpr uint32_t SECURESENSE_MAGIC = 0x53454E53UL; // SENS
constexpr uint8_t SECURESENSE_VERSION = 2;
constexpr uint8_t SECURESENSE_NODE_ID = 2;
constexpr uint8_t SECURESENSE_CHANNEL = 4;

enum PacketType : uint8_t { PK_HELLO=1, PK_STATUS=2, PK_COMMAND=3, PK_ACK=4 };
enum CommandCode : uint8_t {
  CMD_NONE=0, CMD_PING=1, CMD_RED=2, CMD_GREEN=3,
  CMD_LEDS_OFF=4, CMD_BEEP=5, CMD_GET_STATUS=6,
  CMD_ARM=7, CMD_DISARM=8, CMD_SILENCE=9,
  CMD_RESET_REARM=10
};

enum CommandResult : uint8_t {
  RESULT_NONE=0, RESULT_OK=1,
  RESULT_NOT_ARMED=2, RESULT_NO_LATCHED_ALARM=3,
  RESULT_REED_OPEN=4, RESULT_REED_NOT_SETTLED=5,
  RESULT_PIR_ACTIVE=6, RESULT_PIR_NOT_SETTLED=7,
  RESULT_PIR_NOT_READY=8, RESULT_DIP_CHANGED=9,
  RESULT_NO_SENSORS=10, RESULT_ALREADY_ARMED=11,
  RESULT_UNKNOWN_COMMAND=12, RESULT_BUTTON_HELD=13
};

enum AlarmCause : uint8_t { CAUSE_NONE=0, CAUSE_REED=1, CAUSE_PIR=2 };

inline const char* commandResultName(uint8_t v) {
  switch(v){
    case RESULT_NONE: return "NONE";
    case RESULT_OK: return "ACCEPTED";
    case RESULT_NOT_ARMED: return "NOT_ARMED";
    case RESULT_NO_LATCHED_ALARM: return "NO_LATCHED_ALARM";
    case RESULT_REED_OPEN: return "REED_OPEN";
    case RESULT_REED_NOT_SETTLED: return "REED_NOT_STABLE";
    case RESULT_PIR_ACTIVE: return "PIR_STILL_ACTIVE";
    case RESULT_PIR_NOT_SETTLED: return "PIR_NOT_CLEAR_LONG_ENOUGH";
    case RESULT_PIR_NOT_READY: return "PIR_NOT_READY";
    case RESULT_DIP_CHANGED: return "DIP_CHANGED_WHILE_ARMED";
    case RESULT_NO_SENSORS: return "NO_SENSORS_SELECTED";
    case RESULT_ALREADY_ARMED: return "ALREADY_ARMED";
    case RESULT_UNKNOWN_COMMAND: return "UNKNOWN_COMMAND";
    case RESULT_BUTTON_HELD: return "LOCAL_BUTTON_HELD";
    default: return "UNKNOWN_RESULT";
  }
}
inline const char* alarmCauseName(uint8_t v){
  switch(v){case CAUSE_REED:return "REED"; case CAUSE_PIR:return "PIR"; default:return "NONE";}
}

struct __attribute__((packed)) SecureSensePacket {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
  uint8_t nodeId;
  uint8_t command;
  uint32_t sequence;
  uint32_t uptimeMs;
  uint8_t reedRaw;
  uint8_t reedClosed;
  uint8_t pirRaw;
  uint8_t pirReady;
  uint8_t buttonPressed;
  uint8_t armed;
  uint8_t alarm;
  uint8_t redOn;
  uint8_t greenOn;
  uint8_t buzzerOn;
  uint8_t reedEnabled;
  uint8_t pirEnabled;
  // v2: command correlation & explicit result; also expose silence and alarm cause.
  uint32_t responseToSequence;  // only nonzero for command-result ACK
  uint8_t commandResult;       // CommandResult for command-result ACK
  uint8_t alarmSilenced;       // status, command ACK
  uint8_t alarmCause;          // status, command ACK
};

inline SecureSensePacket makePacket(uint8_t type,uint32_t seq){
  SecureSensePacket p={};
  p.magic=SECURESENSE_MAGIC;
  p.version=SECURESENSE_VERSION;
  p.type=type;
  p.nodeId=SECURESENSE_NODE_ID;
  p.sequence=seq;
  return p;
}
inline bool validPacket(const SecureSensePacket &p){
  return p.magic==SECURESENSE_MAGIC && p.version==SECURESENSE_VERSION &&
         p.nodeId==SECURESENSE_NODE_ID;
}

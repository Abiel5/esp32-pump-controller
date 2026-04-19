//
// ESP32 WiFi + MQTT <-> Atlas EZO-PMP (I2C) bridge
// - Publish commands to esp32/pump/cmd (ASCII: i, D,5.0, X, SCAN, etc. OR JSON per examples below)
// - Device responds on esp32/pump/resp
// - Status heartbeat on esp32/pump/status (online [retained], ready, alive)
//

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#if __has_include("../include/secrets.h")
#include "../include/secrets.h"
#else
#error "Missing secrets.h. Copy include/secrets.h.example to include/secrets.h and fill in credentials."
#endif

// ---------- MQTT / WiFi SETTINGS ----------
const char* mqtt_broker       = "10.0.0.232";   // Pi's LAN IP (not 127.0.0.1)
//const char* mqtt_broker       = "10.0.0.134";   // Pi's LAN IP (not 127.0.0.1)
const uint16_t mqtt_port      = 1883;
const char* mqtt_client_id    = "esp32-pump-01"; // <=23 chars, unique per device
const char* mqtt_cmd_topic    = "esp32/pump/cmd";
const char* mqtt_resp_topic   = "esp32/pump/resp";

const char* mqtt_user = MQTT_USER;
const char* mqtt_pass = MQTT_PASS;

const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;
// ----------------------------------------

// ---------- I2C / PUMP SETTINGS ----------
const uint8_t I2C_SDA = 22;
const uint8_t I2C_SCL = 20;

// Scalable pump registry (extend this list as you add pumps / Infineon channels)
struct PumpConfig {
  const char* pumpId;     // logical ID used by JSON, e.g. "pump_1"
  uint8_t i2cAddr;        // EZO-PMP address, e.g. 0x67
  uint8_t board;          // PROFET board index (0..N)
  uint8_t channel;        // PROFET channel index (1..4)
  float mlPerSec;         // calibrated flow estimate for timed dosing
};

static const uint8_t CHANNELS_PER_BOARD = 4;
// Per-board GPIO mapping for PROFET control.
static const int8_t BOARD_OUT_PINS[][CHANNELS_PER_BOARD] = {
  {32, 33, 27, 25},   // Board 0: pump_1–pump_4
  {26, 13, -1, -1},   // Board 1: pump_5–pump_6
};
static const size_t BOARD_COUNT = sizeof(BOARD_OUT_PINS) / sizeof(BOARD_OUT_PINS[0]);

static PumpConfig pumps[] = {
  {"pump_1", 0x64, 0, 1, 1.0f},
  {"pump_2", 0x65, 0, 2, 1.0f},
  {"pump_3", 0x66, 0, 3, 1.0f},
  {"pump_4", 0x68, 0, 4, 1.0f},
  {"pump_5", 0x69, 1, 1, 1.0f},
  {"pump_6", 0x6A, 1, 2, 1.0f},
  //{"pump_7", 0x67, 1, 3, 1.0f},
};
static const size_t PUMP_COUNT = sizeof(pumps) / sizeof(pumps[0]);
static const uint32_t DOSE_BUFFER_SEC = 5;

WiFiClient   mqttNet;
PubSubClient mqttClient(mqttNet);

struct CommandResult {
  bool ok;
  uint8_t statusCode; 
  String raw;
};

enum ActiveJobState : uint8_t {
  JOB_IDLE = 0,
  JOB_SETTLING,
  JOB_WAITING_RESPONSE,
  JOB_FINITE_RUNNING,
  JOB_CONTINUOUS_RUNNING
};

struct ActiveMotorJob {
  bool busy = false;
  ActiveJobState state = JOB_IDLE;
  const PumpConfig* pump = nullptr;
  String pumpId;
  String requestId;
  String cmd;
  uint16_t waitMs = 300;
  unsigned long settleUntil = 0;
  unsigned long readAt = 0;
  unsigned long expectedDoneAt = 0;
};

struct PumpJob {
  String pumpId;
  String requestId;
  String cmd;
  uint16_t waitMs;
};

static ActiveMotorJob activeJob;
static const uint8_t MOTOR_QUEUE_MAX = 8;
static PumpJob motorQueue[MOTOR_QUEUE_MAX];
static uint8_t queueHead = 0;
static uint8_t queueTail = 0;
static uint8_t queueCount = 0;

static bool resolveBoardChannelPins(uint8_t board, uint8_t channel, int8_t& outPin) {
  if (board >= BOARD_COUNT) return false;
  if (channel == 0 || channel > CHANNELS_PER_BOARD) return false;
  uint8_t idx = channel - 1;
  outPin = BOARD_OUT_PINS[board][idx];
  return outPin >= 0;
}

static inline bool setPumpPower(uint8_t board, uint8_t channel, bool on) {
  int8_t outPin = -1;
  if (!resolveBoardChannelPins(board, channel, outPin)) {
    return false;
  }
  digitalWrite(outPin, on ? 1 : 0);
  return true;
}

static inline bool pumpPowerSet(const PumpConfig& pump, bool on) {
  return setPumpPower(pump.board, pump.channel, on);
}

static const PumpConfig* findPumpById(const String& pumpId) {
  for (size_t i = 0; i < PUMP_COUNT; i++) {
    if (pumpId.equalsIgnoreCase(pumps[i].pumpId)) {
      return &pumps[i];
    }
  }
  return nullptr;
}

static const PumpConfig& defaultPump() {
  return pumps[0];
}

static String scanI2CBus() {
  String found = "I2C:";
  bool any = false;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      if (any) found += ",";
      char buf[6];
      snprintf(buf, sizeof(buf), "0x%02X", addr);
      found += buf;
      any = true;
    }
    delay(2);
  }
  if (!any) return "I2C:NONE";
  return found;
}

static CommandResult pumpQuery(const PumpConfig& pump, const String& cmd, uint16_t waitMs) {
  Wire.beginTransmission(pump.i2cAddr);
  Serial.print("pumpQuery[");
  Serial.print(pump.pumpId);
  Serial.print("] -> ");
  Serial.println(cmd);
  Wire.write((const uint8_t*)cmd.c_str(), cmd.length());
  Wire.write((uint8_t)0x00);
  uint8_t err = Wire.endTransmission();
  if (err != 0) {
    return {false, 0, String("I2C_ERR_") + err};
  }

  delay(waitMs);

  uint8_t n = Wire.requestFrom((int)pump.i2cAddr, (int)64);
  if (n == 0) {
    return {false, 0, "NO_DATA"};
  }

  uint8_t status = Wire.read();
  String resp;
  if (status != 1) {
    resp += String("STATUS_") + status + " ";
  }
  while (Wire.available()) {
    char c = (char)Wire.read();
    if (c == '\0') {
      break;
    }
    resp += c;
  }
  if (resp.length() == 0) {
    resp = (status == 1) ? "OK" : (String("STATUS_") + status);
  }
  return {status == 1, status, resp};
}

// Estimate how long a finite dose will take (seconds).
// Uses pump-specific mlPerSec.
// D,<mL>            -> mL seconds + BUFFER
// DS,<mL>,<minutes> -> minutes*60 + BUFFER
static uint32_t estimateDoseSeconds(const PumpConfig& pump, const String& cmd) {
  String c = cmd; c.trim();

  // DS,<mL>,<minutes>  — dose over time
  if (c.length() >= 3 && c.substring(0,3).equalsIgnoreCase("DS,")) {
    int lastComma = c.lastIndexOf(',');
    if (lastComma > 3) {
      float mins = c.substring(lastComma + 1).toFloat();
      if (mins > 0) return (uint32_t)(mins * 60) + DOSE_BUFFER_SEC;
    }
  }

  // D,<mL>  — dose a volume (not D,*)
  if (c.length() >= 2 && c.substring(0,2).equalsIgnoreCase("D,") && c.indexOf('*') < 0) {
    float ml = c.substring(2).toFloat();
    if (ml < 0) ml = -ml; // handle reverse dosing
    float rate = (pump.mlPerSec > 0.0f) ? pump.mlPerSec : 1.0f;
    if (ml > 0) return (uint32_t)(ml / rate) + DOSE_BUFFER_SEC;
  }

  // fallback — 10 seconds
  return 10 + DOSE_BUFFER_SEC;
}

// ---- COMMAND HANDLERS ----
// Returns true if this EZO-PMP command requires the motor to run
static bool isMotorCommand(const String& cmd) {
  String c = cmd; c.trim();
  if (c.equalsIgnoreCase("R"))  return true;  // run continuously
  if (c.equalsIgnoreCase("P"))  return true;  // pause
  if (c.length() >= 2 && c.substring(0,2).equalsIgnoreCase("D,"))  return true; // dose variants
  if (c.length() >= 3 && c.substring(0,3).equalsIgnoreCase("DC,")) return true; // dose continuous at rate
  if (c.length() >= 3 && c.substring(0,3).equalsIgnoreCase("DS,")) return true; // dose over time
  return false;
}

// Returns true for commands that keep the motor running after the command returns
static bool isContinuousCommand(const String& cmd) {
  String c = cmd; c.trim();
  if (c.equalsIgnoreCase("R"))   return true;   // run continuously
  if (c.equalsIgnoreCase("D,*")) return true;   // dose continuously
  if (c.length() >= 3 && c.substring(0,3).equalsIgnoreCase("DC,")) return true; // dose at rate
  return false;
}

static void publishPumpEvent(const String& pumpId, const String& requestId, const String& state, int queuePosition = -1, const String& reason = "") {
  JsonDocument evt;
  evt["v"] = 1;
  evt["type"] = "pump_event";
  evt["pump_id"] = pumpId;
  if (requestId.length()) evt["request_id"] = requestId;
  evt["state"] = state;
  if (queuePosition > 0) evt["queue_position"] = queuePosition;
  if (reason.length()) evt["reason"] = reason;

  String payload;
  serializeJson(evt, payload);
  mqttClient.publish(mqtt_resp_topic, payload.c_str(), false);
}

static void clearActiveJob() {
  activeJob.busy = false;
  activeJob.state = JOB_IDLE;
  activeJob.pump = nullptr;
  activeJob.pumpId = "";
  activeJob.requestId = "";
  activeJob.cmd = "";
  activeJob.waitMs = 300;
  activeJob.settleUntil = 0;
  activeJob.readAt = 0;
  activeJob.expectedDoneAt = 0;
}

static bool enqueueMotorJob(const PumpJob& job, int& outQueuePosition) {
  if (queueCount >= MOTOR_QUEUE_MAX) {
    outQueuePosition = -1;
    return false;
  }
  motorQueue[queueTail] = job;
  queueTail = (uint8_t)((queueTail + 1) % MOTOR_QUEUE_MAX);
  queueCount++;
  outQueuePosition = queueCount;
  return true;
}

static bool dequeueMotorJob(PumpJob& outJob) {
  if (queueCount == 0) return false;
  outJob = motorQueue[queueHead];
  queueHead = (uint8_t)((queueHead + 1) % MOTOR_QUEUE_MAX);
  queueCount--;
  return true;
}

static CommandResult pumpSendOnly(const PumpConfig& pump, const String& cmd) {
  Wire.beginTransmission(pump.i2cAddr);
  Wire.write((const uint8_t*)cmd.c_str(), cmd.length());
  Wire.write((uint8_t)0x00);
  uint8_t err = Wire.endTransmission();
  if (err != 0) return {false, 0, String("I2C_ERR_") + err};
  return {true, 1, "SENT"};
}

static CommandResult pumpReadResponseOnly(const PumpConfig& pump) {
  uint8_t n = Wire.requestFrom((int)pump.i2cAddr, (int)64);
  if (n == 0) return {false, 0, "NO_DATA"};

  uint8_t status = Wire.read();
  String resp;
  if (status != 1) {
    resp += String("STATUS_") + status + " ";
  }
  while (Wire.available()) {
    char c = (char)Wire.read();
    if (c == '\0') break;
    resp += c;
  }
  if (resp.length() == 0) {
    resp = (status == 1) ? "OK" : (String("STATUS_") + status);
  }
  return {status == 1, status, resp};
}

// Non-motor commands execute immediately and bypass scheduler.
static CommandResult processImmediateCommandForPump(const PumpConfig& pump, const String& command, uint16_t waitMs = 300) {
  String cmd = command;
  cmd.trim();
  if (!cmd.length()) return {false, 0, ""};

  if (cmd.equalsIgnoreCase("POWERON")) {
    if (activeJob.busy && !activeJob.pumpId.equalsIgnoreCase(pump.pumpId)) {
      return {false, 0, String("ERR:MOTOR_BUSY active=") + activeJob.pumpId};
    }
    if (!pumpPowerSet(pump, true)) {
      return {false, 0, String("PINMAP_ERR board=") + pump.board + " ch=" + pump.channel};
    }
    return {true, 1, String(pump.pumpId) + " OUT:ON"};
  }
  if (cmd.equalsIgnoreCase("POWEROFF")) {
    if (!pumpPowerSet(pump, false)) {
      return {false, 0, String("PINMAP_ERR board=") + pump.board + " ch=" + pump.channel};
    }
    return {true, 1, String(pump.pumpId) + " OUT:OFF"};
  }

  return pumpQuery(pump, cmd, waitMs);
}

static CommandResult startMotorJob(const PumpJob& job) {
  if (activeJob.busy) {
    return {false, 0, String("ERR:MOTOR_BUSY active=") + activeJob.pumpId};
  }

  const PumpConfig* pump = findPumpById(job.pumpId);
  if (!pump) {
    return {false, 0, String("ERR:PUMP_NOT_FOUND:") + job.pumpId};
  }

  if (!pumpPowerSet(*pump, true)) {
    return {false, 0, String("PINMAP_ERR board=") + pump->board + " ch=" + pump->channel};
  }

  activeJob.busy = true;
  activeJob.state = JOB_SETTLING;
  activeJob.pump = pump;
  activeJob.pumpId = job.pumpId;
  activeJob.requestId = job.requestId;
  activeJob.cmd = job.cmd;
  activeJob.waitMs = job.waitMs;
  activeJob.settleUntil = millis() + 2000;
  activeJob.readAt = 0;
  activeJob.expectedDoneAt = 0;
  return {true, 1, "RUNNING"};
}

static CommandResult stopActiveMotorForPump(const String& pumpId) {
  if (!activeJob.busy || activeJob.pump == nullptr) {
    return {false, 0, "ERR:NO_ACTIVE_MOTOR"};
  }
  if (pumpId.length() && !activeJob.pumpId.equalsIgnoreCase(pumpId)) {
    return {false, 0, String("ERR:ACTIVE_MOTOR_OTHER:") + activeJob.pumpId};
  }

  CommandResult stopResp = pumpQuery(*activeJob.pump, "X", 150);
  pumpPowerSet(*activeJob.pump, false);
  publishPumpEvent(activeJob.pumpId, activeJob.requestId, "completed");
  clearActiveJob();
  return stopResp;
}

static void serviceMotorScheduler() {
  if (!activeJob.busy) {
    PumpJob nextJob;
    if (dequeueMotorJob(nextJob)) {
      CommandResult startRes = startMotorJob(nextJob);
      if (startRes.ok) {
        publishPumpEvent(nextJob.pumpId, nextJob.requestId, "running");
      } else {
        publishPumpEvent(nextJob.pumpId, nextJob.requestId, "rejected", -1, startRes.raw);
      }
    }
    return;
  }

  if (activeJob.pump == nullptr) {
    clearActiveJob();
    return;
  }

  unsigned long now = millis();
  switch (activeJob.state) {
    case JOB_SETTLING: {
      if ((long)(now - activeJob.settleUntil) < 0) break;

      CommandResult sendRes = pumpSendOnly(*activeJob.pump, activeJob.cmd);
      if (!sendRes.ok) {
        pumpPowerSet(*activeJob.pump, false);
        publishPumpEvent(activeJob.pumpId, activeJob.requestId, "rejected", -1, sendRes.raw);
        clearActiveJob();
        break;
      }

      activeJob.readAt = now + activeJob.waitMs;
      activeJob.state = JOB_WAITING_RESPONSE;
      break;
    }
    case JOB_WAITING_RESPONSE: {
      if ((long)(now - activeJob.readAt) < 0) break;

      CommandResult resp = pumpReadResponseOnly(*activeJob.pump);
      bool accepted = (resp.statusCode == 1 || resp.statusCode == 254);
      if (!accepted) {
        pumpPowerSet(*activeJob.pump, false);
        publishPumpEvent(activeJob.pumpId, activeJob.requestId, "rejected", -1, resp.raw);
        clearActiveJob();
        break;
      }

      if (isContinuousCommand(activeJob.cmd)) {
        activeJob.state = JOB_CONTINUOUS_RUNNING;
      } else {
        uint32_t secs = estimateDoseSeconds(*activeJob.pump, activeJob.cmd);
        activeJob.expectedDoneAt = now + (secs * 1000UL);
        activeJob.state = JOB_FINITE_RUNNING;
      }
      break;
    }
    case JOB_FINITE_RUNNING: {
      if ((long)(now - activeJob.expectedDoneAt) < 0) break;
      pumpPowerSet(*activeJob.pump, false);
      publishPumpEvent(activeJob.pumpId, activeJob.requestId, "completed");
      clearActiveJob();
      break;
    }
    case JOB_CONTINUOUS_RUNNING:
    default:
      // Wait for explicit stop command (X)
      break;
  }
}

// Initialize all configured high-side channels
static void shield_out_init_all() {
  for (size_t b = 0; b < BOARD_COUNT; b++) {
    for (uint8_t ch = 1; ch <= CHANNELS_PER_BOARD; ch++) {
      int8_t outPin = -1;
      if (!resolveBoardChannelPins((uint8_t)b, ch, outPin)) {
        continue;
      }
      pinMode(outPin, OUTPUT);
      digitalWrite(outPin, LOW);
    }
  }
}


static void connectToWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.printf("Connecting to WiFi SSID: %s", ssid);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 20000) {
    delay(250);
    Serial.print('.');
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connect timeout.");
  }
}

// ---- MQTT MESSAGE HANDLER ----
static void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  // Convert payload -> String (not null-terminated)
  String msg; 
  msg.reserve(len + 1);
  for (unsigned int i = 0; i < len; i++) {
    msg += (char)payload[i];
  }
  msg.trim();

  Serial.print("[MQTT] RX topic="); Serial.print(topic);
  Serial.print(" payload="); Serial.println(msg);

  String result;
  bool requestWasJson = false;

  // JSON mode (multi-pump routing + bridge commands)
  JsonDocument doc;
  DeserializationError jerr = deserializeJson(doc, msg);
  if (!jerr) {
    requestWasJson = true;
    String requestId;
    if (!doc["request_id"].isNull()) {
      requestId = doc["request_id"].as<String>();
    } else if (!doc["meta"]["request_id"].isNull()) {
      requestId = doc["meta"]["request_id"].as<String>();
    }

    const char* typeC = nullptr;
    if (!doc["type"].isNull()) typeC = doc["type"];
    String type = typeC ? String(typeC) : "pump_command";

    if (type.equalsIgnoreCase("bridge_command")) {
      String action;
      if (!doc["command"]["action"].isNull()) action = doc["command"]["action"].as<String>();
      else if (!doc["action"].isNull()) action = doc["action"].as<String>();

      String raw;
      bool ok = false;
      uint8_t statusCode = 0;
      if (action.equalsIgnoreCase("scan_all") || action.equalsIgnoreCase("scan")) {
        raw = scanI2CBus();
        ok = true;
        statusCode = 1;
      } else {
        raw = "ERR:UNKNOWN_BRIDGE_ACTION";
      }

      JsonDocument resp;
      resp["v"] = 1;
      resp["type"] = "bridge_response";
      if (requestId.length()) resp["request_id"] = requestId;
      JsonObject resultObj = resp["result"].to<JsonObject>();
      resultObj["ok"] = ok;
      resultObj["status_code"] = statusCode;
      resultObj["raw"] = raw;
      serializeJson(resp, result);
    } else {
      String pumpId = "pump_1";
      if (!doc["pump_id"].isNull()) pumpId = doc["pump_id"].as<String>();
      else if (!doc["target"]["pump_id"].isNull()) pumpId = doc["target"]["pump_id"].as<String>();

      String cmd;
      if (!doc["cmd"].isNull()) cmd = doc["cmd"].as<String>();
      else if (!doc["command"]["value"].isNull()) cmd = doc["command"]["value"].as<String>();

      uint16_t waitMs = 300;
      if (!doc["wait_ms"].isNull()) waitMs = doc["wait_ms"].as<uint16_t>();
      else if (!doc["options"]["wait_ms"].isNull()) waitMs = doc["options"]["wait_ms"].as<uint16_t>();

      bool queueIfBusy = true;
      if (!doc["options"]["queue_if_busy"].isNull()) {
        queueIfBusy = doc["options"]["queue_if_busy"].as<bool>();
      }

      bool ok = false;
      uint8_t statusCode = 0;
      String raw;

      const PumpConfig* pump = findPumpById(pumpId);
      if (!pump) {
        raw = String("ERR:PUMP_NOT_FOUND:") + pumpId;
      } else if (!cmd.length()) {
        raw = "ERR:CMD_MISSING";
      } else {
        bool motorCmd = isMotorCommand(cmd);
        bool stopCmd = cmd.equalsIgnoreCase("X");
        bool pauseCmd = cmd.equalsIgnoreCase("P");

        if (!motorCmd) {
          CommandResult pResp = processImmediateCommandForPump(*pump, cmd, waitMs);
          ok = pResp.ok;
          statusCode = pResp.statusCode;
          raw = pResp.raw;
        } else if (stopCmd) {
          CommandResult sResp = stopActiveMotorForPump(pumpId);
          ok = sResp.ok;
          statusCode = sResp.statusCode;
          raw = sResp.raw;
        } else if (pauseCmd) {
          if (!activeJob.busy) {
            raw = "ERR:NO_ACTIVE_MOTOR";
          } else if (!activeJob.pumpId.equalsIgnoreCase(pumpId)) {
            raw = String("ERR:ACTIVE_MOTOR_OTHER:") + activeJob.pumpId;
          } else {
            CommandResult pResp = pumpQuery(*pump, cmd, waitMs);
            ok = pResp.ok;
            statusCode = pResp.statusCode;
            raw = pResp.raw;
          }
        } else if (activeJob.busy) {
          if (queueIfBusy) {
            PumpJob job{pumpId, requestId, cmd, waitMs};
            int pos = -1;
            if (enqueueMotorJob(job, pos)) {
              ok = true;
              statusCode = 1;
              raw = "QUEUED";
              publishPumpEvent(pumpId, requestId, "queued", pos);
            } else {
              raw = "ERR:QUEUE_FULL";
              publishPumpEvent(pumpId, requestId, "rejected", -1, "queue_full");
            }
          } else {
            raw = String("ERR:MOTOR_BUSY active=") + activeJob.pumpId;
            publishPumpEvent(pumpId, requestId, "rejected", -1, "motor_busy");
          }
        } else {
          PumpJob job{pumpId, requestId, cmd, waitMs};
          CommandResult startRes = startMotorJob(job);
          ok = startRes.ok;
          statusCode = startRes.statusCode;
          raw = startRes.ok ? "RUNNING" : startRes.raw;
          if (startRes.ok) {
            publishPumpEvent(pumpId, requestId, "running");
          } else {
            publishPumpEvent(pumpId, requestId, "rejected", -1, startRes.raw);
          }
        }
      }

      JsonDocument resp;
      resp["v"] = 1;
      resp["type"] = "pump_response";
      resp["pump_id"] = pumpId;
      if (requestId.length()) resp["request_id"] = requestId;
      JsonObject resultObj = resp["result"].to<JsonObject>();
      resultObj["ok"] = ok;
      resultObj["status_code"] = statusCode;
      resultObj["raw"] = raw;
      serializeJson(resp, result);
    }
  }
  // Legacy mode: raw ASCII command on single topic -> default pump
  else {
    if (msg.equalsIgnoreCase("SCAN")) {
      result = scanI2CBus();
    } else if (isMotorCommand(msg)) {
      if (msg.equalsIgnoreCase("X")) {
        CommandResult sResp = stopActiveMotorForPump(defaultPump().pumpId);
        result = sResp.raw;
      } else if (msg.equalsIgnoreCase("P")) {
        if (!activeJob.busy || !activeJob.pumpId.equalsIgnoreCase(defaultPump().pumpId)) {
          result = "ERR:NO_ACTIVE_MOTOR";
        } else {
          CommandResult pResp = pumpQuery(defaultPump(), msg, 300);
          result = pResp.raw;
        }
      } else if (activeJob.busy) {
        result = String("ERR:MOTOR_BUSY active=") + activeJob.pumpId;
      } else {
        PumpJob job{defaultPump().pumpId, "", msg, 300};
        CommandResult startRes = startMotorJob(job);
        if (startRes.ok) {
          result = "RUNNING";
          publishPumpEvent(defaultPump().pumpId, "", "running");
        } else {
          result = startRes.raw;
        }
      }
    } else {
      CommandResult legacyResp = processImmediateCommandForPump(defaultPump(), msg, 300);
      result = legacyResp.raw;
    }
  }

  if (!result.length() && requestWasJson) {
    result = "{\"v\":1,\"type\":\"pump_response\",\"result\":{\"ok\":false,\"status_code\":0,\"raw\":\"EMPTY_RESULT\"}}";
  }
  if (!result.length() && !requestWasJson) {
    result = "EMPTY_RESULT";
  }
  mqttClient.publish(mqtt_resp_topic, result.c_str(), false);
  Serial.print("[MQTT] TX resp="); Serial.println(result);
}

// ---- MQTT CONNECT/RECONNECT ----
static bool mqttConnectOnce() {
  mqttClient.setServer(mqtt_broker, mqtt_port);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setKeepAlive(20);
  bool ok = mqttClient.connect(mqtt_client_id, mqtt_user, mqtt_pass);
  // bool ok = mqttClient.connect(mqtt_client_id);

  if (!ok) {
    Serial.print("[MQTT] connect failed, state="); Serial.println(mqttClient.state());
    return false;
  }

  mqttClient.subscribe(mqtt_cmd_topic);

  Serial.println("[MQTT] connected & subscribed");
  return true;
}

static void ensureMqtt() { if (!mqttClient.connected()) mqttConnectOnce(); }

// ---- ARDUINO LIFECYCLE ----
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\nStarting ESP32 Pump Controller...");

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  Serial.printf("I2C init SDA=%d SCL=%d\n", I2C_SDA, I2C_SCL);
  delay(50);
  String bootScan = scanI2CBus();
  Serial.print("[BOOT] I2C scan: ");
  Serial.println(bootScan);
  for (size_t i = 0; i < PUMP_COUNT; i++) {
    int8_t outPin = -1;
    bool mapped = resolveBoardChannelPins(pumps[i].board, pumps[i].channel, outPin);
    Serial.printf("Pump[%u] id=%s addr=0x%02X board=%u ch=%u out=%d mlps=%.2f mapped=%s\n",
                  (unsigned)i,
                  pumps[i].pumpId,
                  pumps[i].i2cAddr,
                  pumps[i].board,
                  pumps[i].channel,
                  outPin,
                  pumps[i].mlPerSec,
                  mapped ? "yes" : "no");
  }

  shield_out_init_all();

  connectToWifi();
  ensureMqtt();
  Serial.println("Multi-pump bridge ready. Raw commands target default pump_1; JSON supports pump_id routing.");
}

void loop() {
  serviceMotorScheduler();

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) ensureMqtt();
    mqttClient.loop();
  }

  delay(2);
}
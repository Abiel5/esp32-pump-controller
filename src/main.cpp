//
// ESP32 WiFi + MQTT <-> Atlas EZO-PMP (I2C) bridge
// - Publish commands to esp32/pump/cmd (ASCII: i, D,5.0, X, SCAN, etc. OR JSON per examples below)
// - Device responds on esp32/pump/resp
// - Status heartbeat on esp32/pump/status (online [retained], ready, alive)
//

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
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
const uint16_t mqtt_port      = 1883;
const char* mqtt_client_id    = "esp32-pump-01"; // <=23 chars, unique per device
const char* mqtt_cmd_topic    = "esp32/pump/cmd";
const char* mqtt_resp_topic   = "esp32/pump/resp";
const char* mqtt_status_topic = "esp32/pump/status";

const char* mqtt_user = MQTT_USER;
const char* mqtt_pass = MQTT_PASS;

const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;

const char* ota_password = OTA_PASSWORD;
// ----------------------------------------

// ---------- I2C / PUMP SETTINGS ----------
const uint8_t I2C_SDA = 22;
const uint8_t I2C_SCL = 20;

// Scalable pump registry (extend this list as you add pumps / Infineon channels)
struct PumpConfig {
  char pumpId[24];        // logical ID — mutable so rename_pump command works at runtime
  uint8_t i2cAddr;        // EZO-PMP address, e.g. 0x67
  uint8_t board;          // PROFET board index (0..N)
  uint8_t channel;        // PROFET channel index (1..4)
  float mlPerSec;         // calibrated flow estimate for timed dosing
};

static const uint8_t CHANNELS_PER_BOARD = 4;
// Per-board GPIO mapping for PROFET control.
static const int8_t BOARD_OUT_PINS[][CHANNELS_PER_BOARD] = {
  {32, 33, 27, 25},   // Board 0: calmag, micro, gro, bloom
  {26, 13, -1, -1},   // Board 1: phup–phdown
};
static const size_t BOARD_COUNT = sizeof(BOARD_OUT_PINS) / sizeof(BOARD_OUT_PINS[0]);

static PumpConfig pumps[] = {
  {"calmag", 0x64, 0, 1, 1.0f},  // Cal-Mag
  {"micro",  0x65, 0, 2, 1.0f},  // FloraMicro
  {"gro",    0x66, 0, 3, 1.0f},  // FloraGro
  {"bloom",  0x68, 0, 4, 1.0f},  // FloraBloom
  {"phup",   0x69, 1, 1, 1.0f},  // pH Up
  {"phdown", 0x6A, 1, 2, 1.0f},  // pH Down
  //{"pump_7", 0x67, 1, 3, 1.0f},
};
static const size_t PUMP_COUNT = sizeof(pumps) / sizeof(pumps[0]);
static const uint32_t DOSE_BUFFER_SEC = 0;

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

// ---------- BATCH COMMAND SUPPORT ----------
static const uint8_t BATCH_ITEMS_MAX = 16;

struct BatchItem {
  String itemId;
  String pumpId;
  String cmd;
  uint16_t waitMs;
  uint32_t postDelayMs;  // inter-step mixing delay after completion
  bool isMotor;
};

struct ActiveBatch {
  bool active = false;
  bool stopOnError = true;
  String batchId;
  BatchItem items[BATCH_ITEMS_MAX];
  size_t totalCount = 0;
  size_t currentIndex = 0;
  bool waitingForMotor = false;   // true while a batch-owned motor job is in flight
  bool waitingPostDelay = false;  // true while sitting in a post-step mixing delay
  unsigned long postDelayUntil = 0;
};

static ActiveBatch activeBatch;

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

// True for the characters an EZO reply is made of (printable ASCII).
static inline bool isReplyChar(char c) {
  return c >= 0x20 && c <= 0x7E;
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
    // A reply ends at the null terminator. Anything that is not printable ASCII (for example
    // 0xFF read from an idle bus) also ends it, so a bad read can never put junk into MQTT.
    if (!isReplyChar(c)) {
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
  if (c.equalsIgnoreCase("X"))  return true;  // stop
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

// Dose result metadata attached to completion/running events so the Pi can log what actually happened
struct DoseMeta {
  float requestedMl = -1.0f;   // what the command asked for
  float estimatedMl = -1.0f;   // ESP32 timer-based estimate
  float reportedMl  = -1.0f;   // pump-reported value (e.g. parsed from *DONE,v)
  String resultSource;          // "estimated" | "pump_reported" | "requested_only" | "unknown"
};

// Parse requested volume from D,<ml> or DS,<ml>,<min>
static float parseDoseMl(const String& cmd) {
  String c = cmd; c.trim();
  if (c.length() >= 2 && c.substring(0,2).equalsIgnoreCase("D,") && c.indexOf('*') < 0) {
    float ml = c.substring(2).toFloat();
    if (ml < 0) ml = -ml;
    return (ml > 0) ? ml : -1.0f;
  }
  if (c.length() >= 3 && c.substring(0,3).equalsIgnoreCase("DS,")) {
    int comma = c.indexOf(',', 3);
    if (comma > 2) {
      float ml = c.substring(3, comma).toFloat();
      if (ml < 0) ml = -ml;
      return (ml > 0) ? ml : -1.0f;
    }
  }
  return -1.0f;
}

// Parse pump-reported volume from *DONE,<v> (returned by X stop command)
static float parseDoneResponse(const String& raw) {
  if (raw.startsWith("*DONE,")) {
    float ml = raw.substring(6).toFloat();
    if (ml < 0) ml = -ml;
    return ml;
  }
  return -1.0f;
}

// Normalizes a firmware or pump error string ("ERR:MOTOR_BUSY active=gro", "I2C_ERR_2",
// "NO_DATA") to a snake-case code. Strings that already are codes pass through.
static String toErrorCode(const String& raw) {
  if (raw.startsWith("ERR:")) {
    String code;
    for (size_t i = 4; i < raw.length(); i++) {
      char ch = raw[i];
      if (ch == ':' || ch == ' ' || ch == '=') break;
      code += (char)tolower(ch);
    }
    return code.length() ? code : String("error");
  }
  if (raw.startsWith("I2C_ERR_"))  return "i2c_error";
  if (raw.startsWith("PINMAP_ERR")) return "pinmap_error";
  if (raw.startsWith("STATUS_"))   return "pump_error";
  if (raw == "NO_DATA")            return "no_data";

  bool isCode = raw.length() > 0;
  for (size_t i = 0; i < raw.length(); i++) {
    char ch = raw[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) { isCode = false; break; }
  }
  return isCode ? raw : String("pump_error");
}

// Sets doc[key] to the error code, and "message" to the original text when it has more detail.
static void setErrorFields(JsonDocument& doc, const char* key, const String& raw) {
  String code = toErrorCode(raw);
  doc[key] = code;
  if (code != raw && raw.length()) doc["message"] = raw;
}

// Envelope shared by every reply on the resp topic.
static void beginResponse(JsonDocument& resp, const String& requestId, bool ok, const String& rawError = "") {
  resp["v"] = 1;
  resp["type"] = "pump_response";
  if (requestId.length()) resp["request_id"] = requestId;
  resp["ok"] = ok;
  if (!ok) setErrorFields(resp, "error", rawError);
}

static void publishControllerStatus(const String& state, bool retained = false) {
  JsonDocument doc;
  doc["v"] = 1;
  doc["type"] = "controller_status";
  doc["controller_id"] = mqtt_client_id;
  doc["state"] = state;
  if (state == "online") {
    doc["wifi"] = "connected";
    doc["mqtt"] = "connected";
    doc["ip"] = WiFi.localIP().toString();
    doc["uptime_sec"] = (unsigned long)(millis() / 1000);
  } else if (state == "alive") {
    doc["uptime_sec"] = (unsigned long)(millis() / 1000);
    doc["free_heap"] = (unsigned long)ESP.getFreeHeap();
    doc["wifi_rssi"] = (int)WiFi.RSSI();
    doc["active_batch"] = activeBatch.active;
    doc["motor_busy"] = activeJob.busy;
  }
  String payload;
  serializeJson(doc, payload);
  mqttClient.publish(mqtt_status_topic, payload.c_str(), retained);
}

static void publishPumpEvent(const String& pumpId, const String& requestId, const String& state, int queuePosition = -1, const String& reason = "", const DoseMeta& dose = DoseMeta()) {
  JsonDocument evt;
  evt["v"] = 1;
  evt["type"] = "pump_event";
  evt["pump_id"] = pumpId;
  if (requestId.length()) evt["request_id"] = requestId;
  evt["state"] = state;
  if (queuePosition > 0) evt["queue_position"] = queuePosition;
  if (reason.length()) setErrorFields(evt, "reason", reason);
  if (dose.requestedMl >= 0) evt["requested_ml"] = dose.requestedMl;
  if (dose.estimatedMl >= 0) evt["estimated_ml"] = dose.estimatedMl;
  if (dose.reportedMl  >= 0) evt["reported_ml"]  = dose.reportedMl;
  if (dose.resultSource.length()) evt["result_source"] = dose.resultSource;

  String payload;
  serializeJson(evt, payload);
  mqttClient.publish(mqtt_resp_topic, payload.c_str(), false);
}

static void publishBatchEvent(const String& batchId, const String& state, int failedIndex = -1, const String& reason = "") {
  JsonDocument evt;
  evt["v"] = 1;
  evt["type"] = "batch_event";
  if (batchId.length()) evt["request_id"] = batchId;
  evt["state"] = state;
  if (activeBatch.active) evt["count"] = (int)activeBatch.totalCount;
  if (failedIndex >= 0) evt["failed_index"] = failedIndex;
  if (reason.length()) setErrorFields(evt, "reason", reason);
  String payload;
  serializeJson(evt, payload);
  mqttClient.publish(mqtt_resp_topic, payload.c_str(), false);
}

static void publishBatchItemEvent(const String& batchId, const BatchItem& item, size_t index, const String& state, const String& reason = "", uint32_t postDelayMs = 0, const DoseMeta& dose = DoseMeta()) {
  JsonDocument evt;
  evt["v"] = 1;
  evt["type"] = "batch_item_event";
  if (batchId.length()) evt["request_id"] = batchId;
  if (item.itemId.length()) evt["item_id"] = item.itemId;
  evt["pump_id"] = item.pumpId;
  evt["cmd"] = item.cmd;
  evt["state"] = state;
  evt["index"] = (int)index;
  if (postDelayMs > 0) evt["post_delay_ms"] = postDelayMs;
  if (reason.length()) setErrorFields(evt, "reason", reason);
  if (dose.requestedMl >= 0) evt["requested_ml"] = dose.requestedMl;
  if (dose.estimatedMl >= 0) evt["estimated_ml"] = dose.estimatedMl;
  if (dose.reportedMl  >= 0) evt["reported_ml"]  = dose.reportedMl;
  if (dose.resultSource.length()) evt["result_source"] = dose.resultSource;
  String payload;
  serializeJson(evt, payload);
  mqttClient.publish(mqtt_resp_topic, payload.c_str(), false);
}

// Forward declarations — advanceBatch body calls these, which are defined later in the file.
static CommandResult processImmediateCommandForPump(const PumpConfig& pump, const String& command, uint16_t waitMs = 300);
static CommandResult startMotorJob(const PumpJob& job);
static CommandResult stopActiveMotorForPump(const String& pumpId);
// publishPriorCompleted: true when called from motor completion (clearActiveJob), false when inline.
static void advanceBatch(bool lastItemFailed, bool publishPriorCompleted = false, const String& failReason = "");

static void clearActiveJob(bool fromBatch = false, bool jobFailed = false, const String& failReason = "") {
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

  if (fromBatch && activeBatch.active && activeBatch.waitingForMotor) {
    activeBatch.waitingForMotor = false;

    if (!jobFailed) {
      // Publish item completed before deciding whether to delay or advance
      BatchItem& completed = activeBatch.items[activeBatch.currentIndex];
      {
        float reqMl = parseDoseMl(completed.cmd);
        DoseMeta dose;
        if (reqMl >= 0) { dose.requestedMl = reqMl; dose.estimatedMl = reqMl; dose.resultSource = "estimated"; }
        publishBatchItemEvent(activeBatch.batchId, completed, activeBatch.currentIndex, "completed", "", 0, dose);
      }

      if (completed.postDelayMs > 0) {
        activeBatch.waitingPostDelay = true;
        activeBatch.postDelayUntil   = millis() + completed.postDelayMs;
        publishBatchItemEvent(activeBatch.batchId, completed, activeBatch.currentIndex,
                              "post_delay", "", completed.postDelayMs);
        return;
      }
    }

    advanceBatch(jobFailed, /*publishPriorCompleted=*/false, failReason);
  }
}

// Advance to the next batch item (or finish the batch).
// processImmediateCommandForPump is defined later — forward-declared via the pump helpers above.
static void advanceBatch(bool lastItemFailed, bool publishPriorCompleted, const String& failReason) {
  if (!activeBatch.active) return;

  size_t idx = activeBatch.currentIndex;

  if (lastItemFailed) {
    publishBatchItemEvent(activeBatch.batchId, activeBatch.items[idx], idx, "failed", failReason);
    if (activeBatch.stopOnError) {
      publishBatchEvent(activeBatch.batchId, "aborted", (int)idx, "item_failed");
      activeBatch.active = false;
      return;
    }
  } else if (publishPriorCompleted && idx < activeBatch.totalCount) {
    // Motor job finished — emit batch item completed
    publishBatchItemEvent(activeBatch.batchId, activeBatch.items[idx], idx, "completed");
  }

  // Advance
  activeBatch.currentIndex++;

  // Run consecutive non-motor items inline
  while (activeBatch.currentIndex < activeBatch.totalCount) {
    idx = activeBatch.currentIndex;
    BatchItem& item = activeBatch.items[idx];

    {
      DoseMeta runDose; runDose.requestedMl = parseDoseMl(item.cmd);
      publishBatchItemEvent(activeBatch.batchId, item, idx, "running", "", 0, runDose);
    }

    if (item.isMotor) {
      String c = item.cmd; c.trim();
      bool isStop  = c.equalsIgnoreCase("X");
      bool isPause = c.equalsIgnoreCase("P");

      if (isStop || isPause) {
        // Stop/pause apply to the currently active motor — run inline like non-motor
        const PumpConfig* pump = findPumpById(item.pumpId);
        CommandResult r = {false, 0, "ERR:PUMP_NOT_FOUND"};
        if (isStop) {
          r = stopActiveMotorForPump(item.pumpId);
        } else if (pump && activeJob.busy && activeJob.pumpId.equalsIgnoreCase(item.pumpId)) {
          r = pumpQuery(*pump, "P", item.waitMs);
        } else {
          r = {false, 0, "ERR:NO_ACTIVE_MOTOR"};
        }
        if (!r.ok) {
          publishBatchItemEvent(activeBatch.batchId, item, idx, "failed", r.raw);
          if (activeBatch.stopOnError) {
            publishBatchEvent(activeBatch.batchId, "aborted", (int)idx, "item_failed");
            activeBatch.active = false;
            return;
          }
        } else {
          publishBatchItemEvent(activeBatch.batchId, item, idx, "completed");
        }
        activeBatch.currentIndex++;
        continue;
      }

      // Hand off to scheduler — it will call clearActiveJob(fromBatch=true) when done
      PumpJob job{item.pumpId, activeBatch.batchId + ":" + item.itemId, item.cmd, item.waitMs};
      CommandResult startRes = startMotorJob(job);
      if (!startRes.ok) {
        // Failed to start; treat as failed item
        publishBatchItemEvent(activeBatch.batchId, item, idx, "failed", startRes.raw);
        if (activeBatch.stopOnError) {
          publishBatchEvent(activeBatch.batchId, "aborted", (int)idx, "item_failed");
          activeBatch.active = false;
          return;
        }
        activeBatch.currentIndex++;
        continue;
      }
      activeBatch.waitingForMotor = true;
      // Scheduler will resume batch on completion
      return;
    } else {
      // Non-motor: execute immediately
      const PumpConfig* pump = findPumpById(item.pumpId);
      String raw;
      bool ok = false;
      if (!pump) {
        raw = String("ERR:PUMP_NOT_FOUND:") + item.pumpId;
      } else {
        CommandResult r = processImmediateCommandForPump(*pump, item.cmd, item.waitMs);
        ok = r.ok;
        raw = r.raw;
      }
      if (!ok) {
        publishBatchItemEvent(activeBatch.batchId, item, idx, "failed", raw);
        if (activeBatch.stopOnError) {
          publishBatchEvent(activeBatch.batchId, "aborted", (int)idx, "item_failed");
          activeBatch.active = false;
          return;
        }
      } else {
        publishBatchItemEvent(activeBatch.batchId, item, idx, "completed");
      }
      activeBatch.currentIndex++;
    }
  }

  // All items done
  size_t doneCount = activeBatch.totalCount;
  String doneBatchId = activeBatch.batchId;
  activeBatch.active = false;
  JsonDocument doneEvt;
  doneEvt["v"] = 1;
  doneEvt["type"] = "batch_event";
  if (doneBatchId.length()) doneEvt["request_id"] = doneBatchId;
  doneEvt["state"] = "completed";
  doneEvt["count"] = (int)doneCount;
  String donePayload;
  serializeJson(doneEvt, donePayload);
  mqttClient.publish(mqtt_resp_topic, donePayload.c_str(), false);
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

// Non-motor commands execute immediately and bypass scheduler.
static CommandResult processImmediateCommandForPump(const PumpConfig& pump, const String& command, uint16_t waitMs) {
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
  activeJob.settleUntil = millis() + 1000;
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
  {
    float reqMl = parseDoseMl(activeJob.cmd);
    float repMl = parseDoneResponse(stopResp.raw);
    DoseMeta dose;
    if (reqMl >= 0) dose.requestedMl = reqMl;
    if (repMl >= 0) { dose.reportedMl = repMl; dose.resultSource = "pump_reported"; }
    else if (reqMl >= 0) { dose.estimatedMl = reqMl; dose.resultSource = "estimated"; }
    else { dose.resultSource = "unknown"; }
    publishPumpEvent(activeJob.pumpId, activeJob.requestId, "completed", -1, "", dose);
  }
  clearActiveJob(true, false);
  return stopResp;
}

static void serviceMotorScheduler() {
  if (!activeJob.busy) {
    PumpJob nextJob;
    if (dequeueMotorJob(nextJob)) {
      CommandResult startRes = startMotorJob(nextJob);
      if (startRes.ok) {
        DoseMeta runDose;
        runDose.requestedMl = parseDoseMl(nextJob.cmd);
        publishPumpEvent(nextJob.pumpId, nextJob.requestId, "running", -1, "", runDose);
      } else {
        publishPumpEvent(nextJob.pumpId, nextJob.requestId, "rejected", -1, startRes.raw);
      }
    }
    return;
  }

  if (activeJob.pump == nullptr) {
    clearActiveJob(true, true, "null_pump");
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
        clearActiveJob(true, true, sendRes.raw);
        break;
      }

      activeJob.readAt = now + activeJob.waitMs;
      activeJob.state = JOB_WAITING_RESPONSE;
      break;
    }
    case JOB_WAITING_RESPONSE: {
      if ((long)(now - activeJob.readAt) < 0) break;

      uint8_t rn = Wire.requestFrom((int)activeJob.pump->i2cAddr, (int)64);
      CommandResult resp = {false, 0, "NO_DATA"};
      if (rn > 0) {
        uint8_t rStatus = Wire.read();
        String rRaw;
        if (rStatus != 1) rRaw += String("STATUS_") + rStatus + " ";
        while (Wire.available()) { char c = (char)Wire.read(); if (!isReplyChar(c)) break; rRaw += c; }
        if (!rRaw.length()) rRaw = (rStatus == 1) ? "OK" : (String("STATUS_") + rStatus);
        resp = {rStatus == 1, rStatus, rRaw};
      }
      bool accepted = (resp.statusCode == 1 || resp.statusCode == 254);
      if (!accepted) {
        pumpPowerSet(*activeJob.pump, false);
        publishPumpEvent(activeJob.pumpId, activeJob.requestId, "rejected", -1, resp.raw);
        clearActiveJob(true, true, resp.raw);
        break;
      }

      if (isContinuousCommand(activeJob.cmd)) {
        activeJob.state = JOB_CONTINUOUS_RUNNING;
      } else {
        // status=1 here means "command acknowledged/started", NOT "dosing complete"
        // status=254 means "still processing" — both transition to polling state
        uint32_t secs = estimateDoseSeconds(*activeJob.pump, activeJob.cmd);
        activeJob.expectedDoneAt = now + (secs * 1000UL);
        Serial.printf("[SCHED] %s running, done in %lus\n", activeJob.pumpId.c_str(), (unsigned long)secs);
        activeJob.state = JOB_FINITE_RUNNING;
      }
      break;
    }
    case JOB_FINITE_RUNNING: {
      // Timer-based completion — calibrate mlPerSec per pump for accuracy
      if ((long)(now - activeJob.expectedDoneAt) >= 0) {
        Serial.printf("[SCHED] %s done (timer)\n", activeJob.pumpId.c_str());
        pumpPowerSet(*activeJob.pump, false);
        {
          float reqMl = parseDoseMl(activeJob.cmd);
          DoseMeta dose;
          if (reqMl >= 0) { dose.requestedMl = reqMl; dose.estimatedMl = reqMl; dose.resultSource = "estimated"; }
          publishPumpEvent(activeJob.pumpId, activeJob.requestId, "completed", -1, "", dose);
        }
        clearActiveJob(true, false);
      }
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

static void setupOTA() {
  if (WiFi.status() != WL_CONNECTED) return;
  ArduinoOTA.setHostname(mqtt_client_id);
  ArduinoOTA.setPassword(ota_password);
  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] update starting");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("[OTA] update complete");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] error [%u]\n", error);
  });
  ArduinoOTA.begin();
  Serial.println("[OTA] ready");
}

// ---- BATCH COMMAND PROCESSOR ----
// Returns an error string on failure, empty string on success.
// The batch is identified by the request_id of the request that started it.
static String processBatchCommand(const JsonDocument& doc, const String& requestId) {
  if (activeBatch.active) return "ERR:BATCH_ALREADY_ACTIVE";
  if (activeJob.busy)     return "ERR:MOTOR_BUSY";

  const String& batchId = requestId;

  JsonArrayConst cmds = doc["commands"].as<JsonArrayConst>();
  if (cmds.isNull() || cmds.size() == 0) return "ERR:COMMANDS_MISSING_OR_EMPTY";
  if (cmds.size() > BATCH_ITEMS_MAX) return "ERR:TOO_MANY_COMMANDS";

  uint16_t defaultWaitMs = 300;
  bool stopOnError = true;
  if (!doc["options"]["default_wait_ms"].isNull()) defaultWaitMs = doc["options"]["default_wait_ms"].as<uint16_t>();
  if (!doc["options"]["stop_on_error"].isNull())   stopOnError   = doc["options"]["stop_on_error"].as<bool>();

  // Validate all items before accepting
  size_t count = cmds.size();
  for (size_t i = 0; i < count; i++) {
    JsonObjectConst item = cmds[i];
    const char* pid = item["pump_id"];
    const char* cmd = item["cmd"];
    if (!pid || !cmd || strlen(cmd) == 0) return String("ERR:ITEM_INVALID:") + i;
    if (!findPumpById(String(pid)))        return String("ERR:PUMP_NOT_FOUND:") + pid + ":item=" + i;
    if (isContinuousCommand(String(cmd)))  return String("ERR:CONTINUOUS_CMD_NOT_ALLOWED_IN_BATCH:item=") + i;
  }

  // Populate activeBatch
  activeBatch.active       = true;
  activeBatch.stopOnError  = stopOnError;
  activeBatch.batchId      = batchId;
  activeBatch.totalCount   = count;
  activeBatch.currentIndex = 0;
  activeBatch.waitingForMotor = false;

  for (size_t i = 0; i < count; i++) {
    JsonObjectConst item = cmds[i];
    activeBatch.items[i].itemId      = item["item_id"].isNull() ? String(i) : item["item_id"].as<String>();
    activeBatch.items[i].pumpId      = item["pump_id"].as<String>();
    activeBatch.items[i].cmd         = item["cmd"].as<String>();
    activeBatch.items[i].waitMs      = item["wait_ms"].isNull() ? defaultWaitMs : item["wait_ms"].as<uint16_t>();
    activeBatch.items[i].postDelayMs = item["post_delay_ms"].isNull() ? 0 : item["post_delay_ms"].as<uint32_t>();
    activeBatch.items[i].isMotor     = isMotorCommand(activeBatch.items[i].cmd);
  }

  publishBatchEvent(batchId, "accepted");

  // Kick off the first item via advanceBatch (currentIndex starts at 0, we set it to BATCH_ITEMS_MAX
  // as a sentinel so advanceBatch's "advance" step lands on index 0).
  // Actually: set currentIndex=0 then call advanceBatch with publishPriorCompleted=false and
  // lastItemFailed=false — but advanceBatch always increments first. So preset to SIZE_MAX trick won't
  // work cleanly. Instead just call the same inline logic advanceBatch uses, starting at index 0.
  activeBatch.currentIndex = 0;
  // Use the same advance logic as advanceBatch's while loop by treating "before first item" state.
  // Temporarily set currentIndex to a value that, after +1, equals 0 — not possible with size_t.
  // Simplest: directly start item 0 inline (mirrors advanceBatch's while body).
  {
    BatchItem& first = activeBatch.items[0];
    {
      DoseMeta runDose; runDose.requestedMl = parseDoseMl(first.cmd);
      publishBatchItemEvent(batchId, first, 0, "running", "", 0, runDose);
    }

    String fc = first.cmd; fc.trim();
    bool firstIsStop  = fc.equalsIgnoreCase("X");
    bool firstIsPause = fc.equalsIgnoreCase("P");

    if (first.isMotor && !firstIsStop && !firstIsPause) {
      PumpJob job{first.pumpId, batchId + ":" + first.itemId, first.cmd, first.waitMs};
      CommandResult startRes = startMotorJob(job);
      if (!startRes.ok) {
        publishBatchItemEvent(batchId, first, 0, "failed", startRes.raw);
        if (stopOnError) {
          publishBatchEvent(batchId, "aborted", 0, "item_failed");
          activeBatch.active = false;
          return "";
        }
        activeBatch.currentIndex = 1;
        advanceBatch(false);
      } else {
        activeBatch.waitingForMotor = true;
      }
    } else {
      // Non-motor, stop, or pause — run inline
      const PumpConfig* pump = findPumpById(first.pumpId);
      bool ok = false;
      String raw;
      if (!pump) {
        raw = String("ERR:PUMP_NOT_FOUND:") + first.pumpId;
      } else if (firstIsStop) {
        CommandResult r = stopActiveMotorForPump(first.pumpId);
        ok = r.ok; raw = r.raw;
      } else if (firstIsPause) {
        if (!activeJob.busy || !activeJob.pumpId.equalsIgnoreCase(first.pumpId)) {
          raw = "ERR:NO_ACTIVE_MOTOR";
        } else {
          CommandResult r = pumpQuery(*pump, "P", first.waitMs);
          ok = r.ok; raw = r.raw;
        }
      } else {
        CommandResult r = processImmediateCommandForPump(*pump, first.cmd, first.waitMs);
        ok = r.ok; raw = r.raw;
      }
      if (!ok) {
        publishBatchItemEvent(batchId, first, 0, "failed", raw);
        if (stopOnError) {
          publishBatchEvent(batchId, "aborted", 0, "item_failed");
          activeBatch.active = false;
          return "";
        }
      } else {
        publishBatchItemEvent(batchId, first, 0, "completed");
      }
      activeBatch.currentIndex = 1;
      advanceBatch(false);
    }
  }

  return "";
}

// ---- MQTT MESSAGE HANDLER ----
// Every request gets one pump_response on the resp topic: ok, then result or error.
static void publishResponse(JsonDocument& resp) {
  String payload;
  serializeJson(resp, payload);
  mqttClient.publish(mqtt_resp_topic, payload.c_str(), false);
  Serial.print("[MQTT] TX resp="); Serial.println(payload);
}

// type "raw": one EZO-PMP command for one pump.
static void handleRaw(const JsonDocument& doc, const String& requestId, JsonDocument& resp) {
  String pumpId;
  if (!doc["pump_id"].isNull()) pumpId = doc["pump_id"].as<String>();

  String cmd;
  if (!doc["cmd"].isNull()) cmd = doc["cmd"].as<String>();

  uint16_t waitMs = 300;
  if (!doc["wait_ms"].isNull()) waitMs = doc["wait_ms"].as<uint16_t>();

  bool queueIfBusy = true;
  if (!doc["options"]["queue_if_busy"].isNull()) {
    queueIfBusy = doc["options"]["queue_if_busy"].as<bool>();
  }

  bool ok = false;
  uint8_t statusCode = 0;
  String raw;

  const PumpConfig* pump = pumpId.length() ? findPumpById(pumpId) : nullptr;
  if (!pumpId.length()) {
    raw = "ERR:PUMP_ID_MISSING";
  } else if (!pump) {
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
        DoseMeta runDose; runDose.requestedMl = parseDoseMl(cmd);
        publishPumpEvent(pumpId, requestId, "running", -1, "", runDose);
      } else {
        publishPumpEvent(pumpId, requestId, "rejected", -1, startRes.raw);
      }
    }
  }

  beginResponse(resp, requestId, ok, raw);
  if (pumpId.length()) resp["pump_id"] = pumpId;
  if (ok) {
    JsonObject resultObj = resp["result"].to<JsonObject>();
    resultObj["raw"] = raw;
    resultObj["status_code"] = statusCode;
  }
}

// type "batch_abort": stop the running motor and end the active batch.
static void handleBatchAbort(const String& requestId, JsonDocument& resp) {
  if (!activeBatch.active) {
    beginResponse(resp, requestId, false, "no_active_batch");
    return;
  }
  String abortedBatchId = activeBatch.batchId;
  size_t abortedAt = activeBatch.currentIndex;
  // Mark inactive first so clearActiveJob won't try to advance the batch
  activeBatch.active = false;
  activeBatch.waitingForMotor = false;
  activeBatch.waitingPostDelay = false;
  // Stop the running motor immediately
  if (activeJob.busy && activeJob.pump) {
    pumpQuery(*activeJob.pump, "X", 150);
    clearActiveJob(false, false);
  }
  publishBatchEvent(abortedBatchId, "aborted", (int)abortedAt, "abort_requested");

  beginResponse(resp, requestId, true);
  JsonObject resultObj = resp["result"].to<JsonObject>();
  resultObj["batch_request_id"] = abortedBatchId;
  resultObj["aborted_at"] = (int)abortedAt;
}

// type "scan": list the I2C addresses that answer on the bus.
static void handleScan(const String& requestId, JsonDocument& resp) {
  beginResponse(resp, requestId, true);
  resp["result"].to<JsonObject>()["raw"] = scanI2CBus();
}

// type "rename_pump": change a pump's ID in memory only (lost on reboot).
static void handleRenamePump(const JsonDocument& doc, const String& requestId, JsonDocument& resp) {
  String fromId, toId;
  if (!doc["from_id"].isNull()) fromId = doc["from_id"].as<String>();
  if (!doc["to_id"].isNull())   toId   = doc["to_id"].as<String>();

  String err;
  if (!fromId.length() || !toId.length()) {
    err = "ERR:MISSING_FROM_ID_OR_TO_ID";
  } else if (toId.length() >= 24) {
    err = "ERR:TO_ID_TOO_LONG";
  } else {
    PumpConfig* target = nullptr;
    bool conflict = false;
    for (size_t i = 0; i < PUMP_COUNT; i++) {
      if (!target && fromId.equalsIgnoreCase(pumps[i].pumpId)) target = &pumps[i];
      if (toId.equalsIgnoreCase(pumps[i].pumpId)) conflict = true;
    }
    if (!target) {
      err = String("ERR:PUMP_NOT_FOUND:") + fromId;
    } else if (conflict) {
      err = String("ERR:ID_ALREADY_EXISTS:") + toId;
    } else {
      strncpy(target->pumpId, toId.c_str(), 23);
      target->pumpId[23] = '\0';
    }
  }

  beginResponse(resp, requestId, err.length() == 0, err);
  if (!err.length()) {
    JsonObject resultObj = resp["result"].to<JsonObject>();
    resultObj["from_id"] = fromId;
    resultObj["to_id"] = toId;
  }
}

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

  JsonDocument resp;
  JsonDocument doc;
  if (deserializeJson(doc, msg)) {
    beginResponse(resp, "", false, "invalid_json");
    publishResponse(resp);
    return;
  }

  String requestId;
  if (!doc["request_id"].isNull()) requestId = doc["request_id"].as<String>();

  // 'type' selects the handler.
  String type;
  if (!doc["type"].isNull()) type = doc["type"].as<String>();

  if (type.equalsIgnoreCase("raw")) {
    handleRaw(doc, requestId, resp);
  } else if (type.equalsIgnoreCase("batch")) {
    String err = processBatchCommand(doc, requestId);
    beginResponse(resp, requestId, err.length() == 0, err);
  } else if (type.equalsIgnoreCase("batch_abort")) {
    handleBatchAbort(requestId, resp);
  } else if (type.equalsIgnoreCase("scan")) {
    handleScan(requestId, resp);
  } else if (type.equalsIgnoreCase("rename_pump")) {
    handleRenamePump(doc, requestId, resp);
  } else {
    beginResponse(resp, requestId, false, "unknown_type");
  }
  publishResponse(resp);
}

// ---- MQTT CONNECT/RECONNECT ----
static bool mqttConnectOnce() {
  mqttClient.setServer(mqtt_broker, mqtt_port);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setKeepAlive(20);
  mqttClient.setBufferSize(1024);

  // Build LWT offline payload
  String lwtPayload = "{\"v\":1,\"type\":\"controller_status\",\"controller_id\":\"";
  lwtPayload += mqtt_client_id;
  lwtPayload += "\",\"state\":\"offline\"}";

  bool ok = mqttClient.connect(
    mqtt_client_id, mqtt_user, mqtt_pass,
    mqtt_status_topic, 0, true, lwtPayload.c_str()
  );
  // bool ok = mqttClient.connect(mqtt_client_id);

  if (!ok) {
    Serial.print("[MQTT] connect failed, state="); Serial.println(mqttClient.state());
    return false;
  }

  mqttClient.subscribe(mqtt_cmd_topic);
  publishControllerStatus("online", /*retained=*/true);

  Serial.println("[MQTT] connected & subscribed");
  return true;
}

static void ensureMqtt() {
  if (mqttClient.connected()) return;
  static unsigned long lastAttemptMs = 0;
  unsigned long now = millis();
  if (now - lastAttemptMs < 5000) return;
  lastAttemptMs = now;
  mqttConnectOnce();
}

// ---- ARDUINO LIFECYCLE ----
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\nStarting ESP32 Pump Controller...");

  Wire.begin(I2C_SDA, I2C_SCL);
  // 50 kHz, not the usual 100 kHz. The EZO-PMP circuits cannot finish a reply at 100 kHz (the
  // text is cut off after two characters) but answer correctly at 90 kHz and below. They are
  // powered from the Feather's 3V pin and report about 3.07 V, under their 3.3 V minimum.
  Wire.setClock(50000);
  Serial.printf("I2C init SDA=%d SCL=%d clock=50kHz\n", I2C_SDA, I2C_SCL);
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
  setupOTA();
  ensureMqtt();
  publishControllerStatus("ready");
  Serial.println("Multi-pump bridge ready. Raw commands target default pump_1; JSON supports pump_id routing.");
}

static unsigned long lastHeartbeatMs = 0;
static const unsigned long HEARTBEAT_INTERVAL_MS = 30000;

void loop() {
  serviceMotorScheduler();

  // Service post-step mixing delay between batch items
  if (activeBatch.active && activeBatch.waitingPostDelay) {
    if ((long)(millis() - activeBatch.postDelayUntil) >= 0) {
      activeBatch.waitingPostDelay = false;
      advanceBatch(false, false, "");
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.handle();
    if (!mqttClient.connected()) ensureMqtt();
    mqttClient.loop();

    // Periodic heartbeat
    unsigned long now = millis();
    if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
      lastHeartbeatMs = now;
      publishControllerStatus("alive");
    }
  }

  delay(2);
}
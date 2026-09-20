
# ESP32 Motor Power Scheduling & Execution Model

## Purpose
This document defines how the ESP32 should manage pump motor operations when constrained by a limited 12V power supply (e.g., 1A). It ensures safe, scalable control of multiple pumps using a single ESP32 and multiple Infineon PROFET boards.

---

## Core Constraint

- Each pump motor draws ~400mA at 12V
- A 1A power supply cannot safely support multiple simultaneous pumps

👉 Only **ONE pump motor operation may run at a time**

---

## Design Approach

The ESP32 acts as a **scheduler**, not just a command forwarder.

---

## Command Classification

### Motor Commands (Require Power Budget)
- D,<mL>
- DC,<...>
- DS,<...>
- R (run continuously)
- X (stop)
- P (pause)

### Non-Motor Commands (No Power Constraint)
- i
- PV,?
- Status
- Cal,?
- Name,?

---

## Execution Model

### Single Active Motor Slot

```cpp
struct ActiveMotorJob {
  bool busy;
  String pumpId;
  String requestId;
  unsigned long startedAt;
  unsigned long expectedDoneAt;
};
```

---

### Queue System

```cpp
struct PumpJob {
  String pumpId;
  String cmd;
  uint16_t waitMs;
  String requestId;
};
```

---

## Execution Flow

1. MQTT message received  
2. Parse JSON  
3. Resolve pump_id → hardware config  
4. If motor command:
   - If busy → queue or reject  
   - If free → execute immediately  
5. If non-motor command:
   - Execute immediately  

---

## Motor Job Lifecycle

States:
- queued
- running
- completed
- rejected

---

## MQTT Event Messages

### Queued
```json
{
  "type": "pump_event",
  "pump_id": "pump_3",
  "request_id": "req-123",
  "state": "queued",
  "queue_position": 2
}
```

### Running
```json
{
  "type": "pump_event",
  "pump_id": "pump_3",
  "request_id": "req-123",
  "state": "running"
}
```

### Completed
```json
{
  "type": "pump_event",
  "pump_id": "pump_3",
  "request_id": "req-123",
  "state": "completed"
}
```

### Rejected
```json
{
  "type": "pump_event",
  "pump_id": "pump_3",
  "request_id": "req-123",
  "state": "rejected",
  "reason": "motor_busy"
}
```

---

## JSON Command Extension

```json
{
  "v": 1,
  "pump_id": "pump_2",
  "cmd": "D,5.0",
  "wait_ms": 300,
  "request_id": "req-555",
  "options": {
    "queue_if_busy": true
  }
}
```

---

## Hardware Mapping (Internal Only)

```json
{
  "pump_1": { "i2c": "0x67", "board": 0, "channel": 1 },
  "pump_2": { "i2c": "0x68", "board": 0, "channel": 2 },
  "phup":   { "i2c": "0x6B", "board": 1, "channel": 1 }
}
```

---

## Power Control Abstraction

```cpp
void setPumpPower(uint8_t board, uint8_t channel, bool on);
```

---

## Controller State (Minimal)

```cpp
bool motorBusy = false;
String activePumpId = "";
```

---

## Recommended Behavior

- Only one pump motor active at a time  
- Motor commands default to queue  
- Non-motor commands always allowed  
- "X" command interrupts active job immediately  

---

## Implementation Notes

- Avoid long blocking delays  
- Use millis()-based timing instead of delay()  
- Keep MQTT responsive  
- Publish state transitions  

---

## Future Enhancements

- Priority queue  
- Max queue length  
- Timeout handling  
- Parallel execution (if power upgraded)  
- Telemetry  

---

## Version

v1 - Single-motor scheduling model

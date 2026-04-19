
# MQTT JSON Messaging Context for ESP32 Pump Controller (v2)

## Purpose
Defines JSON communication between Mycodo and an ESP32 controlling:
- N pumps
- Multiple Infineon PROFET boards (4 channels each)

Supports scalability across:
- multiple pumps
- multiple power controllers
- future expansion

---

## Architecture

Mycodo → MQTT → ESP32 → 
    ├── I2C → EZO Pumps
    └── GPIO → Infineon Boards (power switching)

---

## Key Design Concept

A pump is defined as:

pump = (i2c + board + channel)

---

## Design Principles

1. JSON = transport + routing  
2. Atlas commands = unchanged  
3. ESP32 owns all hardware mapping  
4. Mycodo only uses pump_id  
5. System must scale without schema changes  

---

## Minimal Schema

```json
{
  "v": 1,
  "pump_id": "pump_1",
  "cmd": "D,5.0",
  "wait_ms": 300,
  "request_id": "req-001"
}
```

---

## Full Schema (Future-Proof)

```json
{
  "v": 1,
  "type": "pump_command",
  "target": {
    "pump_id": "pump_1"
  },
  "command": {
    "mode": "atlas_raw",
    "value": "D,5.0"
  },
  "options": {
    "wait_ms": 300,
    "expect_response": true
  },
  "meta": {
    "request_id": "req-001",
    "source": "mycodo"
  }
}
```

---

## Pump Registry (ESP32)

```json
{
  "pump_1": { "i2c": "0x67", "board": 0, "channel": 1 },
  "pump_2": { "i2c": "0x68", "board": 0, "channel": 2 },
  "pump_5": { "i2c": "0x6B", "board": 1, "channel": 1 }
}
```

---

## ESP32 Responsibilities

- Resolve pump_id → hardware config
- Route I2C commands
- Control correct Infineon board + channel
- Handle timing, retries, parsing
- Manage power switching

---

## Internal Structures (Suggested)

```cpp
struct PumpConfig {
  uint8_t i2c;
  uint8_t board;
  uint8_t channel;
};
```

---

## Power Control Abstraction

```cpp
void setPumpPower(uint8_t board, uint8_t channel, bool on);
```

---

## Message Types

- pump_command
- pump_query
- bridge_command

---

## Example Messages

### Dose
```json
{
  "pump_id": "pump_1",
  "cmd": "D,5.0"
}
```

### Stop
```json
{
  "pump_id": "pump_1",
  "cmd": "X"
}
```

---

## Response Schema

```json
{
  "v": 1,
  "type": "pump_response",
  "pump_id": "pump_1",
  "request_id": "req-001",
  "result": {
    "ok": true,
    "status_code": 1,
    "raw": "?D,OK"
  }
}
```

---

## MQTT Topics

- esp32/pump/cmd
- esp32/pump/resp
- esp32/pump/status

---

## Key Takeaways

- Mycodo stays simple
- ESP32 handles complexity
- Supports unlimited pumps (with added boards)
- No schema changes required as system grows

---

## Future Enhancements

- Batch commands
- Scheduling
- Safety limits (max pumps active)
- Telemetry streaming
- Structured commands (non-raw)

---

## Version

v2 - Multi-board, multi-pump architecture

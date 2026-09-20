# CLAUDE.md

## Purpose

This is the ESP32 pump controller firmware. It runs on an Adafruit Feather ESP32 v2 and acts as the hardware execution layer for the hydroponic dosing system. It receives commands from Mycodo or grow-ui via MQTT and drives Atlas EZO-PMP peristaltic pumps over I2C through Infineon PROFET high-side switches.

At a high level, this firmware is responsible for:

- connecting to WiFi and the MQTT broker
- subscribing to `esp32/pump/cmd` and routing commands by `pump_id`
- controlling pump motor power via PROFET GPIO before and after each I2C transaction
- running a single-motor scheduler with a bounded job queue
- executing `pump_batch` sequences with per-step delays and error handling
- publishing granular events back to `esp32/pump/resp` after each step

This repo is the hardware execution layer. It does not compute nutrient amounts, own any UI, or contain business logic. It executes what it is told.

## How This Repo Relates to the Other Repos

- **grow-ui / Mycodo repo:** computes doses, builds batch commands, publishes to `esp32/pump/cmd`
- **This repo:** receives commands, drives hardware, publishes events to `esp32/pump/resp`

The shared boundary is the MQTT message contract. Changes to batch schema, event naming, or `pump_id` values must be coordinated across repos. Do not move GPIO, I2C, or PROFET details into the Mycodo or grow-ui repos — those belong here exclusively.

## Tech Stack

- C++ / Arduino framework
- Adafruit Feather ESP32 v2 (240MHz, ~520KB free heap)
- PubSubClient 2.8 — MQTT client
- ArduinoJson 7 — JSON parsing and serialization
- PlatformIO build system (`pio run`, `pio run -t upload`)

## Hardware

### Pumps
- **Atlas EZO-PMP** peristaltic pumps controlled over I2C
- I2C bus: SDA = GPIO 22, SCL = GPIO 20, 100kHz clock
- Each pump has a unique I2C address and a PROFET channel for 12V motor power

### PROFET Boards (Infineon high-side switches)
- Board 0 channels 1–4: GPIO 32, 33, 27, 25
- Board 1 channels 1–2: GPIO 26, 13
- Pump power is switched ON before sending the I2C command and OFF after the motor job completes

### Pump Registry (hardcoded in `main.cpp`)

| pump_id | I2C addr | Board | Channel | Description |
|---|---|---|---|---|
| `calmag` | 0x64 | 0 | 1 | Cal-Mag |
| `micro` | 0x65 | 0 | 2 | FloraMicro |
| `gro` | 0x66 | 0 | 3 | FloraGro |
| `bloom` | 0x68 | 0 | 4 | FloraBloom |
| `phup` | 0x69 | 1 | 1 | pH Up |
| `phdown` | 0x6A | 1 | 2 | pH Down |

To add a pump: add an entry to the `pumps[]` array and increment `PUMP_COUNT`. All other code looks up pumps by `pump_id` string via `findPumpById()` (case-insensitive).

## MQTT Topics

| Topic | Direction | Purpose |
|---|---|---|
| `esp32/pump/cmd` | inbound | Pump commands and batch sequences |
| `esp32/pump/resp` | outbound | Events: pump_event, batch_event, batch_item_event |
| `esp32/pump/status` | outbound | Controller heartbeat (retained) |

## Command Formats

### JSON single command (preferred)
```json
{
  "v": 1,
  "pump_id": "calmag",
  "cmd": "D,5.00",
  "wait_ms": 300,
  "request_id": "req-001",
  "options": { "queue_if_busy": true }
}
```

### Batch command
```json
{
  "v": 1,
  "type": "pump_batch",
  "batch_id": "nutrient-abc123",
  "options": { "stop_on_error": true },
  "commands": [
    { "item_id": "step-1", "pump_id": "calmag", "cmd": "D,138.04", "post_delay_ms": 1200000 },
    { "item_id": "step-2", "pump_id": "micro",  "cmd": "D,46.20",  "post_delay_ms": 150000 },
    { "item_id": "step-3", "pump_id": "gro",    "cmd": "D,46.20",  "post_delay_ms": 90000 },
    { "item_id": "step-4", "pump_id": "bloom",  "cmd": "D,46.20",  "post_delay_ms": 90000 }
  ]
}
```

### Key Atlas EZO-PMP commands

| Command | Meaning |
|---|---|
| `D,<mL>` | Dose exact volume |
| `DS,<mL>,<min>` | Dose volume over time |
| `DC,<mL/min>` | Dose continuously at rate |
| `R` | Run continuously |
| `X` | Stop immediately (returns `*DONE,<mL>`) |
| `P` | Pause |
| `i` | Device info |

## Motor Scheduler

Only one motor can run at a time — the 1A 12V power supply cannot sustain multiple motors simultaneously. This is a hard hardware constraint, not an optimization.

### State machine
```
IDLE → SETTLING (1s power stabilize)
     → WAITING_RESPONSE (send I2C, wait waitMs ~300ms)
     → FINITE_RUNNING (timer until estimated completion) → IDLE
     → CONTINUOUS_RUNNING (wait for explicit X stop)
```

### Duration estimation
Completion time is estimated as `mL / pump.mlPerSec` — the scheduler does NOT poll the pump during a run. Accuracy depends on the `mlPerSec` calibration per pump. If mis-calibrated, the job will complete early or late relative to actual dispensing.

### Motor queue
Up to 8 jobs can be queued. With `queue_if_busy: true`, incoming jobs queue behind the active motor. The next job starts automatically when the active one completes.

## Batch Execution

1. **Validation** — all `pump_id` values must exist; continuous commands (`R`, `DC,`, `D,*`) are rejected because they have no natural completion signal and would stall the batch
2. **Sequential execution** — motor items hand off to the scheduler; non-motor items (info, calibration, status) execute inline immediately without waiting
3. **Post-delay** — `post_delay_ms` per item is a mixing/settling wait observed after item completion before advancing
4. **Error handling** — `stop_on_error: true` aborts remaining items on first failure; `false` continues

Maximum 16 items per batch. Only one batch active at a time.

## Event Publishing

All events publish to `esp32/pump/resp` (non-retained):

- **`pump_event`** — state: `queued | running | completed | rejected` — per motor job
- **`batch_event`** — state: `accepted | completed | aborted` — per batch lifecycle
- **`batch_item_event`** — state: `running | completed | failed | post_delay` — per batch step

After a motor item finishes, two `batch_item_event` messages fire in sequence: `completed` then `post_delay`. This lets subscribers distinguish "motor done" from "now waiting before next step."

## Secrets and Config

Credentials live in `include/secrets.h` (gitignored). Copy `include/secrets.h.example` to `include/secrets.h` and fill in WiFi SSID/password and MQTT username/password before building. The build will fail with a `#error` if the file is missing.

All other runtime config (broker IP, pump registry, pin mapping) is hardcoded in `main.cpp`. There is no runtime configuration — changes require a recompile and flash.

## Build and Monitor

```bash
# Build
pio run

# Build and upload
pio run -t upload

# Serial monitor (115200 baud)
pio device monitor -b 115200

# Watch pump events
mosquitto_sub -h 10.0.0.232 -u mqttclient -P '<pass>' -t 'esp32/pump/resp' -v

# Manual test command
mosquitto_pub -h 10.0.0.232 -u mqttclient -P '<pass>' -t 'esp32/pump/cmd' \
  -m '{"v":1,"pump_id":"calmag","cmd":"i","wait_ms":300,"request_id":"test-1"}'
```

## Gotchas and Conventions

### 1. Timer-based completion, not polled
The scheduler trusts `estimateDoseSeconds()`. There is no I2C polling mid-run. Calibrate `mlPerSec` per pump for accurate timing.

### 2. Continuous commands are forbidden in batches
`R`, `DC,`, and `D,*` have no completion signal — the batch would stall. Validation rejects any batch containing these. This is intentional.

### 3. Stop (X) and Pause (P) in a batch are special
They target the currently active motor inline — they are not scheduled. They stop whatever is running right now.

### 4. Post-delay does not block the motor queue
The mixing delay between batch steps is a timer only. The motor scheduler can service other queued jobs during that window, though in practice a nutrient batch owns the full session.

### 5. pump_id lookup is case-insensitive
`findPumpById("CalMag")` and `findPumpById("calmag")` both work.

### 6. Reported vs. estimated dose volume
When stopped with `X`, the pump returns `*DONE,<mL>` — the actual dispensed volume. For timer-based completion, only the estimated volume is available. `DoseMeta` tracks which source is authoritative (`pump_reported` vs `estimated`).

### 7. Single-threaded Arduino loop
No RTOS. The motor scheduler, batch processor, and MQTT loop all run sequentially each iteration with a 2ms delay. A blocking I2C call or long MQTT callback stalls everything.

### 8. Rename pump is runtime-only
The `bridge_command` type supports `rename_pump` which mutates `pump.pumpId` at runtime. This is lost on reboot.

### 9. pumpId in activeJob is a String, not indexed
Batch items get `requestId = batchId + ":" + itemId` for correlation between pump events and batch item events on the subscriber side.

### 10. LWT publishes offline status automatically
If the device drops unexpectedly, the broker publishes `{"state":"offline"}` to `esp32/pump/status`. Mycodo/grow-ui can use this to detect hardware unavailability.

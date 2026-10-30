# CLAUDE.md

## Purpose

Firmware for the pump controller in a hydroponic dosing system. It runs on an Adafruit
Feather ESP32 v2 and is the hardware execution layer: it receives JSON commands over MQTT
and drives Atlas EZO-PMP peristaltic pumps over I2C, switching each motor's 12 V power
through Infineon PROFET high-side switches.

It does not compute nutrient amounts, own a UI, or hold business logic. It executes what
it is told and reports what happened.

## Who talks to it

- **grow-ui** (Flask app on the Pi) sends the day-to-day nutrient batches and manual commands.
- **Mycodo** (on the same Pi) listens to the events and also has a pump output; event-driven
  dosing from Mycodo is planned.
- The shared boundary is the MQTT contract: topics, JSON schema, `pump_id` names, batch
  semantics. A change to any of those must land in this firmware, grow-ui and the Mycodo
  side together. GPIO, I2C and PROFET details stay in this repo only.

## Stack

C++ / Arduino framework, PubSubClient 2.8 (MQTT), ArduinoJson 7, PlatformIO. Everything is
in `src/main.cpp`.

## Hardware

- **I2C:** SDA = GPIO 22, SCL = GPIO 20, 100 kHz
- **PROFET board 0, channels 1-4:** GPIO 32, 33, 27, 25
- **PROFET board 1, channels 1-2:** GPIO 26, 13
- A pump's motor power is switched on before its I2C command and off after the job ends.

### Pump registry (the `pumps[]` array in `main.cpp`)

| `pump_id` | I2C | Board / channel | Description |
|---|---|---|---|
| `calmag` | 0x64 | 0 / 1 | Cal-Mag |
| `micro` | 0x65 | 0 / 2 | FloraMicro |
| `gro` | 0x66 | 0 / 3 | FloraGro |
| `bloom` | 0x68 | 0 / 4 | FloraBloom |
| `phup` | 0x69 | 1 / 1 | pH Up |
| `phdown` | 0x6A | 1 / 2 | pH Down |

To add a pump, add an entry to `pumps[]` and the rest of the code finds it through
`findPumpById()` (case-insensitive). Then update `grow-ui/config.py` (`SYSTEMS`) and the
Mycodo pump options. Each entry also carries `mlPerSec`, currently `1.0` for every pump.

## MQTT

| Topic | Direction | Purpose |
|---|---|---|
| `esp32/pump/cmd` | in | `raw`, `batch`, `batch_abort`, `scan`, `rename_pump` |
| `esp32/pump/resp` | out | `pump_response`, `pump_event`, `batch_event`, `batch_item_event` (not retained) |
| `esp32/pump/status` | out | `controller_status`: `online` (retained), `ready`, `alive` every 30 s, last will `offline` |

Every message carries `"v": 1`, and requests select their handler with `type`. Requests
carry a caller-assigned `request_id` that is echoed back; a batch is identified by its
`request_id`, and its motor jobs are correlated as `<request_id>:<item_id>`. Every request
gets one `pump_response` with `ok` and either `result` or an `error` code. Anything that is
not valid JSON or has an unknown `type` gets an error reply. The shared rules are in
`docs/mqtt_conventions.md`; the full contract is in `docs/mqtt_contract.md`.

Useful Atlas EZO-PMP commands: `D,<mL>` dose, `DS,<mL>,<min>` dose over time, `DC,<mL/min>`
continuous rate, `R` run, `X` stop (returns `*DONE,<mL>`), `P` pause, `i` info.

## Motor scheduler

Only one motor can run at a time. The 12 V supply cannot sustain two, so this is a hard
hardware limit, not an optimization.

```
IDLE -> SETTLING (1 s power stabilize)
     -> WAITING_RESPONSE (send I2C, wait ~300 ms)
     -> FINITE_RUNNING (timer to estimated completion) -> IDLE
     -> CONTINUOUS_RUNNING (until an explicit X)
```

- **Completion is estimated, not measured.** Finish time is `mL / mlPerSec`; the scheduler
  never polls the pump mid-run. Accuracy depends on calibrating `mlPerSec` per pump.
- **Queue:** up to 8 jobs. With `queue_if_busy: true` a new job queues behind the active one.

## Batches

1. **Validation:** every `pump_id` must exist, at most 16 items, and continuous commands
   (`R`, `DC,`) are rejected because they have no completion signal and would stall the batch.
2. **Execution:** motor items go to the scheduler; non-motor items (info, calibration,
   status) run inline.
3. **Post-delay:** `post_delay_ms` is a mixing wait observed after an item completes. It is
   a timer only and does not block the motor queue.
4. **Errors:** `stop_on_error: true` aborts the remaining items on the first failure.

Only one batch is active at a time. Step timing belongs to the firmware, so a batch of
20+ minutes of mixing delays does not depend on the Pi staying up.

### Events

- `pump_event`: `queued | running | completed | rejected`, per motor job
- `batch_event`: `accepted | completed | aborted`, per batch
- `batch_item_event`: `running | completed | failed | post_delay`, per step

After a motor item finishes, `completed` fires and then `post_delay`, so a subscriber can
tell "motor done" from "waiting before the next step."

## Secrets and config

WiFi, MQTT and OTA credentials live in `include/secrets.h` (gitignored). Copy
`include/secrets.h.example` and fill it in; the build stops with a `#error` if it is
missing. The broker IP, pump registry and pins are compile-time constants in `main.cpp`,
so changing any of them means a rebuild and reflash.

## Build, flash, monitor

```bash
pio run                       # build
pio run -t upload             # flash over USB
pio run -e ota -t upload      # flash over WiFi (set OTA_PASSWORD in the shell first)
pio device monitor -b 115200  # serial monitor
```

The OTA target is `esp32-pump-01.local`.

## Gotchas

1. **Do not present a completion event as proof of delivery.** When stopped with `X`, the
   pump returns the real dispensed volume; for timer completion only an estimate exists.
   `DoseMeta` tracks which one is authoritative (`pump_reported` vs `estimated`). Keep
   requested, reported and estimated volumes separate.
2. **Stop (`X`) and pause (`P`) in a batch act inline** on whatever motor is running now;
   they are not scheduled.
3. **Single-threaded loop.** There is no RTOS. The scheduler, batch processor and MQTT loop
   run in sequence with a 2 ms delay, so a blocking I2C call or a long MQTT callback stalls
   everything.
4. **`rename_pump` is runtime-only.** The `rename_pump` request can rename a pump, but the
   change is lost on reboot.
5. **The last will matters.** If the device drops, the broker publishes
   `{"state":"offline"}` to `esp32/pump/status`; clients can use that to detect the
   controller being unavailable.

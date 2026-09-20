# ESP32 Pump Controller

Firmware for the peristaltic dosing pumps in a hydroponic nutrient system. It runs on an
Adafruit Feather ESP32 v2, receives JSON commands over MQTT, and drives Atlas EZO-PMP
pumps over I2C through Infineon PROFET high-side switches.

This is the hardware execution layer. It does not compute nutrient doses and has no UI —
it executes what it is told and reports what happened.

## What it does

- Subscribes to `esp32/pump/cmd` and routes commands by logical `pump_id`
- Switches 12 V motor power per pump via PROFET before and after each I2C transaction
- Runs a single-motor scheduler with a bounded job queue (one motor at a time)
- Executes multi-step `pump_batch` sequences with per-step mixing delays
- Publishes granular events to `esp32/pump/resp` and a retained heartbeat to `esp32/pump/status`
- Supports OTA firmware updates over WiFi

## Hardware

- **Board:** Adafruit Feather ESP32 v2
- **Pumps:** 6× Atlas EZO-PMP over I2C (SDA 22, SCL 20, 100 kHz)
- **Power switching:** Infineon PROFET boards — board 0 on GPIO 32/33/27/25, board 1 on GPIO 26/13
- **Supply:** 1 A 12 V, which is why only one motor may run at a time

| `pump_id` | Role | I2C | Board / channel |
|---|---|---|---|
| `calmag` | Cal-Mag | 0x64 | 0 / 1 |
| `micro` | FloraMicro | 0x65 | 0 / 2 |
| `gro` | FloraGro | 0x66 | 0 / 3 |
| `bloom` | FloraBloom | 0x68 | 0 / 4 |
| `phup` | pH Up | 0x69 | 1 / 1 |
| `phdown` | pH Down | 0x6A | 1 / 2 |

The pump registry is hardcoded in `src/main.cpp`. Everything else looks pumps up by name.

## Setup

Requires [PlatformIO](https://platformio.org/).

```bash
git clone https://github.com/Abiel5/esp32-pump-controller.git
cd esp32-pump-controller
cp include/secrets.h.example include/secrets.h
```

Fill in `include/secrets.h` with your WiFi SSID and password, MQTT username and password,
and an OTA password. The file is gitignored; the build fails with a clear `#error` if it
is missing.

The broker IP, pump registry and pin mapping are compile-time constants in `src/main.cpp`.
There is no runtime configuration — changing any of them means a rebuild and reflash.

## Build and flash

```bash
pio run                      # build
pio run -t upload            # flash over USB
pio device monitor -b 115200 # serial monitor
```

Once OTA firmware is on the device, you can flash over WiFi:

```bash
export OTA_PASSWORD=your-ota-password   # PowerShell: $env:OTA_PASSWORD = "..."
pio run -e ota -t upload
```

The OTA target is `esp32-pump-01.local`. If mDNS does not resolve on your network, replace
`upload_port` in `platformio.ini` with the device's IP address.

## MQTT

| Topic | Direction | Purpose |
|---|---|---|
| `esp32/pump/cmd` | in | Single commands and batches |
| `esp32/pump/resp` | out | `pump_event`, `batch_event`, `batch_item_event` |
| `esp32/pump/status` | out | Heartbeat (retained), plus an `offline` last will |

Single command:

```json
{ "v": 1, "pump_id": "calmag", "cmd": "D,5.00", "wait_ms": 300, "request_id": "req-001" }
```

Batch (up to 16 items, executed in order, `post_delay_ms` is a mixing wait after each step):

```json
{
  "v": 1,
  "type": "pump_batch",
  "batch_id": "nutrient-abc123",
  "options": { "stop_on_error": true },
  "commands": [
    { "item_id": "step-1", "pump_id": "calmag", "cmd": "D,138.04", "post_delay_ms": 600000 },
    { "item_id": "step-2", "pump_id": "micro",  "cmd": "D,46.20",  "post_delay_ms": 150000 }
  ]
}
```

Test from the Pi:

```bash
mosquitto_sub -h 10.0.0.232 -u mqttclient -P '<pass>' -t 'esp32/pump/resp' -v
mosquitto_pub -h 10.0.0.232 -u mqttclient -P '<pass>' -t 'esp32/pump/cmd' \
  -m '{"v":1,"pump_id":"calmag","cmd":"i","wait_ms":300,"request_id":"test-1"}'
```

Dose completion is estimated from `mL / mlPerSec`, not measured — the scheduler does not
poll the pump mid-run. Calibrate `mlPerSec` per pump for accurate timing, and do not treat
a completion event as proof of delivery.

## Related projects

Part of a four-repo hydroponics system:

- [grow-ui](https://github.com/Abiel5/grow-ui) — web control panel, sends the batches
- [Mycodo fork](https://github.com/Abiel5/Mycodo) — automation and scheduling, also sends batches
- [esp32-sensor-controller](https://github.com/Abiel5/esp32-sensor-controller) — pH, EC and temperature

## Documentation

- `CLAUDE.md` — architecture, scheduler state machine, batch rules, conventions
- `docs/mqtt_pump_controller_context_v2.md` — message contract
- `docs/batch_commands.md` — batch semantics and events
- `docs/esp32_motor_scheduler_context.md` — motor scheduling design
- `docs/EZO_PMP_Datasheet.pdf` — Atlas pump command reference

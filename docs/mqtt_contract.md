# Pump MQTT Contract

Message contract between the ESP32 pump controller and its clients on the Pi (grow-ui and
Mycodo). Clients speak only in logical `pump_id` names; the firmware owns every I2C address,
PROFET board and channel.

All messages are JSON and carry `"v": 1`. Pump IDs are matched case-insensitively.

| Topic | Direction | Purpose |
|---|---|---|
| `esp32/pump/cmd` | client → ESP32 | Commands |
| `esp32/pump/resp` | ESP32 → client | Command responses and events (not retained) |
| `esp32/pump/status` | ESP32 → client | `controller_status` heartbeat |

Pump IDs: `calmag`, `micro`, `gro`, `bloom`, `phup`, `phdown`.

---

## Message types

The `type` field selects the handler. A message with no `type` is a single pump command.

| `type` | Purpose | Reply |
|---|---|---|
| *(none)* / `pump_command` | One Atlas EZO-PMP command | `pump_response` |
| `pump_batch` | Several commands executed in order | `batch_response`, then events |
| `batch_abort` | Stop the active batch | `batch_abort_response` |
| `bridge_command` | Controller actions: `scan`, `rename_pump` | `bridge_response` |

A message that is not JSON is treated as a legacy raw ASCII command for the first pump.

---

## Single command

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

| Field | Required | Default | Notes |
|---|---|---|---|
| `pump_id` | yes | none | An unknown ID returns `ERR:PUMP_NOT_FOUND:<id>` |
| `cmd` | yes | none | Atlas EZO-PMP command string |
| `wait_ms` | no | `300` | I2C wait before reading the reply |
| `request_id` | no | none | Echoed in the response and events |
| `options.queue_if_busy` | no | `true` | Queue behind a running motor instead of rejecting |

### Response

```json
{ "v": 1, "type": "pump_response", "pump_id": "calmag", "request_id": "req-001",
  "result": { "ok": true, "status_code": 1, "raw": "RUNNING" } }
```

For a motor command `raw` is `RUNNING` (started) or `QUEUED`; the outcome arrives later as
`pump_event` messages. Non-motor commands (`i`, `Status`, `Cal,?`, ...) run immediately and
return the pump's reply in `raw`.

Errors appear in `raw`: `ERR:PUMP_NOT_FOUND:<id>`, `ERR:CMD_MISSING`,
`ERR:MOTOR_BUSY active=<id>`, `ERR:QUEUE_FULL`, `ERR:NO_ACTIVE_MOTOR`,
`ERR:ACTIVE_MOTOR_OTHER:<id>`.

### Atlas EZO-PMP commands

| Command | Meaning |
|---|---|
| `D,<mL>` | Dose an exact volume |
| `DS,<mL>,<min>` | Dose a volume over a time |
| `DC,<mL/min>` | Dose continuously at a rate |
| `R` | Run continuously |
| `X` | Stop immediately (returns `*DONE,<mL>`) |
| `P` | Pause |

`X` and `P` act inline on the motor that is running now and are not scheduled.

### Motor job events

Published as `pump_event` on `esp32/pump/resp` for each motor job:

```json
{ "v": 1, "type": "pump_event", "pump_id": "calmag", "request_id": "req-001",
  "state": "completed", "requested_ml": 5.0, "estimated_ml": 5.0, "result_source": "estimated" }
```

`state` is `queued` (with `queue_position`), `running`, `completed` or `rejected` (with
`reason`). Volumes are kept separate on purpose: `requested_ml` is what was asked,
`estimated_ml` comes from the timer, and `reported_ml` is what the pump itself returned after
an `X`. `result_source` is `estimated` or `pump_reported`. Completion is timer-based, so
treat it as an estimate, never as measured delivery.

---

## Batch

```json
{
  "v": 1,
  "type": "pump_batch",
  "batch_id": "nutrient-abc123",
  "options": { "stop_on_error": true, "default_wait_ms": 300 },
  "commands": [
    { "item_id": "step-1", "pump_id": "calmag", "cmd": "D,138.04", "post_delay_ms": 600000 },
    { "item_id": "step-2", "pump_id": "micro",  "cmd": "D,46.20",  "post_delay_ms": 150000 }
  ]
}
```

| Field | Required | Default | Notes |
|---|---|---|---|
| `batch_id` | no | empty | Echoed in every event |
| `options.stop_on_error` | no | `true` | Abort the remaining items on the first failure |
| `options.default_wait_ms` | no | `300` | I2C wait for items that omit `wait_ms` |
| `commands` | yes | none | 1 to 16 items |
| `commands[].pump_id` | yes | none | Must exist |
| `commands[].cmd` | yes | none | EZO-PMP command; continuous ones are rejected |
| `commands[].item_id` | no | index | Echoed in item events |
| `commands[].wait_ms` | no | `default_wait_ms` | Per-item I2C wait |
| `commands[].post_delay_ms` | no | `0` | Mixing wait after the item completes |

### Acceptance

The reply is a `batch_response`:

```json
{ "v": 1, "type": "batch_response", "batch_id": "nutrient-abc123", "ok": true }
```

On rejection, before anything runs, `ok` is `false` and `reason` is one of
`ERR:BATCH_ALREADY_ACTIVE`, `ERR:MOTOR_BUSY`, `ERR:COMMANDS_MISSING_OR_EMPTY`,
`ERR:TOO_MANY_COMMANDS`, `ERR:ITEM_INVALID:<index>`, `ERR:PUMP_NOT_FOUND:<id>:item=<index>`
or `ERR:CONTINUOUS_CMD_NOT_ALLOWED_IN_BATCH:item=<index>`.

All items are validated before the batch is accepted. Continuous commands (`R`, `DC,`) are
rejected because they have no completion signal and would stall the batch.

### Execution

- Items run in array order, one motor at a time. The batch waits for each motor job to
  finish before advancing.
- Non-motor items (`i`, `Status`, `Cal,?`, ...) run inline and complete immediately.
- `post_delay_ms` is a timer only; it does not block the motor queue.
- Only one batch can be active, and a batch is refused while a motor job is running.
- The firmware sequences everything itself, so a batch with 20+ minutes of mixing delays does
  not depend on the Pi staying connected. Batches do not survive a reboot of the ESP32.

### Events

Published on `esp32/pump/resp`.

`batch_event`, with `state` of `accepted`, `completed` or `aborted`:

```json
{ "v": 1, "type": "batch_event", "batch_id": "nutrient-abc123", "state": "accepted", "count": 2 }
{ "v": 1, "type": "batch_event", "batch_id": "nutrient-abc123", "state": "aborted",
  "reason": "item_failed", "failed_index": 1 }
```

`batch_item_event`, with `state` of `running`, `completed`, `failed` or `post_delay`:

```json
{ "v": 1, "type": "batch_item_event", "batch_id": "nutrient-abc123", "item_id": "step-1",
  "pump_id": "calmag", "cmd": "D,138.04", "state": "post_delay", "index": 0,
  "post_delay_ms": 600000 }
```

After a motor item finishes, `completed` fires and then `post_delay`, so a subscriber can
tell "motor done" from "waiting before the next step." Items that finish dosing also carry
`requested_ml`, `estimated_ml`, `reported_ml` and `result_source` as above. A `failed` item
carries a `reason`.

### Abort

```json
{ "v": 1, "type": "batch_abort" }
```

Stops the running motor with `X`, ends the batch and publishes a `batch_event` with
`state: "aborted"` and `reason: "abort_requested"`. The reply is a `batch_abort_response`:
`{"ok": true, "batch_id": "...", "aborted_at": 1}`, or `{"ok": false, "reason": "no_active_batch"}`.

---

## Bridge commands

```json
{ "v": 1, "type": "bridge_command", "action": "scan", "request_id": "req-9" }
{ "v": 1, "type": "bridge_command", "action": "rename_pump", "from_id": "gro", "to_id": "grow" }
```

`scan` lists the I2C devices on the bus. `rename_pump` changes a pump's ID **in memory only**;
it is lost on reboot, so the permanent name is the `pumps[]` entry in `src/main.cpp`. The
reply is a `bridge_response` with a `result` object like a `pump_response`.

---

## Controller status

Published to `esp32/pump/status`:

| `state` | When | Extra fields |
|---|---|---|
| `online` | After connecting (retained) | `wifi`, `mqtt`, `ip`, `uptime_sec` |
| `ready` | Once setup finishes | none |
| `alive` | Every 30 s | `uptime_sec`, `free_heap`, `wifi_rssi`, `active_batch`, `motor_busy` |
| `offline` | Last will, published by the broker if the device drops | none |

```json
{ "v": 1, "type": "controller_status", "controller_id": "esp32-pump-01", "state": "alive",
  "uptime_sec": 12345, "free_heap": 210000, "wifi_rssi": -55,
  "active_batch": false, "motor_busy": false }
```

---

## Compatibility notes

- A change to message shapes, event names or `pump_id` values must land in the firmware,
  grow-ui and the Mycodo side together.
- Do not move hardware details (I2C addresses, GPIO, PROFET channels) out of the firmware.
- Only one motor can run at a time; the 12 V supply cannot sustain two.

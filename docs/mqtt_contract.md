# Pump MQTT Contract

Message contract between the ESP32 pump controller and its clients on the Pi (grow-ui and
Mycodo). Clients speak only in logical `pump_id` names; the firmware owns every I2C address,
PROFET board and channel.

All messages are JSON and carry `"v": 1`. Pump IDs are matched case-insensitively. The
envelope (request, reply, error and heartbeat shapes) follows `mqtt_conventions.md`, which is
shared with the sensor controller.

| Topic | Direction | Purpose |
|---|---|---|
| `esp32/pump/cmd` | client → ESP32 | Requests |
| `esp32/pump/resp` | ESP32 → client | `pump_response` replies and events (not retained) |
| `esp32/pump/status` | ESP32 → client | `controller_status` heartbeat |

Pump IDs: `calmag`, `micro`, `gro`, `bloom`, `phup`, `phdown`.

---

## Request types

`type` selects the handler. A request without a known `type`, or a payload that is not
JSON, gets a `pump_response` with `error` set to `unknown_type` or `invalid_json`.

| `type` | Purpose |
|---|---|
| `raw` | One Atlas EZO-PMP command for one pump |
| `batch` | Several commands executed in order |
| `batch_abort` | Stop the active batch |
| `scan` | List the I2C devices on the bus |
| `rename_pump` | Rename a pump in memory |

Every request is answered by one `pump_response`. Progress after that arrives as events.

---

## Replies

```json
{ "v": 1, "type": "pump_response", "request_id": "req-001", "pump_id": "calmag",
  "ok": true, "result": { "raw": "RUNNING", "status_code": 1 } }

{ "v": 1, "type": "pump_response", "request_id": "req-001", "pump_id": "gro",
  "ok": false, "error": "pump_not_found", "message": "ERR:PUMP_NOT_FOUND:gro" }
```

`ok` says whether the request was accepted. On success `result` holds the outcome; on
failure `error` is a snake-case code and `message` may carry more detail (never parse it).

### Error codes

| `error` | When |
|---|---|
| `invalid_json` | The payload was not valid JSON. |
| `unknown_type` | `type` is missing or not one of the request types. |
| `pump_id_missing` | A `raw` request had no `pump_id`. |
| `pump_not_found` | The `pump_id` is not in the registry. |
| `cmd_missing` | A `raw` request had no `cmd`. |
| `motor_busy` | Another motor is running and `queue_if_busy` is false (or the request is a batch). |
| `queue_full` | The motor queue (8 jobs) is full. |
| `no_active_motor` | `X` or `P` with no motor running. |
| `active_motor_other` | `X` or `P` for a pump other than the one running. |
| `i2c_error` | The I2C transfer failed; `message` has the code. |
| `no_data` | The pump returned nothing. |
| `pump_error` | The pump replied with a non-OK status; `message` has the reply. |
| `pinmap_error` | The pump's PROFET channel is not mapped. |
| `batch_already_active` | A batch is already running. |
| `commands_missing_or_empty` | A batch had no `commands`. |
| `too_many_commands` | A batch had more than 16 items. |
| `item_invalid` | A batch item lacks `pump_id` or `cmd`; `message` has the item index. |
| `continuous_cmd_not_allowed_in_batch` | A batch item is `R` or `DC,...`; `message` has the item index. |
| `no_active_batch` | `batch_abort` with no batch running. |
| `missing_from_id_or_to_id`, `to_id_too_long`, `id_already_exists` | `rename_pump` validation. |

---

## `raw`: one pump command

```json
{
  "v": 1,
  "type": "raw",
  "pump_id": "calmag",
  "cmd": "D,5.00",
  "wait_ms": 300,
  "request_id": "req-001",
  "options": { "queue_if_busy": true }
}
```

| Field | Required | Default | Notes |
|---|---|---|---|
| `pump_id` | yes | none | |
| `cmd` | yes | none | Atlas EZO-PMP command string |
| `wait_ms` | no | `300` | I2C wait before reading the reply |
| `request_id` | no | none | Echoed in the reply and in the events for this job |
| `options.queue_if_busy` | no | `true` | Queue behind a running motor instead of rejecting |

For a motor command `result.raw` is `RUNNING` (started) or `QUEUED`; the outcome arrives
later as `pump_event` messages. Non-motor commands (`i`, `Status`, `Cal,?`, ...) run
immediately and return the pump's reply in `result.raw`. The firmware also handles the pseudo
commands `POWERON` and `POWEROFF`, which switch a pump's PROFET channel for manual testing.

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

`state` is `queued` (with `queue_position`), `running`, `completed` or `rejected` (with a
`reason` code and, when there is more detail, a `message`). Volumes are kept separate on
purpose: `requested_ml` is what was asked, `estimated_ml` comes from the timer, and
`reported_ml` is what the pump itself returned after an `X`. `result_source` is `estimated`
or `pump_reported`. Completion is timer-based, so treat it as an estimate, never as
measured delivery.

---

## `batch`

```json
{
  "v": 1,
  "type": "batch",
  "request_id": "nutrient-abc123",
  "options": { "stop_on_error": true, "default_wait_ms": 300 },
  "commands": [
    { "item_id": "step-1", "pump_id": "calmag", "cmd": "D,138.04", "post_delay_ms": 600000 },
    { "item_id": "step-2", "pump_id": "micro",  "cmd": "D,46.20",  "post_delay_ms": 150000 }
  ]
}
```

The batch is identified by its `request_id`. There is no separate batch id: every batch
event carries that `request_id`.

| Field | Required | Default | Notes |
|---|---|---|---|
| `request_id` | recommended | none | Identifies the batch in every event |
| `options.stop_on_error` | no | `true` | Abort the remaining items on the first failure |
| `options.default_wait_ms` | no | `300` | I2C wait for items that omit `wait_ms` |
| `commands` | yes | none | 1 to 16 items |
| `commands[].pump_id` | yes | none | Must exist |
| `commands[].cmd` | yes | none | EZO-PMP command; continuous ones are rejected |
| `commands[].item_id` | no | index | Echoed in item events |
| `commands[].wait_ms` | no | `default_wait_ms` | Per-item I2C wait |
| `commands[].post_delay_ms` | no | `0` | Mixing wait after the item completes |

### Acceptance

The reply is a `pump_response` with `ok: true`, or `ok: false` and an `error` code
(`batch_already_active`, `motor_busy`, `commands_missing_or_empty`, `too_many_commands`,
`item_invalid`, `pump_not_found`, `continuous_cmd_not_allowed_in_batch`) before anything runs.

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
{ "v": 1, "type": "batch_event", "request_id": "nutrient-abc123", "state": "accepted", "count": 2 }
{ "v": 1, "type": "batch_event", "request_id": "nutrient-abc123", "state": "aborted",
  "reason": "item_failed", "failed_index": 1 }
```

`batch_item_event`, with `state` of `running`, `completed`, `failed` or `post_delay`:

```json
{ "v": 1, "type": "batch_item_event", "request_id": "nutrient-abc123", "item_id": "step-1",
  "pump_id": "calmag", "cmd": "D,138.04", "state": "post_delay", "index": 0,
  "post_delay_ms": 600000 }
```

After a motor item finishes, `completed` fires and then `post_delay`, so a subscriber can
tell "motor done" from "waiting before the next step." Items that finish dosing also carry
`requested_ml`, `estimated_ml`, `reported_ml` and `result_source` as above. A `failed` item
carries a `reason` code and, when there is more detail, a `message`.

The `pump_event` messages for a batch's motor jobs carry `request_id` in the form
`<batch request_id>:<item_id>`.

---

## `batch_abort`

```json
{ "v": 1, "type": "batch_abort", "request_id": "ui-ab12cd34" }
```

Stops the running motor with `X`, ends the batch and publishes a `batch_event` with
`state: "aborted"` and `reason: "abort_requested"`. The reply is a `pump_response` whose
`result` is `{"batch_request_id": "...", "aborted_at": 1}`, or `ok: false` with
`error: "no_active_batch"`.

---

## `scan` and `rename_pump`

```json
{ "v": 1, "type": "scan", "request_id": "req-9" }
{ "v": 1, "type": "rename_pump", "from_id": "gro", "to_id": "grow", "request_id": "req-10" }
```

`scan` lists the I2C devices on the bus in `result.raw` (for example `I2C:0x64,0x65`).
`rename_pump` changes a pump's ID **in memory only**; it is lost on reboot, so the permanent
name is the `pumps[]` entry in `src/main.cpp`. Its `result` is `{"from_id": "...", "to_id": "..."}`.

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

- There is one form of each message. The older request shapes (a command with no `type`,
  `pump_batch`, `bridge_command`, a separate `batch_id`, the nested `target` and `command`
  objects, and raw ASCII text) are no longer accepted.
- A change to message shapes, event names or `pump_id` values must land in the firmware,
  grow-ui and the Mycodo side together.
- Do not move hardware details (I2C addresses, GPIO, PROFET channels) out of the firmware.
- Only one motor can run at a time; the 12 V supply cannot sustain two.

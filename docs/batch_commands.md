# ESP32 Batch Command Support

## Overview

The ESP32 firmware supports a `pump_batch` message type that lets a single MQTT message trigger multiple pump operations executed sequentially, one motor command at a time.

**MQTT topics:**
- Command in: `esp32/pump/cmd`
- Events out: `esp32/pump/resp`

---

## Request Schema

```json
{
  "v": 1,
  "type": "pump_batch",
  "batch_id": "batch-001",
  "options": {
    "stop_on_error": true,
    "default_wait_ms": 300
  },
  "commands": [
    { "item_id": "step-1", "pump_id": "pump_1", "cmd": "D,5.0" },
    { "item_id": "step-2", "pump_id": "pump_2", "cmd": "D,3.0", "wait_ms": 500 },
    { "item_id": "step-3", "pump_id": "pump_3", "cmd": "i" }
  ]
}
```

### Fields

| Field | Required | Default | Description |
|---|---|---|---|
| `type` | yes | â€” | Must be `"pump_batch"` |
| `batch_id` | no | `""` | Echoed in all events |
| `options.stop_on_error` | no | `true` | Abort remaining items on first failure |
| `options.default_wait_ms` | no | `300` | I2C wait used when item omits `wait_ms` |
| `commands` | yes | â€” | Array of 1â€“16 items |
| `commands[].pump_id` | yes | â€” | Must match a configured pump |
| `commands[].cmd` | yes | â€” | EZO-PMP command string |
| `commands[].item_id` | no | index | Echoed in item events |
| `commands[].wait_ms` | no | `default_wait_ms` | Per-item I2C wait override |

---

## Immediate Response

On receipt, the firmware immediately publishes a `batch_response` ACK:

```json
{ "v": 1, "type": "batch_response", "batch_id": "batch-001", "ok": true }
```

On rejection (before any items run):

```json
{ "v": 1, "type": "batch_response", "batch_id": "batch-001", "ok": false, "reason": "ERR:BATCH_ALREADY_ACTIVE" }
```

Possible rejection reasons: `ERR:BATCH_ALREADY_ACTIVE`, `ERR:MOTOR_BUSY`, `ERR:COMMANDS_MISSING_OR_EMPTY`, `ERR:TOO_MANY_COMMANDS`, `ERR:ITEM_INVALID:<index>`, `ERR:PUMP_NOT_FOUND:<id>:item=<index>`.

---

## Event Stream

All events publish to `esp32/pump/resp`.

### Batch accepted
```json
{ "v": 1, "type": "batch_event", "batch_id": "batch-001", "state": "accepted", "count": 3 }
```

### Item started
```json
{ "v": 1, "type": "batch_item_event", "batch_id": "batch-001", "item_id": "step-1", "pump_id": "pump_1", "cmd": "D,5.0", "state": "running", "index": 0 }
```

### Item completed
```json
{ "v": 1, "type": "batch_item_event", "batch_id": "batch-001", "item_id": "step-1", "pump_id": "pump_1", "cmd": "D,5.0", "state": "completed", "index": 0 }
```

### Item failed
```json
{ "v": 1, "type": "batch_item_event", "batch_id": "batch-001", "item_id": "step-2", "pump_id": "pump_2", "cmd": "D,3.0", "state": "failed", "reason": "STATUS_2 ...", "index": 1 }
```

### Batch completed
```json
{ "v": 1, "type": "batch_event", "batch_id": "batch-001", "state": "completed", "count": 3 }
```

### Batch aborted (stop_on_error)
```json
{ "v": 1, "type": "batch_event", "batch_id": "batch-001", "state": "aborted", "reason": "item_failed", "failed_index": 1, "count": 3 }
```

---

## Behavior

### Execution model
- All items execute in array order â€” no parallel execution.
- Motor commands (`D,`, `DS,`, `DC,`, `R`, `P`, `X`) go through the existing single-motor scheduler. The batch waits for the scheduler to signal completion before advancing.
- Non-motor commands (e.g. `i`, `Status`, `Cal,?`, `POWERON`, `POWEROFF`) execute inline without entering the queue, so they complete immediately and the batch advances in the same loop tick.

### Constraints
- Only **one batch** may be active at a time. A second batch is rejected with `ERR:BATCH_ALREADY_ACTIVE`.
- If a motor job is already running when the batch arrives, the batch is rejected with `ERR:MOTOR_BUSY`. Retry when the active job completes.
- Maximum **16 items** per batch (`BATCH_ITEMS_MAX`).

### Error handling
- `stop_on_error: true` (default): first item failure cancels all remaining items and emits `batch_event.state = "aborted"`.
- `stop_on_error: false`: failed item is reported and the batch continues to the next item.

### Validation
All items are validated before the batch is accepted. If any `pump_id` is unknown, any `cmd` is missing, or any `cmd` is a continuous command (`R`, `D,*`, `DC,`), the whole batch is rejected before any command executes. Continuous commands would stall the batch indefinitely since there is no reliable completion signal for them.

---

## Minimal Example (Mycodo dispense to three pumps)

```json
{
  "v": 1,
  "type": "pump_batch",
  "batch_id": "batch-xyz",
  "options": { "stop_on_error": true },
  "commands": [
    { "pump_id": "pump_1", "cmd": "D,5.0" },
    { "pump_id": "pump_2", "cmd": "D,5.0" },
    { "pump_id": "pump_3", "cmd": "D,5.0" }
  ]
}
```

---

## Not Supported (deferred)

- Queuing multiple batches (second batch must wait and retry)
- Batch pause / resume
- Conditionals or retries within a batch
- Persistence across reboot



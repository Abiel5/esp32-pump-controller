# MQTT Message Conventions

Rules shared by every ESP32 controller in this system (pumps, sensors, and any future
device such as an outlet controller). Each controller's own contract document describes its
commands; this document describes the envelope they all use. The same file lives in both
firmware repos and must be kept identical.

Controllers have different jobs, so only the envelope is standardized: the shape of
requests, replies, events and heartbeats. What a pump or a sensor actually does is not.

---

## Topics

Each controller owns three topics, named after the device kind:

| Topic | Direction | Purpose |
|---|---|---|
| `esp32/<device>/cmd` | client → ESP32 | Requests |
| `esp32/<device>/resp` | ESP32 → client | Replies and events (not retained) |
| `esp32/<device>/status` | ESP32 → client | Heartbeat, and telemetry the device pushes on its own |

`<device>` is `pump`, `sensors`, and so on.

## Every message

- JSON object, with `"v": 1`.
- `type` names the kind of message (see below).
- `ts` (optional) is UTC Unix seconds, for devices that have a synced clock.

## Requests

```json
{ "v": 1, "type": "<verb>", "<kind>_id": "<name>", "cmd": "<device command>",
  "request_id": "ui-ab12cd34" }
```

| Field | Rule |
|---|---|
| `type` | The verb. Selects the handler. |
| `<kind>_id` | The target, as a logical name: `pump_id`, `sensor_id`, `outlet_id`. Matched case-insensitively. Hardware details (addresses, pins) never appear. |
| `cmd` | The device's own command string, passed through, and **only** that. It is never the verb. |
| `request_id` | Chosen by the caller and echoed in every reply and event for that request. Callers use a prefix plus 8 hex characters (`ui-`, `mycodo-`). |

Other fields are specific to the verb (for example `post_delay_ms` on a pump batch).

A request that starts something long-running, such as a batch, is identified by its
`request_id`. There is no separate batch id on the wire: the `batch_event` and
`batch_item_event` messages carry the batch's `request_id`, and `item_id` for the step. The
event `type` says what kind of job it is. (Today the pump still uses `batch_id`; see below.)

### Standard verbs

A verb means the same thing on every controller that supports it. A controller implements
only the verbs that make sense for its hardware.

| `type` | Meaning | Used by |
|---|---|---|
| `raw` | Pass the device's own command string (`cmd`) straight through to a target | pumps, sensors (`ph`, `ec`) |
| `read` | Read one target and return its value | sensors |
| `read_all` | Read every target; answered with a telemetry push | sensors |
| `batch` | Run several commands in order, with per-step delays | pumps |
| `batch_abort` | Stop the active batch | pumps |
| `scan` | List the devices found on the controller's bus | pumps |
| `rename_pump` | Rename a target in memory (lost on reboot) | pumps |

Controller-specific verbs are allowed (a future outlet controller might add `set`), but a
verb that already exists above keeps its meaning. Friendlier verbs such as a pump `dose`
may be added later; `raw` stays as the passthrough.

## Replies

Every request gets exactly one reply on `resp`, named `<kind>_response`:

```json
{ "v": 1, "type": "sensor_response", "request_id": "ui-ab12cd34", "sensor_id": "ph",
  "ok": true, "result": { "value": 7.513 } }

{ "v": 1, "type": "sensor_response", "request_id": "ui-ab12cd34", "sensor_id": "ph",
  "ok": false, "error": "read_failed" }
```

| Field | Rule |
|---|---|
| `ok` | Boolean. Did the request succeed. |
| `result` | Object with the outcome, present when `ok` is true. |
| `error` | Snake-case code, present when `ok` is false (`unknown_sensor`, `pump_not_found`). |
| `message` | Optional human-readable detail. Never parse it. |

Unknown input is never ignored. These codes are common to every controller:

| `error` | Meaning |
|---|---|
| `invalid_json` | The payload was not valid JSON. |
| `unknown_type` | `type` (or the legacy `cmd` verb) is not one this controller handles. |

An error reply to input that carried no `request_id` omits it.

## Events

Anything with a lifecycle (a motor running, a batch advancing) publishes `<kind>_event`
messages on `resp` with a `state` field, echoing the `request_id` that started it.

## Telemetry

Values a device pushes on its own go to `status` with their own `type`, for example
`sensor_reading`. A client that reads `status` branches on `type`.

## Heartbeat

Every controller publishes `controller_status` on `status`:

```json
{ "v": 1, "type": "controller_status", "controller_id": "esp32-sensor-01",
  "state": "alive", "uptime_sec": 368475, "free_heap": 226960, "wifi_rssi": -52 }
```

| `state` | When |
|---|---|
| `online` | After connecting to the broker (retained) |
| `ready` | Once setup has finished |
| `alive` | Every 30 s |
| `offline` | Last will, published by the broker if the device drops |

Devices may add fields of their own to `alive` (the pump adds `motor_busy` and
`active_batch`).

## Compatibility

- Controllers keep accepting the older request forms while clients migrate, and keep the
  older reply fields next to the new ones. Today that means the verb in `cmd` instead of
  `type`, and `status`, `value`, `air_temp`, `air_hum` and `raw_response` at the top level
  of sensor replies.
- Once every client uses the typed forms, the older fields are removed and `v` becomes `2`.
- A change to the envelope must land in every firmware repo, grow-ui and the Mycodo side
  together.

## Status of each controller

| Controller | Conforms |
|---|---|
| Sensors | Yes. Typed requests and replies, `ok`/`error`, error replies for unknown input. Older forms still accepted. |
| Pumps | Partly. Uses `type`, `<kind>_response` and events. Still differs in four ways: a single command has no `type` (target: `raw`; `pump_command` is accepted today), the batch verb is `pump_batch` (target: `batch`), a batch is identified by `batch_id` (target: its `request_id`), errors are `ERR:` strings in `result.raw` instead of `ok`/`error`, and non-JSON is treated as a legacy raw command. |

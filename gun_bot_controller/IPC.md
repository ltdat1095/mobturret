# gun_bot_controller IPC — A55 side

This file mirrors the canonical A55 ↔ M33 RPMsg contract at
[`../IPC.md`](../IPC.md). Read that first for the wire format,
message IDs, payload layouts, and versioning rules.

This file is the **A55-side implementation map**: how the A55 Linux
userland talks to the M33, which messages it sends, and any A55-side
behaviour the canonical spec doesn't capture.

## Outbound (A55 → M33)

| `msg_id` | Name | Source | Notes |
|---:|---|---|---|
| `0x0001` | `MSG_SET_TARGET` | vision (AUTO mode) **or** mobile input (MANUAL mode) | At most 50 Hz; rate-limited to M33's slew rate |
| `0x0002` | `MSG_FIRE` | mobile fire button **or** vision trigger (AUTO mode with confidence threshold) | Honour M33's 250 ms hardware cooldown; check `MSG_ACK` |
| `0x0003` | `MSG_MODE` | mobile `cmd/mode` topic | Persist to local config; also published to AWS |
| `0x0004` | `MSG_HEARTBEAT` | `ipc/heartbeat.c` at 1 Hz | Starts once M33 heartbeat is observed |
| `0x0005` | `MSG_CONFIG` | mobile `config` topic | `video_capture_enabled` flag mirrors DynamoDB |
| `0x0006` | `MSG_EMERGENCY_STOP` | local e-stop input **or** `cmd/mode = SAFE` from cloud | Wait for `MSG_ACK` before re-arming |
| `0x0007` | `MSG_PING` | liveness check | 1 Hz; `MSG_PONG` within 100 ms expected |

## Inbound (M33 → A55)

| `msg_id` | Name | Sink | Notes |
|---:|---|---|---|
| `0x0081` | `MSG_TELEMETRY` | `net/mqtt_publish.c` → `turret/{robot_id}/telemetry` | At 10 Hz from M33; rate-limited at MQTT layer if needed |
| `0x0082` | `MSG_ACK` | request correlator | Match `orig_msg_id` to pending command |
| `0x0083` | `MSG_ALERT` | `vision/event_handler.c`; `ALERT_LOW_VOLT` → MQTT `evt/alert`; `ALERT_STALL` → mobile push | See `code` table in `../IPC.md` |
| `0x0084` | `MSG_PONG` | liveness | Log RTT; alert if RTT > 100 ms |
| `0x0085` | `MSG_SERVO_DIAG` | local diagnostics log | Per-servo health dump |

## A55-specific notes

- **Boot sequencing.** A55 starts the M33 firmware via remoteproc
  (see `../gun_bot/CLAUDE.md`), then opens the RPMsg endpoint. The
  first heartbeat received from M33 triggers the A55 heartbeat
  loop. Until then, no commands are sent (we'd just queue anyway).
- **Buffering.** Local outbound queue per `msg_id`, max 4 frames,
  drop-oldest on overflow. Inbound queue from M33 is processed by a
  single dedicated thread (no concurrent access).
- **Watchdog.** If no inbound frame (heartbeat or otherwise) from
  M33 within 3 s, publish `evt/alert` with code `HOST_WATCHDOG` and
  set local mode to `SAFE`.
- **Video capture gate.** Before issuing an `MSG_FIRE` in AUTO mode,
  check that `video_capture_enabled` is set in the local config;
  if so, ring-buffer the last 10 s of H.264 from the VPU and upload
  via pre-signed S3 URL on `MSG_ALERT` arrival (see `design.md`
  §4.2 step 4).

## Versioning

When `../IPC.md` bumps `ver`, update the `PROTOCOL_VER` constant in
`ipc/protocol.h` and rebuild. The A55 must reply with
`ACK_UNSUPPORTED` to any inbound frame whose `ver` is higher than
its own.

# gun_bot IPC — M33 side

This file mirrors the canonical A55 ↔ M33 RPMsg contract at
[`../IPC.md`](../IPC.md). Read that first for the wire format,
message IDs, payload layouts, and versioning rules.

This file is the **M33-side implementation map**: which messages the
M33 firmware currently handles, which it sends, and any M33-specific
behaviour the canonical spec doesn't capture.

## Inbound (A55 → M33)

| `msg_id` | Name | M33 handler | Notes |
|---:|---|---|---|
| `0x0001` | `MSG_SET_TARGET` | `on_set_target()` | Slew rate-limited per servo (planned); clamped to safe yaw/pitch range |
| `0x0002` | `MSG_FIRE` | `on_fire()` | Enforces 250 ms hardware cooldown between shots |
| `0x0003` | `MSG_MODE` | `on_mode()` | `MODE_SAFE` zeroes all servo torque |
| `0x0004` | `MSG_HEARTBEAT` | `rpmsg_rx_thread` | Resets watchdog timer |
| `0x0005` | `MSG_CONFIG` | `on_config()` | Persists to non-volatile storage on next reboot |
| `0x0006` | `MSG_EMERGENCY_STOP` | `on_estop()` | Always FLAG_ACK; cancels in-flight `MSG_FIRE`; safe pose |
| `0x0007` | `MSG_PING` | `on_ping()` | Replies with `MSG_PONG` using the same `seq` |

Unknown `msg_id` → reply `MSG_ACK` with `status = ACK_UNSUPPORTED`.
Malformed payload → reply with `status = ACK_BAD_PAYLOAD`,
`detail = offset of first invalid byte`.

## Outbound (M33 → A55)

| `msg_id` | Name | Cadence | Notes |
|---:|---|---|---|
| `0x0081` | `MSG_TELEMETRY` | 10 Hz | Filled from servo feedback + on-board sensors |
| `0x0082` | `MSG_ACK` | on demand | Replies to any FLAG_ACK message |
| `0x0083` | `MSG_ALERT` | event-driven | See `code` values in `../IPC.md` |
| `0x0084` | `MSG_PONG` | on demand | Carries the `seq` of the triggering `MSG_PING` |
| `0x0085` | `MSG_SERVO_DIAG` | on stall / 1 Hz | Per-servo health dump |

## M33-specific notes

- **Boot state:** M33 advertises a `MSG_HEARTBEAT` once per second
  from `main()` until it sees a host heartbeat. Once a heartbeat is
  received, the watchdog timer starts; `ALERT_WATCHDOG` fires after
  3 s of silence.
- **Hardware cooldown:** `MSG_FIRE` requests that arrive within
  250 ms of a previous accepted fire are rejected with
  `MSG_ACK { status = ACK_BUSY }`.
- **Safe pose:** on `MSG_EMERGENCY_STOP` or `MSG_MODE = MODE_SAFE`,
  yaw/pitch servos move to `SAFE_YAW_CENTIDEG` / `SAFE_PITCH_CENTIDEG`
  (defined in `src/servo/safe_pose.h`).
- **Servo discovery:** the SC15 ping at boot uses broadcast ID `0xFE`
  at 1 Mbaud. Discovery runs once and populates `servo_id_table[]`;
  re-discovery is triggered by `MSG_CONFIG` flag bit 1 (reserved for
  this — see `../IPC.md` versioning rules).

## Versioning

When `../IPC.md` bumps `ver`, update the `PROTOCOL_VER` constant in
`src/ipc.h` and rebuild. The M33 must reply with `ACK_UNSUPPORTED` to
any inbound frame whose `ver` is higher than its own.

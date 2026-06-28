# IPC — A55 ↔ M33 RPMsg Contract

**Canonical wire format for the inter-core channel between
`gun_bot_controller/` (A55 / Linux / host) and `gun_bot/` (M33 /
Zephyr / remote).**

If you change anything here, bump `ver` and update the mirrored files
at `gun_bot/IPC.md` and `gun_bot_controller/IPC.md`.

## Transport

- **Framework:** RPMsg over OpenAMP (i.MX93 supports it natively).
- **Endpoints:** two virtio-backed RPMsg channels — `turret_host` (A55,
  source) and `turret_remote` (M33, sink). Notifications via hardware
  mailbox interrupts; payload over shared-memory ring buffers.
- **Ordering:** best-effort in-order within a single endpoint; receivers
  must tolerate drops and replay by `seq`. No retransmit at the RPMsg
  layer — application-level `MSG_ACK` handles that for messages with
  `FLAG_ACK` set.

## Frame format

Little-endian, 4-byte aligned, packed:

```
offset  size  field
------  ----  -------------------------------------------------
  0       2   msg_id    (u16) — message type
  2       1   ver       (u8)  — protocol version (currently 1)
  3       1   flags     (u8)  — bit 0 = FLAG_ACK, bits 1-7 reserved
  4       2   length    (u16) — payload size in bytes, 0..256
  6       2   reserved  (u16) — must be zero
  8       4   seq       (u32) — monotonic per-sender counter
 12       N   payload   (length bytes)
 12+N     2   crc16     (u16) — CRC-16/CCITT-FALSE over bytes 0..12+N-1
 14+N     pad to 4-byte alignment (zero bytes)
```

- `msg_id` 0x0000 and 0xFFFF are reserved (invalid / no-op).
- `seq` starts at 1 from each side after boot; the receiver tracks the
  last-seen `seq` per source and flags gaps in `MSG_ALERT` (code
  `ALERT_SEQ_GAP`).
- `crc16` polynomial 0x1021, init 0xFFFF, no reflection, no xor-out.
- Header is 12 bytes; minimum frame is 16 bytes (header + empty payload
  + CRC, already 4-byte aligned).

## Message IDs

Split into two ranges by direction. A55 never sends IDs ≥ 0x0080; M33
never sends IDs < 0x0080. This lets a receiver sanity-check direction.

### A55 → M33 (`MSG_*_HOST_TO_REMOTE`)

| ID | Name | Payload |
|---:|---|---|
| `0x0001` | `MSG_SET_TARGET` | yaw (i16 centi-deg), pitch (i16 centi-deg) |
| `0x0002` | `MSG_FIRE` | duration_ms (u16), pressure (u8, 0–100), reserved (u8) |
| `0x0003` | `MSG_MODE` | mode (u8: `MODE_MANUAL`=0, `MODE_AUTO`=1, `MODE_SAFE`=2), reserved ×3 |
| `0x0004` | `MSG_HEARTBEAT` | empty |
| `0x0005` | `MSG_CONFIG` | flags (u32): bit 0 = video_capture_enabled, bits 1–31 reserved |
| `0x0006` | `MSG_EMERGENCY_STOP` | empty (always FLAG_ACK) |
| `0x0007` | `MSG_PING` | empty (always FLAG_ACK) |

### M33 → A55 (`MSG_*_REMOTE_TO_HOST`)

| ID | Name | Payload |
|---:|---|---|
| `0x0081` | `MSG_TELEMETRY` | yaw_pos (i16 centi-deg), pitch_pos (i16 centi-deg), temp_c (i8), batt_mv (u16 LE), flags (u8), reserved ×3 |
| `0x0082` | `MSG_ACK` | orig_msg_id (u16), status (u8), detail (u16) |
| `0x0083` | `MSG_ALERT` | code (u16 LE), param (u32) |
| `0x0084` | `MSG_PONG` | empty |
| `0x0085` | `MSG_SERVO_DIAG` | servo_id (u8), pos (u16 LE), load (u16 LE), voltage_mv (u16 LE), temp_c (i8), reserved ×3 |

### `MSG_ACK.status` values

| Value | Meaning |
|---:|---|
| `0` | `ACK_OK` |
| `1` | `ACK_BUSY` — try again later |
| `2` | `ACK_BAD_PAYLOAD` — `detail` = offset of first invalid byte |
| `3` | `ACK_UNSUPPORTED` — `msg_id` not implemented on this build |
| `4` | `ACK_FAULT` — hardware fault, detail = subsystem code |

### `MSG_ALERT.code` values

| Value | Meaning | `param` |
|---:|---|---|
| `0x0001` | `ALERT_LOW_VOLT` | threshold in mV |
| `0x0002` | `ALERT_OVERCURRENT` | peak mA |
| `0x0003` | `ALERT_OVERTEMP` | measured °C ×100 |
| `0x0004` | `ALERT_STALL` | servo_id |
| `0x0005` | `ALERT_SEQ_GAP` | last seq seen |
| `0x0006` | `ALERT_WATCHDOG` | ms since last host heartbeat |

## Behaviour rules

- **Boot:** M33 advertises `MSG_HEARTBEAT` once per second until it
  sees a host heartbeat; A55 starts sending `MSG_HEARTBEAT` at 1 Hz
  once it sees the first remote heartbeat. Both sides treat absence of
  any inbound frame for >3 s as `ALERT_WATCHDOG`.
- **Set target:** `MSG_SET_TARGET` is **advisory** — the M33 may
  rate-limit servo slewing; centi-degree precision is sufficient.
- **Fire:** `MSG_FIRE` arms the trigger. `duration_ms` is the hold
  time; M33 enforces a hardware cooldown of 250 ms between shots
  regardless of A55 cadence.
- **Emergency stop:** clears any in-flight `MSG_FIRE`, drives all
  servos to a safe pose, and ignores `MSG_SET_TARGET` until a new
  `MSG_MODE` arrives.
- **Heartbeats:** never carry payload; receivers do not send `MSG_ACK`
  for them.

## Versioning

`ver` starts at `1`. Bump when:
- a new mandatory field is added to an existing message;
- the meaning of an existing field changes;
- a new `MSG_ALERT.code` or `MSG_ACK.status` value is repurposed.

Adding new `msg_id`s at unused IDs, or new flag bits in the reserved
range, does **not** require a version bump — but the older side must
respond with `ACK_UNSUPPORTED` (or `MSG_ALERT` for unsolicited) rather
than silently ignoring.

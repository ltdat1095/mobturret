# MobTurret

An intelligent, dual-core serverless IoT Nerf Turret: a FRDM-iMX93
edge robot running YOLOv8-based human detection on Linux, with a
Zephyr-controlled servo blaster on the M33 coprocessor, talking to an
AWS serverless backend and a Flutter mobile control app.

`design.md` is the authoritative architecture document. This file is
the entry point — read it first, then dive into the relevant
subproject.

> **What to read first:**
> - `PLAN.md` — phases & milestones (what we're building, in what order)
> - `INFRASTRUCTURE.md` — runtime topology (where everything runs, what protocols connect them)
> - `design.md` — architecture (what the system is)
> - `IPC.md` — A55 ↔ M33 wire contract (how the cores talk)
> - Subproject `CLAUDE.md` — build & run for the part you're touching

---

## Architecture

Three pillars (see `design.md` §1 for the full diagram):

| Pillar | Hardware / Stack | Responsibilities |
|---|---|---|
| **Edge robot — A55 (Linux)** | `gun_bot_controller/` (planned) | YOLOv8 detection at 30 FPS, RGB-D ingest, 3D vector calc, MQTT → AWS IoT Core, RTSP streaming, Manual-mode TCP server |
| **Edge robot — M33 (Zephyr)** | `gun_bot/` | Real-time servo + actuator control; consumes A55 commands over RPMsg |
| **Cloud** | `server/` (planned) | CloudFormation (`server/aws/`) + Go Lambdas (`server/gun_bot_server/`) + DynamoDB + S3 + SNS/FCM/APNs |
| **Mobile** | `gun_bot_mobile/` (planned) | Flutter app: auth, telemetry dashboard, manual control (dual virtual joystick), media gallery, push alerts |

Inter-core communication between A55 and M33 uses **RPMsg over
OpenAMP** — see `IPC.md` for the canonical payload contract.

External communication:
- **MQTT/TLS** via AWS IoT Core for telemetry, alerts, mode/config
  commands (full topic hierarchy in `design.md` §3.1.4).
- **Direct TCP / WebSocket** between Flutter and the A55 in Manual
  mode for sub-50 ms control latency.
- **RTSP / RTP** from the A55's hardware-accelerated VPU for live
  video.

---

## Subprojects

Each subproject has its own `CLAUDE.md` with build, deploy, and
implementation details. **Read the relevant one before touching code.**
`server/` has a top-level `CLAUDE.md` plus per-subfolder docs
(`server/aws/CLAUDE.md` for CloudFormation, `server/gun_bot_server/CLAUDE.md`
for the Go handlers).

| Folder | Status | Role |
|---|---|---|
| `gun_bot/` | **Active** | M33 Zephyr firmware — blaster mechanism (servos + trigger + pusher + lock) |
| `gun_bot_controller/` | Planned | A55 Linux userland — vision, networking, cloud, video |
| `server/aws/` | Planned | CloudFormation templates — every AWS resource declared here |
| `server/gun_bot_server/` | Planned | Go Lambda handlers (one binary each: auth, telemetry_ingest, alert_router, presign_upload, media_list) |
| `gun_bot_mobile/` | Planned | Flutter app (BLoC/Riverpod, dio, mqtt_client, flutter_vlc_player) |

Only `gun_bot/` is currently buildable. The other three folders are
placeholders — see their stub `CLAUDE.md`s for the planned scope.

---

## Build & run

### M33 firmware (`gun_bot/`)

The only thing currently buildable end-to-end.

```bash
cd gun_bot/m33_firmware/gun_controller
cmake --preset=debug
cmake --build --preset=debug

# Deploy to FRDM-iMX93 via Linux remoteproc on the A55:
# (full workflow in gun_bot/CLAUDE.md)
```

Prereqs: Zephyr SDK 0.17.4, toolchain venv at
`~/.mcuxpressotools/.mcux-venv-3.12/bin`. See `gun_bot/CLAUDE.md` for
the full environment and deploy steps.

### Other pillars

Not yet scaffolded with buildable code. When started, each will get a
top-level `Makefile` / `package.json` / `pubspec.yaml` referenced from
its own `CLAUDE.md`.

---

## Inter-core communication

The RPMsg contract between A55 (`gun_bot_controller/`) and M33
(`gun_bot/`) is defined canonically in [`IPC.md`](./IPC.md).
`gun_bot/IPC.md` and `gun_bot_controller/IPC.md` mirror that spec and
add per-side implementation notes.

Do not redefine the wire format in a subproject. Change the canonical
`IPC.md` and bump the protocol `ver` field.

---

## Other docs

- `PLAN.md` — phases & milestones. The execution plan; what we're
  building and in what order. **Check this before starting any new
  milestone.**
- `INFRASTRUCTURE.md` — runtime topology diagrams (Phase 1 + Phase 2
  preview + happy-path sequence). Where every process runs, what
  protocol each link uses.
- `design.md` — full system architecture (MQTT topics, DynamoDB schema,
  mobile workflows). **The source of truth for what MobTurret is.**
- `IPC.md` — A55 ↔ M33 RPMsg wire contract.
- `gun_bot/CLAUDE.md`, `gun_bot/IPC.md`, `gun_bot/TROUBLESHOOTING.md`
  — M33 firmware deep-dive.

---

## Conventions

- **Endianness:** little-endian on the wire (i.MX93 is LE-native).
- **Units:** angles in centi-degrees (`i16`, range ±327.67°), time
  in ms (`u16` or `u32`), temperature in °C (`i8`), battery in mV
  (`u16`), servo position in raw SCSCL units (`u16`, 0–4095).
- **Versions:** protocol changes bump the `ver` byte in the IPC
  header; never silently change payload layouts.
- **Secrets:** device certs, AWS keys, and FCM tokens never go in
  this repo.

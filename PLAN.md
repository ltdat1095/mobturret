# PLAN.md — MobTurret development plan

MobTurret is built in phases. Each phase is self-contained: at the
end of a phase, you can run the system end-to-end (locally or with
real hardware), even if later features are stubbed.

| Phase | Theme | Status |
|---|---|---|
| **1** | Local development — server + DynamoDB Local + ROS2 on A55 + Flutter | **In progress** |
| 2 | AWS wiring — replace local stubs with real AWS services | Planned |
| 3 | Real-time manual mode — direct TCP/WS, RTSP video, virtual joystick | Planned |
| 4 | Computer vision — YOLOv8 on the A55 NPU, depth fusion, auto-detect loop | Planned |
| 5 | Production hardening — security, multi-region, observability, OTA | Planned |

The authoritative architecture spec is [`design.md`](./design.md).
The A55 ↔ M33 wire contract is [`IPC.md`](./IPC.md). This file is
the **execution plan** — what we build, in what order.

---

## Phase 1 — Local development

**Goal:** a self-contained MobTurret stack that runs without AWS.
Used to develop and verify every user-facing flow (signup, login,
robot registration, state view, mode toggle) before we wire in real
cloud services.

### Hardware / runtime layout

| Component | Where it runs | Notes |
|---|---|---|
| Server (Go HTTP) | Dev machine (laptop) | `server/cmd/server` — Gin/Echo, plain HTTP for dev; same handlers re-wrap as Lambdas in Phase 2 |
| DynamoDB Local | Dev machine (Docker) | Single container; matches AWS DynamoDB API |
| Mobile app | Dev machine (Flutter desktop / Android emulator) | `gun_bot_mobile/` |
| `gun_controller` ROS2 node | **FRDM-iMX93 A55 (Linux)** | C++, talks to M33 over RPMsg |
| M33 Zephyr firmware | **FRDM-iMX93 M33** | Existing `gun_bot/` firmware, extended with an RPMsg endpoint |
| `cloud_bridge` ROS2 node | **FRDM-iMX93 A55 (Linux)** | Same host as `gun_controller`, talks to server over local HTTP/WS |

Inter-host link: dev machine ↔ FRDM-iMX93 over the local network
(Wi-Fi or Ethernet). The FRDM runs a ROS2 domain; the laptop doesn't
need to join it — it talks to the cloud_bridge node's HTTP/WS endpoint.

### ROS2 graph (Phase 1 scope)

```
                                          ┌─────────────────────┐
   /gun/target  ───────────────────────▶  │                     │
   /gun/fire    ───────────────────────▶  │  gun_controller     │  RPMsg  ▶ M33 (Zephyr)
   /gun/mode    ───────────────────────▶  │  (C++)              │  ◀ RPMsg
                                          │                     │
                                          └──────────┬──────────┘
                                                     │ /gun/state, /gun/telemetry
                                                     ▼
                                          ┌─────────────────────┐
                                          │  cloud_bridge       │
                                          │  (C++)              │
                                          └──────────┬──────────┘
                                                     │ HTTP POST /turrets/{id}/state
                                                     │ HTTP GET  /turrets/{id}/commands
                                                     ▼
                                          ┌─────────────────────┐
                                          │  server (Go HTTP)   │  ──▶ DynamoDB Local
                                          │  laptop             │
                                          └──────────┬──────────┘
                                                     │ REST
                                                     ▼
                                          ┌─────────────────────┐
                                          │  Flutter app        │
                                          │  (mobile / emulator)│
                                          └─────────────────────┘
```

Topic contract (initial; refined as we go):

| Topic | Type | Direction | Notes |
|---|---|---|---|
| `/gun/target` | `gun_msgs/Target` (yaw, pitch centi-deg) | → `gun_controller` | Rate-limited by `gun_controller` to the slew rate |
| `/gun/fire` | `std_msgs/Bool` (edge-triggered) | → `gun_controller` | `gun_controller` enforces 250 ms hardware cooldown |
| `/gun/mode` | `gun_msgs/Mode` (`MANUAL`/`AUTO`/`SAFE`) | → `gun_controller` | `SAFE` zeroes servo torque |
| `/gun/state` | `gun_msgs/State` (mode, last cmd, error flags) | ← `gun_controller` | 1 Hz |
| `/gun/telemetry` | `gun_msgs/Telemetry` (servo pos, batt_mv, temp_c) | ← `gun_controller` | 10 Hz |

`cloud_bridge` relays `/gun/state` + `/gun/telemetry` to the server,
and polls the server for pending commands (mode/fire/target) which
it re-publishes on the matching ROS2 topics. In Phase 3 we replace
the polling with a server-push WebSocket for sub-50 ms manual control.

### Milestones

Each milestone ends with something runnable. Mark the box only when
the milestone is verified end-to-end, not when the code is written.

#### M1 — Local dev environment

- [ ] `docker-compose.yml` at repo root: DynamoDB Local on **port 8181** (no admin UI)
- [ ] Go module under `server/cmd/server` — **Gin** HTTP framework,
      **Viper** for config (`.env`), **Zap** for logging, **Wire**
      for DI, **`gin-contrib/cors`** for CORS, **`google/uuid`** for
      user_id + request_id (full module layout + locked deps in
      `server/CLAUDE.md`)
- [ ] Empty ROS2 workspace at `ros2_ws/` with two package skeletons:
      `gun_controller`, `cloud_bridge` (target: **ROS2 Humble
      Hawksbill** on Ubuntu 22.04)
- [ ] A55 cross-toolchain: extract the **Yocto SDK** from the
      sibling Yocto build for the FRDM-iMX93 A55 image; cross-compile
      ROS2 nodes against it; deploy to FRDM via SSH (see
      [`gun_bot_controller/CLAUDE.md`](./gun_bot_controller/CLAUDE.md))
- [ ] `gun_bot_mobile/` initialized with `flutter create` and
      feature-first layout (`lib/features/{auth,dashboard,control,gallery}`)
- [ ] Each subproject's `CLAUDE.md` updated with setup steps for
      Phase 1 (separate from the MobTurret-overview CLAUDE.md)

**Done when:** `docker compose up` brings up DynamoDB Local on
`:8181`, the Go server starts on `localhost:8080` and serves
`/healthz` 200 OK, and `flutter run` builds the mobile app on the
emulator.

#### M2 — Local server: auth

- [ ] `Users` DynamoDB table (schema per `design.md` §5.1.1)
- [ ] `POST /auth/signup` — create user (bcrypt-hashed password)
- [ ] `POST /auth/login` — issue **HS256** JWT (24 h TTL) with shared
      secret from `JWT_SIGNING_KEY` env var
- [ ] `GET /auth/me` — verify middleware reads JWT
- [ ] Unit tests against DynamoDB Local container

**Done when:** `curl POST /auth/signup` → 201, `curl POST /auth/login`
→ 200 + token, `curl GET /auth/me` with token → 200, without token →
401. All in CI.

#### M3 — Local server: robot state + registration

- [ ] `Turrets` DynamoDB table (schema per `design.md` §5.1.2)
- [ ] `POST /turrets` — admin endpoint to register a robot
      (owner_id, robot_id)
- [ ] `GET /turrets` — list robots owned by the current user
- [ ] `GET /turrets/{id}` — single-robot state (online/offline,
      mode, last telemetry, last_seen)
- [ ] `POST /turrets/{id}/state` — `cloud_bridge` pushes state
      updates here (no auth required, signed by `robot_id` header
      shared secret for now)
- [ ] `GET /turrets/{id}/commands` — `cloud_bridge` polls pending
      mode/fire/target commands; server returns them in FIFO order
      and marks them delivered
- [ ] `Commands` DynamoDB table (PK: `robot_id`, SK: `seq`) — backs
      the polling endpoint

**Done when:** seed script registers a robot, `cloud_bridge` (or a
test stub) can `POST` state and `GET` commands, the values round-trip.

#### M4 — `gun_controller` ROS2 node

- [ ] ROS2 package `gun_controller` under `ros2_ws/src/`
      (**Humble Hawksbill**, C++)
- [ ] Cross-compile against the **Yocto SDK** for the A55 (extract
      from the sibling Yocto project); deploy binary to FRDM via SSH
- [ ] RPMsg client via **kernel `rpmsg_char`** driver (not
      `libopenamp`); see `gun_bot_controller/CLAUDE.md`
- [ ] Wire up `IPC.md` `MSG_SET_TARGET` / `MSG_FIRE` / `MSG_MODE`
      / `MSG_TELEMETRY` / `MSG_ALERT`
- [ ] `gun_msgs` package already drafted at
      `gun_bot_controller/ros2_ws/src/gun_msgs/msg/` — finish
      `package.xml` + `CMakeLists.txt` via `ros2 pkg create` when
      the workspace is initialised
- [ ] ROS2 topic ↔ RPMsg bridge:
  - `/gun/target` (`gun_msgs/Target`) → `MSG_SET_TARGET`
  - `/gun/fire` (`gun_msgs/Fire`, edge-detected) → `MSG_FIRE`
  - `/gun/mode` (`gun_msgs/Mode`) → `MSG_MODE`
  - `MSG_TELEMETRY` + `MSG_SERVO_DIAG` → `/gun/telemetry`
    (`gun_msgs/Telemetry`)
  - `MSG_ACK` + `MSG_ALERT` → `/gun/state` (`gun_msgs/State`)
- [ ] M33 Zephyr firmware extended with an RPMsg endpoint
      (`src/rpmsg.c`, SYS_INIT hook, IPC thread) — see
      `gun_bot/IPC.md`
- [ ] Verify on FRDM-iMX93: `ros2 topic echo /gun/telemetry` shows
      live values from the M33

**Done when:** `ros2 topic pub /gun/target …` makes the turret
slew, `ros2 topic pub /gun/fire std_msgs/Bool "data: true"` fires,
and `/gun/telemetry` shows changing servo positions.

#### M5 — `cloud_bridge` ROS2 node

- [ ] ROS2 package `cloud_bridge` under `ros2_ws/src/`
- [ ] Subscribes to `/gun/state` + `/gun/telemetry`
- [ ] HTTP client (libcurl or `rclcpp` + `curl`) to server:
  - `POST {SERVER}/turrets/{id}/state` every 1 s (latest state)
  - `GET  {SERVER}/turrets/{id}/commands` every 200 ms (poll for
    pending commands)
- [ ] On command: re-publish on `/gun/mode`, `/gun/target`,
      `/gun/fire` (consume from a queue, edge-detect fire)
- [ ] `robot_id` and `server_url` from ROS2 params / launch file
- [ ] Local config: `ros2_ws/launch/cloud_bridge.launch.py` with
      defaults

**Done when:** start both nodes + server; command the robot's mode
via `curl POST` (or a dev-only server endpoint) and observe the
state change propagate to the server.

#### M6 — Mobile app: auth

- [ ] Login screen (`email` + `password`)
- [ ] Signup screen
- [ ] `dio` HTTP client + auth interceptor (attach JWT)
- [ ] JWT stored in `flutter_secure_storage`
- [ ] **Riverpod 2.x** providers for auth state, API client,
      base URL (overridable via `--dart-define`)
- [ ] Auto-logout on 401; manual logout in settings
- [ ] Base URL configurable (defaults to `http://10.0.2.2:8080` on
      Android emulator for dev — points to host's localhost)

**Done when:** signup on the emulator → app stores token → killing
the app and relaunching shows the user still logged in.

#### M7 — Mobile app: robot state

- [ ] Dashboard: list owned robots (live status dot + last seen)
- [ ] Robot detail page: state, mode, telemetry (battery, temp,
      servo positions)
- [ ] Mode toggle (`MANUAL` / `AUTO` / `SAFE`) — issues a command
      via the server
- [ ] Pull-to-refresh + 1 s background polling for live state
- [ ] Empty / loading / error states

**Done when:** on the emulator, see the FRDM robot appear on the
dashboard within 1 s of `cloud_bridge` starting, mode toggle takes
effect at the robot within ~500 ms (round-trip = 200 ms poll +
processing).

#### M8 — End-to-end local integration

- [ ] `docker-compose.yml` runs: DynamoDB Local, server, plus a
      stub `cloud_bridge` that publishes fake telemetry for
      laptops without FRDM attached
- [ ] `scripts/e2e.sh` (or `make e2e`):
  1. `docker compose up -d dynamodb server`
  2. seed an admin user + register a robot
  3. start `cloud_bridge` (real or stub)
  4. `curl POST /turrets/{id}/commands` to set mode = `AUTO`
  5. verify `cloud_bridge` consumed the command
  6. verify `GET /turrets/{id}` shows `mode=AUTO`
- [ ] CI runs the e2e script on every PR
- [ ] Manual run sheet in `docs/run-sheet.md`: exact steps to bring
      the full stack up on a fresh laptop

**Done when:** `make e2e` is green on CI and the run sheet works
on a colleague's laptop.

---

## Phase 2 — AWS wiring (planned)

- Replace DynamoDB Local with AWS DynamoDB (`server/aws/`)
- Replace Go HTTP server with the same handlers wrapped as
  Lambda entry points; deploy via CloudFormation in `server/aws/`
- Add API Gateway in front of the Lambdas with a Cognito or
  custom-JWT authorizer
- Replace `cloud_bridge` HTTP polling with **AWS IoT Core** MQTT:
  the bridge becomes an IoT thing with mTLS cert, publishes
  telemetry on `turret/{robot_id}/telemetry`, subscribes to
  `turret/{robot_id}/cmd/+`
- Add S3 + SNS for alert clips + push notifications per
  `design.md` §4.2
- Mobile app: swap base URL to API Gateway; add FCM/APNs for
  push alerts

## Phase 3 — Real-time manual mode (planned)

- `cloud_bridge` opens a direct WebSocket listener for the mobile
  app on the local network
- Flutter app: WebRTC or RTSP video overlay + virtual dual-axis
  joystick → joystick packets go over the WebSocket, not MQTT
- Targets sub-50 ms local-network control latency (per
  `design.md` §3.2)
- STUN/TURN fallback for off-LAN clients (deferred to Phase 5)

## Phase 4 — Computer vision (planned)

- A55 NPU (ETHOS-U65 / Cadence VIP) + TensorRT-Lite pipeline
- YOLOv8-nano at 30 FPS targeting human detection
- RGB-D USB camera ingest; depth fusion gives 3D vectors
  $(x, y, z)$ relative to muzzle
- Auto-mode loop: `detection → MSG_SET_TARGET → MSG_FIRE (if
  armed and confidence above threshold)`

## Phase 5 — Production hardening (planned)

- mTLS for all device-to-cloud links, key rotation
- JWT rotation + refresh tokens
- Multi-region active/active for the API layer
- Structured logs + tracing (CloudWatch + X-Ray)
- OTA firmware updates for the M33 (mcuboot + image swap)
- Observability dashboards + alerting
- Penetration test + threat model

---

## Conventions for this plan

- Each milestone is independently shippable — if a phase slips, the
  milestones already shipped still work.
- "Done" means end-to-end verified, not "code merged". M2 isn't
  done until the curl round-trip works against a real DynamoDB Local.
- Open questions live in each subproject's `CLAUDE.md`, not here.
  This file tracks *what* and *in what order*; subproject docs
  track *how*.
- When priorities change, edit this file. Don't let the plan drift
  from what we're actually building.

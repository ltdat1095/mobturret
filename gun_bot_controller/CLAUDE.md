# gun_bot_controller — A55 Linux subproject

**Status: planned.** This folder is a placeholder; nothing is built
yet.

The A55 side of the FRDM-iMX93 runs Linux and handles the heavy
workloads that the deterministic M33 firmware shouldn't touch: vision
inference, network I/O, video encoding, and all cloud / mobile
communication.

## Responsibilities (per `design.md` §2.1.1)

- **Computer vision.** Ingest RGB-D USB camera stream; run YOLOv8
  (nano / small) at 30 FPS via TensorRT / NPU; produce bounding
  boxes and 3D vectors $(x, y, z)$ relative to the muzzle.
- **Cloud I/O.** Publish telemetry and alerts to AWS IoT Core over
  MQTT/TLS using a device X.509 cert. Consume `cmd/mode` and `config`
  topics.
- **Mobile I/O.** Serve the Manual-mode direct TCP / WebSocket
  channel for sub-50 ms pan / tilt / fire from the Flutter app.
- **Video.** Encode H.264 / H.265 via the hardware VPU and stream
  over RTSP / RTP / UDP.
- **Inter-core IPC.** Send `MSG_SET_TARGET`, `MSG_FIRE`, `MSG_MODE`,
  etc. to the M33 over RPMsg and consume `MSG_TELEMETRY`,
  `MSG_ALERT`, `MSG_SERVO_DIAG` back. **Wire contract:
  [`../IPC.md`](../IPC.md)** (canonical), mirrored at
  [`./IPC.md`](./IPC.md) (A55-side handler map).
  **Transport implementation: [`./RPMSG.md`](./RPMSG.md)** — the
  A55-side guide to the i.MX93 RPMsg/OpenAMP plumbing (remoteproc
  boot, `rpmsg_char` / `/dev/rpmsg*`, endpoint names, pitfalls).
  The M33 side is at [`../gun_bot/RPMSG.md`](../gun_bot/RPMSG.md).
  Wire format and transport are split into two docs so a wire-format
  change never has to touch the transport doc, and vice versa.

## Planned layout (not yet committed)

```
gun_bot_controller/
├── ros2_ws/
│   └── src/
│       ├── gun_controller/         # Phase 1 M4
│       ├── cloud_bridge/           # Phase 1 M5
│       └── gun_msgs/               # custom msg types (Target, Mode, Fire, State, Telemetry, ServoDiag)
│           ├── CLAUDE.md
│           └── msg/*.msg           # ← already written; see ros2_ws/src/gun_msgs/
├── vision/                 # YOLOv8 + TensorRT + depth fusion (Phase 4)
├── net/                    # MQTT (AWS IoT Core), TCP server for manual mode (Phase 2/3)
├── streaming/              # RTSP server, VPU H.264/H.265 encoder (Phase 3)
├── ipc/                    # RPMsg client to M33 (kernel rpmsg_char — locked)
├── systemd/                # service units
├── config/                 # device cert, AWS endpoint, RTSP port
└── README.md
```

## Runtime stack (locked decisions)

- **OS / distro:** A55 Linux image is built by a **separate Yocto
  project** that lives outside this repo. That Yocto build also
  produces an SDK we extract and cross-compile against.
- **ROS2:** **Humble Hawksbill** on Ubuntu 22.04 (hosted by Yocto).
- **Language:** C++ (`rclcpp`). Rust ROS2 clients are still
  experimental; revisit if a hard requirement appears.
- **RPMsg transport:** **Linux kernel `rpmsg_char` driver**
  (mainline `drivers/rpmsg/rpmsg_char.c`). Exposes RPMsg endpoints
  as `/dev/rpmsg_ctrlN` + `/dev/rpmsgN`. Wire protocol is identical
  to OpenAMP, but we avoid the userspace OpenAMP dependency in the
  Yocto image and get normal gdb + tooling.
- **Custom msg package:** `gun_msgs` — see
  [`./ros2_ws/src/gun_msgs/`](./ros2_ws/src/gun_msgs/) for the
  schema (Target, Mode, Fire, State, Telemetry, ServoDiag). Locked.
- **Build:** cross-compile ROS2 nodes on the dev machine against
  the Yocto SDK; copy the resulting binaries + libs to the FRDM
  over SSH.

## Cross-compile workflow (target)

The A55 system image is built by a **separate Yocto project** at
`~/Desktop/autonomous_explorer/imx93-frdm-yocto/frdm-imx93`
(MACHINE=`imx93-frdm`, DISTRO=`imx93-gunbot`). That project produces
the SDK installer we cross-compile against.

### One-time setup — build the SDK installer

The Yocto project has built the rootfs image
(`imx93-gunbot-imx93-frdm-*.wic`) but has **not** yet run
`populate_sdk` — `tmp/deploy/sdk/` does not exist on the dev
machine. Before M4 starts, the user needs to:

```bash
cd ~/Desktop/autonomous_explorer/imx93-frdm-yocto/frdm-imx93
source setup-environment <build-dir>
bitbake imx93-gunbot-imx93-frdm -c populate_sdk
```

This produces `tmp/deploy/sdk/imx93-gunbot-imx93-frdm-x86_64-<timestamp>-toolchain-<version>.sh`.

### One-time setup — install the SDK on the dev machine

```bash
chmod +x <installer>.sh
./<installer>.sh -y -d /opt/imx93-gunbot-sdk
# Verify:
source /opt/imx93-gunbot-sdk/environment-setup-aarch64-poky-linux
echo $OECORE_NATIVE_SYSROOT   # should print /opt/imx93-gunbot-sdk/sysroots/cortexa55-poky-linux
```

### Per-build — cross-compile a ROS2 package

```bash
# Source the SDK (every fresh shell)
source /opt/imx93-gunbot-sdk/environment-setup-aarch64-poky-linux

# Build
cd gun_bot_controller/ros2_ws
colcon build --packages-select gun_controller \
  --cmake-args -DCMAKE_TOOLCHAIN_FILE=$OECORE_NATIVE_SYSROOT/usr/lib/aarch64-poky-linux/cmake/OEToolchainConfig.cmake

# Deploy
scp -r install/gun_controller root@imx93frdm:/opt/ros2_ws/
ssh root@imx93frdm 'systemctl restart gun_controller.service'
```

## Open questions still to settle

- **ROS2 ↔ server protocol** — HTTP polling vs SSE vs WebSocket vs
  local MQTT. Defer to M5.
- **Local auth for the Manual-mode TCP socket (mTLS vs PSK)** —
  Phase 3.
- **RPMsg endpoint names** — proposed in [`./RPMSG.md`](./RPMSG.md)
  §3.1 as `turret-ctrl` (host→remote) and `turret-telemetry`
  (remote→host). Promote these to the canonical
  [`../IPC.md`](../IPC.md) §Transport so the wire doc stops being
  silent on the topic.

## Related docs

- [`../CLAUDE.md`](../CLAUDE.md) — MobTurret root
- [`../IPC.md`](../IPC.md) — A55 ↔ M33 wire contract (canonical)
- [`./IPC.md`](./IPC.md) — A55-side mirror with implementation notes
  (handler map: which messages the A55 emits in response to cloud /
  mobile input)
- [`./RPMSG.md`](./RPMSG.md) — **A55-side transport implementation of
  the inter-core link** (remoteproc boot, `rpmsg_char` /
  `/dev/rpmsg*` usage, endpoint names, liveness, pitfalls). Read
  this before writing any A55 IPC client. [`../IPC.md`](../IPC.md) is
  the wire format; this is what makes the A55 actually open the
  endpoints.
- [`../gun_bot/RPMSG.md`](../gun_bot/RPMSG.md) — the M33 side of the
  same link (resource table, MU1 client, Zephyr Kconfig). The two
  RPMSG docs together are the "how to make the link work" pair.
- [`../gun_bot/HOW_TO_DEBUG.md`](../gun_bot/HOW_TO_DEBUG.md) — the
  deploy + console-capture playbook (used by both sides)
- [`../design.md`](../design.md) — full architecture

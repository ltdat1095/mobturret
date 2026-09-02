# RPMSG — A55-side implementation guide

This is the **A55 (Linux) implementation** of the A55 ↔ M33
inter-core link. The wire format lives in [`./IPC.md`](./IPC.md);
this doc covers the **transport below the wire** as seen from
Linux userspace: the `remoteproc` boot of the M33, the kernel
`rpmsg_char` driver, the device-tree regions the kernel hands
you, and the syscall sequence that opens a working channel.

**Status (2026-09-02): the A55 device tree is fully wired for
OpenAMP/RPMsg and the Linux 6.6.36 image includes both
`imx_rproc` and `imx_rpmsg` drivers.** What is **not** present is
any A55 userland client (`gun_controller`, `ipc/heartbeat.c`, …)
— the A55 side of this repo is still planned, and the only thing
the A55 can do today with the link is `cat` the sysfs nodes.
The sections below describe what the A55 userland has to do
*once it's written* to actually move bytes.

## Two-doc relationship

| Doc | Scope |
|---|---|
| [`./IPC.md`](./IPC.md) | Wire format, message IDs, ACK/ALERT semantics, versioning |
| [`./RPMSG.md`](./RPMSG.md) (this) | remoteproc boot, rpmsg_char usage, sysfs, sequencing, pitfalls |

If you change the wire format, edit `IPC.md` and bump `ver`. If
you change transport details — kernel driver choice, endpoint
names, fd lifecycle — edit this file.

## 1. What's already in place on the A55

The running board (Linux 6.6.36, `imx93frdm`) is *already*
prepared for RPMsg. Confirmed on 2026-09-02 by reading
`/proc/device-tree/`:

| Resource | Where | What it is |
|---|---|---|
| `remoteproc-cm33` | `/proc/device-tree/remoteproc-cm33/` | `compatible = "fsl,imx93-cm33"`, `status = "okay"`, mboxes tx/rx/rxdb, `fsl,startup-delay-ms = 500` |
| `mailbox@44230000` | `/proc/device-tree/soc@0/bus@44000000/mailbox@44230000/` | MU1, `compatible = "fsl,imx93-mu"`, IRQ 22, `#mbox-cells = <2>` |
| `vdev0vring0@a4000000` | `/proc/device-tree/reserved-memory/vdev0vring0@a4000000/` | 32 KB RX vring for endpoint 0 |
| `vdev0vring1@a4008000` | `/proc/device-tree/reserved-memory/vdev0vring1@a4008000/` | 32 KB TX vring for endpoint 0 |
| `vdev1vring0@a4010000` | `/proc/device-tree/reserved-memory/vdev1vring0@a4010000/` | 32 KB RX vring for endpoint 1 |
| `vdev1vring1@a4018000` | `/proc/device-tree/reserved-memory/vdev1vring1@a4018000/` | 32 KB TX vring for endpoint 1 |
| `rsc-table@2021e000` | `/proc/device-tree/reserved-memory/rsc-table@2021e000/` | 4 KB, the M33 resource table location |
| `vdevbuffer@a4020000` | `/proc/device-tree/reserved-memory/vdevbuffer@a4020000/` | 1 MB shared scratch |
| `ele-reserved@a4120000` | `/proc/device-tree/reserved-memory/ele-reserved@a4120000/` | 1 MB ELE; do not touch |

Linux modules loaded: `imx_rproc`, `imx_rpmsg`, `rpmsg_core`,
`rpmsg_char`. `dmesg: imx rpmsg driver is registered` on every
boot.

The boot partition (`/run/media/boot-mmcblk0p1/`) ships a
**single relevant DTB** for our board: `imx93-11x11-frdm.dtb`.
There is no `imx93-11x11-frdm-rpmsg.dtb` — the rpmsg overlay is
already baked into the FRDM DTB itself, so no overlay selection
is needed in U-Boot. The two `*-rpmsg.dtb` files in the
partition are EVK-only and are not used.

## 2. remoteproc boot of the M33 firmware

The M33 is **not** booted standalone (no JLink, no MCUXpresso
debugger). Linux loads the ELF over the A55↔M33 boot ROM and
brings the M33 out of reset. The mechanism is the standard
`drivers/remoteproc/imx_rproc.c` driver.

### 2.1 Where the ELF lives

```
/lib/firmware/zephyr_<variant>_<yyyymmdd>.elf
```

Name with date so old builds are easy to identify. The current
ping-only firmware is `zephyr_ping_20260902.elf` (see
[`../gun_bot/HOW_TO_DEBUG.md`](../gun_bot/HOW_TO_DEBUG.md) §3
for the deploy recipe).

### 2.2 Boot sequence (mirrors `HOW_TO_DEBUG.md` §3)

```bash
# 1. Stop the running M33 firmware (retry up to 8x)
for i in 1 2 3 4 5 6 7 8; do
  [ "$(cat /sys/class/remoteproc/remoteproc0/state)" = "offline" ] && break
  echo stop > /sys/class/remoteproc/remoteproc0/state
  sleep 2
done

# 2. Point remoteproc at the new ELF
echo /lib/firmware/zephyr_<variant>_<date>.elf \
  > /sys/class/remoteproc/remoteproc0/firmware

# 3. Start
echo start > /sys/class/remoteproc/remoteproc0/state
sleep 3

# 4. Verify
cat /sys/class/remoteproc/remoteproc0/state
# → "running"
```

### 2.3 What "no resource table" looks like

If the M33 ELF lacks a `.resource_table` section, dmesg will
print:

```
remoteproc remoteproc0: Booting fw image zephyr_ping_20260902.elf, size 719420
remoteproc remoteproc0: No resource table in elf
remoteproc remoteproc0: remote processor imx-rproc is now up
```

The third line says "now up" but the M33 is **not** usable for
RPMsg — no virtio channel will ever form. To diagnose, check
the ELF:

```bash
ssh root@192.168.1.94 'scp_or_local: readelf -SW /lib/firmware/zephyr_*.elf | grep resource'
# If empty: the M33 side has not been ported to RPMsg yet.
# See ../gun_bot/RPMSG.md §2.
```

## 3. `/dev/rpmsg*` and `rpmsg_char` usage

The A55 userland talks to the M33 through the kernel
`rpmsg_char` driver (`drivers/rpmsg/rpmsg_char.c`). The driver
exposes:

- `/dev/rpmsg_ctrl<N>` — control device. Open once, use
  `RPMSG_CREATE_EPT_IOCTL` and `RPMSG_DESTROY_EPT_IOCTL` to
  create / destroy endpoints.
- `/dev/rpmsg<N>` — the data endpoint created by the ioctl.
  One fd per endpoint, opened by name returned from
  `RPMSG_CREATE_EPT_IOCTL`. Use `read()` / `write()` to send
  and receive bytes.

The kernel automatically creates `/dev/rpmsg_ctrl0` once the
remote processor is up **and** the resource table declared
virtio devices. With the current ping-only firmware, neither
file exists — the link is dormant.

### 3.1 Endpoint names (must match M33)

The M33 publishes service names via `rpmsg_service_register_endpoint()`;
the A55 matches them in the ioctl. Locked names:

| Endpoint | Service name (M33 publishes) | Direction | A55 fd |
|---|---|---|---|
| 0 | `turret-ctrl` | host → remote | `/dev/rpmsg0` |
| 1 | `turret-telemetry` | remote → host | `/dev/rpmsg1` |

If the A55 tries to open an endpoint with a name the M33 hasn't
published, `RPMSG_CREATE_EPT_IOCTL` returns `ENXIO`. Confirmed
silent failure mode — dmesg does not log it.

## 4. Bringing up the link from A55 userland

### 4.1 Boot sequencing rules (from `IPC.md` + `gun_bot/IPC.md`)

1. Linux boots; kernel registers `imx_rproc` and the
   `remoteproc-cm33` node.
2. **Linux does *not* start the M33 automatically** (no
   `auto-boot` property in the FRDM DTB). A userspace helper
   (systemd `m33-loader.service` or similar) must `echo start`
   after the wifi / cloud bring-up has reached a steady state.
3. The M33 runs its init; if it has a resource table, it
   advertises endpoints `turret-ctrl` and `turret-telemetry`.
4. Linux's `virtio_rpmsg_bus` notices the announcements and
   exposes `/dev/rpmsg_ctrl0`.
5. A55 userland opens `/dev/rpmsg_ctrl0`, creates the two
   endpoints by name, then begins `MSG_HEARTBEAT` at 1 Hz.
6. M33 sees the first heartbeat, drops its 1 Hz heartbeat
   advertisement, starts its 3 s watchdog.

### 4.2 A55 userland lifecycle (one endpoint)

```c
/* pseudocode; real impl is in ipc/rpmsg_client.c when written */

int ctrl_fd = open("/dev/rpmsg_ctrl0", O_RDWR | O_CLOEXEC);
if (ctrl_fd < 0) fatal("no rpmsg_ctrl — M33 not running RPMsg");

struct rpmsg_endpoint_info info = {
    .name = "turret-ctrl",
    .src = 0, .dst = 0xFFFF,
};
int ioctl_ret = ioctl(ctrl_fd, RPMSG_CREATE_EPT_IOCTL, &info);
if (ioctl_ret < 0) fatal("create ept: %s", strerror(errno));

/* ioctl returns the local endpoint address in info.addr; open
 * the data fd by name. The kernel creates /dev/rpmsg0 once a
 * remote endpoint matches the name we registered. */
int data_fd = open("/dev/rpmsg0", O_RDWR | O_CLOEXEC);
if (data_fd < 0) fatal("no /dev/rpmsg0: name mismatch?");

/* From here, write() and read() framed messages per IPC.md. */
```

Two endpoints in parallel (one for `MSG_SET_TARGET` / `MSG_FIRE`,
one for `MSG_TELEMETRY` / `MSG_ALERT`) keeps the latency-critical
manual-mode control loop independent of the telemetry stream.

### 4.3 Watchdog

`IPC.md` says absence of any inbound frame for > 3 s fires
`ALERT_WATCHDOG`. Implement it by:

- Background `read()` thread on `/dev/rpmsg1` (telemetry). If
  `read()` returns no data for > 3 s, raise
  `MSG_ALERT { code = 0x0006, param = ms_since_last_heartbeat }`
  on the cloud side (`evt/alert` MQTT topic) and flip local
  mode to `SAFE`.
- Heartbeat from M33 is 1 Hz once a host heartbeat is observed.
  The 3 s threshold is a true three missed heartbeats — robust
  to one transient drop.

## 5. Liveness check from the A55 shell

Once the M33 firmware has a resource table, you can poke the
link without writing any code:

```bash
# After `echo start` and `state=running`:
ls /dev/rpmsg*
#   /dev/rpmsg_ctrl0

# Create the control endpoint
echo "turret-ctrl" > /tmp/ept_name
RPMSG_CREATE_EPT_IOCTL=$(python3 -c 'print(0x4008_5605)')  # see <linux/rpmsg.h>
# (or use the `rpmsg_client_sample` utility if it's in the Yocto image)
```

A more deterministic smoke test is to read M33 console
(`/dev/ttyACM1`) and look for the OpenAMP banner on boot — the
M33's `prj.conf` is configured to log it. The A55 dmesg also
prints the channel creation:

```
virtio_rpmsg_bus virtio0: creating channel tur[tle]
rpmsg_char: registered new rpmsg_client device rpmsg_ctrl0
```

The phrase "registered new rpmsg_client device rpmsg_ctrl0" is
the canonical "RPMsg is alive" signal.

## 6. Pitfalls (from existing bring-up notes)

- **DTB selection.** The board partition has 30+ DTBs; only
  `imx93-11x11-frdm.dtb` is correct. The `imx93-11x11-evk-*.dtb`
  files do not have the FRDM-specific pinctrl/peripheral map and
  will fail to bring up wifi. U-Boot selects by name; verify
  `cat /proc/device-tree/compatible` shows `fsl,imx93-11x11-frdm`.
- **`No resource table in elf`.** Means the M33 ELF lacks
  `.resource_table`. The link won't form. See
  [`../gun_bot/RPMSG.md`](../gun_bot/RPMSG.md) §2.
- **Endpoint name typo.** `/dev/rpmsg0` is created only after a
  matching `RPMSG_CREATE_EPT_IOCTL` AND a remote announcement
  with the same name. If the M33's `prj.conf` advertises
  `turret-ctrl` and the A55 asks for `turret-ctrl`, both
  succeed silently. A mismatch (e.g. `turret_ctrl`) gives
  `ENXIO` from the ioctl with no dmesg line.
- **Boot order.** Starting `gun_controller` (the A55 IPC client)
  *before* `echo start` on remoteproc just means the A55 sits on
  a `select()` until `/dev/rpmsg_ctrl0` appears. Make the
  systemd unit `After=m33-loader.service` to avoid racing.
- **CRTC / SoC crash.** The 2026-07-13 v19 test showed that a
  bad M33 firmware can take down the A55's wifi within seconds
  (see [`../gun_bot/INIT_SOURCE_PROBLEM.md`](../gun_bot/INIT_SOURCE_PROBLEM.md)
  §1 case 3). A55 userland cannot recover from this; the only
  recovery is the physical cable-replug in
  [`../gun_bot/HOW_TO_DEBUG.md`](../gun_bot/HOW_TO_DEBUG.md) §6.

## 7. What this gets you, and what it doesn't

**Gets you:** a working byte-stream between the A55 userland and
the M33 firmware, framing the messages in [`IPC.md`](./IPC.md).
All cloud, mobile, and vision functionality on the A55 routes
its commands to the M33 through this link.

**Doesn't get you:**
- The M33's *interpretation* of those messages — that's
  [`../gun_bot/IPC.md`](../gun_bot/IPC.md) (M33 handler map).
- The servo-bus protocol itself (LPUART3 + SCSCL) — see
  [`../gun_bot/SERVO_SETUP.md`](../gun_bot/SERVO_SETUP.md).
- The A55 vision stack, MQTT bridge, RTSP server — those are
  `gun_controller/`, `net/`, `streaming/`, none of which exist
  yet (see [`./CLAUDE.md`](./CLAUDE.md) for the planned layout).

## 8. Open / unfinished (as of 2026-09-02)

- **No A55 userland client** (high). `/dev/rpmsg_char` works, but
  nothing opens it. `ros2_ws/src/gun_controller/` is the planned
  home for the `ipc/rpmsg_client.c` + `ipc/heartbeat.c` files
  referenced in [`./CLAUDE.md`](./CLAUDE.md).
- **Endpoint names not in `IPC.md`** (low). "turret-ctrl" and
  "turret-telemetry" are proposed in [`../gun_bot/RPMSG.md`](../gun_bot/RPMSG.md)
  §5. Promote them to the canonical `IPC.md` so both sides stop
  being silent on the topic.
- **M33 side has no resource table** (high). Ping-only firmware
  does not advertise vdevs. The A55 userland cannot exercise
  this until the M33 build adds the table — see
  [`../gun_bot/RPMSG.md`](../gun_bot/RPMSG.md) §2 and §8.

## Related docs

- [`./CLAUDE.md`](./CLAUDE.md) — A55 subproject entry point
- [`./IPC.md`](./IPC.md) — wire format (canonical, mirror of
  root `../IPC.md`)
- [`../gun_bot/RPMSG.md`](../gun_bot/RPMSG.md) — the M33 side
  of this link
- [`../gun_bot/IPC.md`](../gun_bot/IPC.md) — M33 handler map
- [`../gun_bot/HOW_TO_DEBUG.md`](../gun_bot/HOW_TO_DEBUG.md) —
  deploy + console capture sequence (used by both sides)
- [`../design.md`](../design.md) — full architecture

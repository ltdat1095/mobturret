# gun_bot — M33 blaster mechanism firmware

This is the **M33 (Zephyr RTOS) subproject of MobTurret**. It drives
the SC15 servo bus and the firing mechanism on the Cortex-M33 core of
the FRDM-iMX93.

For the full system picture (A55 Linux, AWS, Flutter) see
[`../CLAUDE.md`](../CLAUDE.md). For the A55 ↔ M33 wire contract (the
main artifact of this subproject) see [`./IPC.md`](./IPC.md).

Documentation map:
- [`./HOW_TO_DEBUG.md`](./HOW_TO_DEBUG.md) — **how to deploy and test
  on real hardware.** The operational playbook: build → scp to A55 →
  remoteproc stop/start → crash check → read M33 console → recover
  from a crash. **Read this if you want to run on the board.**
- [`./m33_firmware/BUILD_SYSTEM.md`](./m33_firmware/BUILD_SYSTEM.md) — how
  to build and deploy the firmware. **Read this if you want to build.**
- [`./IPC.md`](./IPC.md) — M33-side mirror of the wire format
  (canonical at [`../IPC.md`](../IPC.md)). The handler map: which
  inbound messages the M33 implements and which outbound messages it
  emits.
- [`./RPMSG.md`](./RPMSG.md) — **the M33-side transport implementation
  of the inter-core link** (resource table, MU1 mailbox client,
  Zephyr Kconfig, address map reconciled with the A55 DTB). Read
  this before adding RPMsg to the M33 firmware. [`../IPC.md`](../IPC.md)
  is the wire format; this is what makes the M33 actually publish
  the endpoints.
- [`./SERVO_SETUP.md`](./SERVO_SETUP.md) — SC15 servo bus bring-up
  record, SCSCL wire protocol, wheel-mode direction control, and the
  **§0 motion safety envelope**. Read §0 before commanding any motion.
- [`./INIT_SOURCE_PROBLEM.md`](./INIT_SOURCE_PROBLEM.md) — the
  initial-step problems (LPUART3 baud, missing printk, SoC crash,
  broken SDK file, SDK auto-selection), their root causes, and the
  fixes that produced the working recipe. Read this for context.
- [`./RPMSG_BISECT_20260902.md`](./RPMSG_BISECT_20260902.md) —
  the 17-iteration v1–v17 bisect that got the link half-up.
  A55 side works; M33 side has an unidentified LPUART2 console
  wedge.
- [`./RPMSG_BISECT_20260903.md`](./RPMSG_BISECT_20260903.md) —
  the v18–v33 bisect pass. Identifies the v22 "LPUART2 alive"
  result as a stale-buffer false positive, explains why the DT
  MPU child regions don't apply at runtime, and documents the
  ARMv8-M RLAR EN-bit layout. Read this **before** continuing
  the RPMSG port.

---

## TL;DR

```bash
cd m33_firmware/gun_controller
cmake --preset=debug
cmake --build --preset=debug

# Deploy to FRDM-iMX93 via A55 Linux remoteproc:
scp debug/zephyr/zephyr.elf root@<frdm-ip>:/lib/firmware/zephyr_vN.elf
ssh root@<frdm-ip> '
  echo stop > /sys/class/remoteproc/remoteproc0/state
  sleep 2
  echo /lib/firmware/zephyr_vN.elf > /sys/class/remoteproc/remoteproc0/firmware
  echo start > /sys/class/remoteproc/remoteproc0/state
  sleep 3
'
```

The full step-by-step (host prereqs → SDK install → submodule update →
west workspace populate → build → deploy → verify on hardware) is in
[`./m33_firmware/BUILD_SYSTEM.md`](./m33_firmware/BUILD_SYSTEM.md) § 4.

---

## Role in MobTurret

The M33 firmware is a deterministic real-time controller. It does
**not** do vision, networking, or cloud I/O — those live on the A55
side. The M33 listens for `MSG_SET_TARGET`, `MSG_FIRE`, `MSG_MODE`,
etc. over RPMsg and drives servos.

Inbound commands: `MSG_SET_TARGET` (yaw/pitch), `MSG_FIRE`,
`MSG_MODE`, `MSG_CONFIG`, `MSG_EMERGENCY_STOP`, `MSG_HEARTBEAT`,
`MSG_PING`. Outbound telemetry: `MSG_TELEMETRY`, `MSG_SERVO_DIAG`,
`MSG_ALERT`, `MSG_PONG`, `MSG_ACK`. See
[`./IPC.md`](./IPC.md) for the M33-side handler map.

---

## Hardware

- **Board:** FRDM-iMX93, **Cortex-M33 core** (MIMX9352)
- **Build target:** `imx93_evk/mimx9352/m33` — this Zephyr fork does
  not ship a `frdm_imx93/mimx9352/m33` board port, but the M33
  subsystem is identical between the EVK and FRDM, so the EVK port
  is reused.
- **Servos:** 3× SC15 serial servos (SCSCL protocol, **1,000,000 baud**
  factory default) on **LPUART3**:
  - TX: `GPIO_IO14` (`iomuxc1_gpio_io14_lpuart_tx_lpuart3_tx`)
  - RX: `GPIO_IO15` (`iomuxc1_gpio_io15_lpuart_rx_lpuart3_rx`)
  - Base: `0x42570000` (M33 non-secure alias), NVIC IRQ 68
- **Console:** LPUART2 → CH9102 dual-USB-serial channel B → `/dev/ttyACM1`

The 1 Mbaud clock math and the SYS_INIT clock hook needed to make
this work live in `src/main.cpp`. Why that's necessary is recorded
in [`./INIT_SOURCE_PROBLEM.md`](./INIT_SOURCE_PROBLEM.md) § 2.1 and
§ 2.3.

---

## Files

| Path | Role |
|---|---|
| `m33_firmware/gun_controller/src/main.cpp` | The M33 firmware. `lpuart_clocks_init` SYS_INIT (PRE_KERNEL_1 prio 0) opens the LPUART3 clock root; `loopback_thread` runs the 16-byte loopback test every 1 s and floods PASS/FAIL to `ttyACM1`. |
| `m33_firmware/gun_controller/src/servo/` | SCSCL / SCS / SCSerial / SMS_STS protocol library for the SC15 servos. |
| `m33_firmware/gun_controller/boards/imx93_evk_mimx9352_m33.overlay` | Declares `lpuart3: serial@42570000`, IRQ 68, pinctrl `uart3_default` on `&pinctrl` (NOT at root). Pins: GPIO_IO14 TX, GPIO_IO15 RX. |
| `m33_firmware/gun_controller/prj.conf` | `CONFIG_SERIAL=y`, `CONFIG_UART_MCUX_LPUART=y`, `CONFIG_PRINTK=y`. |
| `m33_firmware/gun_controller/CMakePresets.json` | `debug` and `release` presets; BOARD=`imx93_evk/mimx9352/m33`. |
| `m33_firmware/gun_controller/mcux_include.json` | `ZEPHYR_BASE` and `ZEPHYR_SDK_INSTALL_DIR` env. (The SDK 0.17.4 value declared here is overridden by CMake auto-selection of SDK 1.0.1 — see [`./INIT_SOURCE_PROBLEM.md`](./INIT_SOURCE_PROBLEM.md) § 3.5.) |
| `m33_firmware/nxp_zephyr/` | Git submodule → user's MobTurret zephyr fork at `mobturret/m33-lpuart3-clock-and-overlay`. Contains the LPUART3 dtsi, the `IMX_CCM_LPUART{1..8}_CLK` clock cases, and `west.yml` pinning hal_nxp. |
| `m33_firmware/{modules,bootloader,tools,.west}/` | West workspace output. Populated by `west update`. All in `.gitignore`. The critical entry is `modules/hal/nxp` at `c7f1b8449` (the MobTurret 1 Mbaud SDK patch). |
| `./IPC.md` | The A55 ↔ M33 wire contract (M33-side mirror). The main artifact of this subproject. |
| `./RPMSG.md` | M33-side transport implementation: resource table, MU1 client, Zephyr Kconfig, A55 DTB address map. |
| `./HOW_TO_DEBUG.md` | Operational playbook: deploy to hardware, monitor, debug, recover. |
| `./SERVO_SETUP.md` | SC15 bus bring-up, SCSCL protocol, motion safety envelope (§0). |
| `./INIT_SOURCE_PROBLEM.md` | Initial-step archaeology (root causes and fixes). |
| `./m33_firmware/BUILD_SYSTEM.md` | Build system reference (clone → build → deploy). |

Build artifacts under `debug/`, `release/`, `build/`, `install/`,
`log/` are gitignored — see the root `.gitignore`.

---

## Workflow

1. Read [`./m33_firmware/BUILD_SYSTEM.md`](./m33_firmware/BUILD_SYSTEM.md) § 4.
2. Make code changes in `src/main.cpp` or `src/servo/`.
3. Build with `cmake --build --preset=debug` from `m33_firmware/gun_controller`.
4. Deploy + verify on hardware by following
   [`./HOW_TO_DEBUG.md`](./HOW_TO_DEBUG.md) §3–§5 (the TL;DR above is
   the short form; the playbook adds the stop-retry loop, the
   post-deploy crash check, and console capture).
5. If the deploy crashes the SoC (A55 wifi drops), recover per
   [`./HOW_TO_DEBUG.md`](./HOW_TO_DEBUG.md) §6 — physical cable
   unplug/replug. There is no software power-cycle.
6. Before commanding servo motion, read
   [`./SERVO_SETUP.md`](./SERVO_SETUP.md) §0 (safety envelope).
7. For loopback bring-up: wire GPIO_IO14 ↔ GPIO_IO15 on the FRDM
   board (a single Dupont jumper) before booting. The M33 will
   print `LPUART3: loopback OK` to `ttyACM1` once per second.

---

## What this subproject does NOT do

- No vision, no MQTT, no AWS, no Flutter — those are A55 / cloud /
  mobile responsibilities.
- No standalone boot via JLink — the M33 is loaded by A55 Linux via
  remoteproc. The output of `cmake --build` is `zephyr.elf`, not
  `zephyr.bin`.

The `zephyr.bin` raw binary is generated as a side effect but isn't
used for deployment — remoteproc needs the ELF.
# gun_bot — M33 blaster mechanism firmware

This is the **M33 (Zephyr RTOS) subproject of MobTurret**. It drives
the SC15 servo bus and the firing mechanism on the Cortex-M33 core of
the FRDM-iMX93.

For the full system picture (A55 Linux, AWS, Flutter) see
[`../CLAUDE.md`](../CLAUDE.md). For the A55 ↔ M33 wire contract see
[`../IPC.md`](../IPC.md) and the mirrored [`./IPC.md`](./IPC.md).
For low-level debugging (SDK corruption, 1 Mbaud, remoteproc quirks)
see [`./TROUBLESHOOTING.md`](./TROUBLESHOOTING.md).

If you only need to read servo positions, read [`./SERVOS.md`](./SERVOS.md).

---

## TL;DR

```bash
cd m33_firmware/gun_controller
cmake --preset=debug
cmake --build --preset=debug

# Deploy to FRDM-iMX93 via A55 Linux remoteproc:
ssh root@imx93frdm '
  echo stop > /sys/class/remoteproc/remoteproc0/state
  while [ "$(cat /sys/class/remoteproc/remoteproc0/state)" != "offline" ]; do sleep 0.5; done
  cat > /lib/firmware/zephyr_v2.elf' < debug/zephyr/zephyr.elf
ssh root@imx93frdm 'echo /lib/firmware/zephyr_v2.elf > /sys/class/remoteproc/remoteproc0/firmware
                    echo start > /sys/class/remoteproc/remoteproc0/state'
```

---

## Role in MobTurret

The M33 firmware is a deterministic real-time controller. It does **not**
do vision, networking, or cloud I/O — those live on the A55 side in
`gun_bot_controller/` (planned). The M33 listens for `MSG_SET_TARGET`,
`MSG_FIRE`, `MSG_MODE`, etc. over RPMsg and drives servos.

Inbound commands: `MSG_SET_TARGET` (yaw/pitch), `MSG_FIRE`, `MSG_MODE`,
`MSG_CONFIG`, `MSG_EMERGENCY_STOP`, `MSG_HEARTBEAT`, `MSG_PING`.
Outbound telemetry: `MSG_TELEMETRY`, `MSG_SERVO_DIAG`, `MSG_ALERT`,
`MSG_PONG`, `MSG_ACK`. See [`./IPC.md`](./IPC.md) for the M33-side
implementation map.

---

## Hardware

- **Board:** FRDM-iMX93, **Cortex-M33 core** (MIMX9352)
- **Build target:** `imx93_evk/mimx9352/m33` — this Zephyr fork does not
  ship a `frdm_imx93/mimx9352/m33` board port, but the M33 subsystem is
  identical between the EVK and FRDM, so the EVK port is reused.
- **Servo bus:** 3× SC15 servos (SCSCL protocol, **1,000,000 baud** —
  see `SERVOS.md`) on **LPUART3**:
  - TX: `GPIO_IO14` (`iomuxc1_gpio_io14_lpuart_tx_lpuart3_tx`)
  - RX: `GPIO_IO15` (`iomuxc1_gpio_io15_lpuart_rx_lpuart3_rx`)
  - Base: `0x42570000` (M33 non-secure alias), NVIC IRQ 68
- **Console:** LPUART2 — collides with A55 Linux debug console under
  remoteproc; see "Console output caveat" below.

---

## Files

| Path | Purpose |
|---|---|
| `m33_firmware/gun_controller/src/main.cpp` | `imx93_m33_clock_init` SYS_INIT hook, RPMsg entry point (when wired up), servo control threads |
| `m33_firmware/gun_controller/src/servo/` | SCSCL protocol library (`SCSerial`, `SCSCL`, `SCS`, `SMS_STS`) |
| `m33_firmware/gun_controller/boards/imx93_evk_mimx9352_m33.overlay` | `lpuart3` soc node + `uart3_default` pinctrl group |
| `m33_firmware/gun_controller/prj.conf` | `CONFIG_SERIAL`, `CONFIG_UART_MCUX_LPUART`, etc. |
| `m33_firmware/gun_controller/CMakePresets.json` | debug/release presets; `BOARD=imx93_evk/mimx9352/m33` |
| `m33_firmware/gun_controller/SERVOS.md` | Bench servo inventory and SCSCL reference |
| `m33_firmware/nxp_zephyr` | Symlink → `~/mobturret-forks/zephyr` (user's GitHub fork, upstream zephyr main, `hal_nxp` from upstream). MobTurret SDK baud-override patch is applied directly to the fork's `modules/hal/nxp/mcux/mcux-sdk-ng/drivers/lpuart/fsl_lpuart.c`. |

---

## Build

```bash
cd m33_firmware/gun_controller
cmake --preset=debug      # debug
cmake --build --preset=debug
# or:
cmake --preset=release
cmake --build --preset=release
```

Outputs:
- `debug/zephyr/zephyr.elf` — loadable via Linux remoteproc
- `debug/zephyr/zephyr.bin` — raw, for standalone flash via JLink

If the overlay isn't being picked up (`debug/zephyr/zephyr.dts` has no
`lpuart3` node), clear the cmake cache:

```bash
rm -rf debug/CMakeCache.txt debug/CMakeFiles
cmake --preset=debug
cmake --build --preset=debug
```

### Environment

- **ZEPHYR_BASE:** `m33_firmware/nxp_zephyr` (resolves through the
  symlink to `~/mobturret-forks/zephyr` — the user's fork's source
  root, not a workspace subdir). Set in `mcux_include.json` under the
  `debug-env` / `release-env` preset `environment` blocks.
- **Zephyr SDK:** `~/zephyr-sdk-0.17.4` (installed once)
- **Toolchain venv:** `~/.mcuxpressotools/.mcux-venv-3.12/bin`
- **MCUXpresso SDK:** `mcusdk_m33_firmware/sdks/mcimx93_evk_blank_sdk/mcuxsdk/`
  (downloaded as a zip — see below)

### Fetch the vendor trees (one-time setup)

The Zephyr source tree and the MCUXpresso SDK are upstream vendor
sources and are **not tracked in git** — they're gitignored. The
project's `nxp_zephyr` path is a **symlink** to the user's local fork
clone at `~/mobturret-forks/zephyr`. Set that up once:

```bash
# Zephyr — use the user's GitHub fork (upstream zephyr main, with
# hal_nxp pulled in as a west submodule from zephyrproject-rtos).
# The fork root is a west workspace; the zephyr source is at zephyr/.
mkdir -p ~/mobturret-forks
cd ~/mobturret-forks
git clone --depth 1 https://github.com/ltdat1095/zephyr.git
cd zephyr
pipx install west   # or: pip install --user west
west init -l .
west update          # pulls modules/hal/nxp, modules/crypto/..., etc.

# Wire the project to the fork:
ln -s /home/ltdat/mobturret-forks/zephyr \
    /home/ltdat/Desktop/mobturret/gun_bot/m33_firmware/nxp_zephyr

# Apply the MobTurret SDK baud-override patch:
cd ~/mobturret-forks/modules/hal/nxp
git apply /path/to/mobturret/gun_bot/m33_firmware/gun_controller/fork-snapshots/nxp-zephyr-fork/0001-fsl_lpuart-1mbaud-baud-override-fork4.5.patch

# MCUXpresso SDK for i.MX93 EVK
cd ../../mcusdk_m33_firmware
mkdir -p sdks
cd sdks
# Download from https://mcuxpresso.nxp.com/en/builder (select
# "MCIMX93-EVK board", "Blank" example set), then unzip:
unzip ~/Downloads/mcimx93_evk_blank_sdk.zip -d .
```

The repo's `.gitignore` keeps these paths out of git. Build artifacts
under `debug/`, `release/`, `build/`, `install/`, `log/` are also
gitignored — see the root `.gitignore`.

> **Migration history:** Before 2026-06-28 the project used NXP's
> downstream fork (`nxp-zephyr/zephyr` at `nxp-v4.3.0`) checked out
> directly under `m33_firmware/nxp_zephyr/`. The pre-migration tree is
> preserved at `m33_firmware/nxp_zephyr.bak/` (gitignored) for
> archaeology; delete it once you're sure you don't need to compare.

---

## Workflow

1. Edit code under `m33_firmware/gun_controller/src/`.
2. Build with `cmake --build --preset=debug`.
3. Deploy via A55 Linux remoteproc (commands above). Note the kernel
   caches the firmware by name — use a new filename (`zephyr_v2.elf`,
   `zephyr_v3.elf`, …) each redeploy to bust the cache, or unbind/
   rebind the platform device.
4. Before chasing servo bugs, run the GPIO_14 ↔ GPIO_15 loopback test
   (see "Verification steps" below) to confirm the LPUART3 path is
   working end-to-end.

---

## Why the SYS_INIT clock hook is mandatory

The Zephyr i.MX93 M33 soc port only zeroes DTCM and clears a sleep-hold
bit. **No CCM setup.** Without our `imx93_m33_clock_init` SYS_INIT hook
(PRE_KERNEL_1, priority 0):

- LPUART2 clock root OFF → console silent → looks like a hang
- LPUART3 clock root OFF → `uart_poll_out()` hangs on TDRE

The hook calls the equivalent of MCUXpresso's `BOARD_InitHardware()`
(clock root source/divider + IP gate) for every peripheral used. It
must run before the LPUART driver's `CONFIG_SERIAL_INIT_PRIORITY = 50`.

The Zephyr clock driver flips the IP gate (`CLOCK_EnableClock` →
LPCG) but does **not** call `CLOCK_SetRootClock` to clear the OFF bit
or configure mux/divider — that's a gap in this Zephyr fork.

---

## Overlay gotchas (easy to get wrong)

### 1. Overlay filename must match the BOARD

Board is `imx93_evk/mimx9352/m33`. Zephyr's `zephyr_file()` resolves
the overlay filename with `/` → `_` substitution, so the file **must**
be `boards/imx93_evk_mimx9352_m33.overlay`. A file named
`frdm_imx93_mimx9352_m33.overlay` is silently ignored.

### 2. Pinctrl must extend the iomuxc container

```dts
&pinctrl {                              /* ✅ extends the iomuxc container */
    uart3_default: uart3_default { … };
};
```

NOT a `/ { pinctrl { … } }` root-level group — that creates a separate
container and the per-node `*_P_pinmux_FOREACH_PROP_ELEM` macros never
get generated. Build then fails at `pinctrl_soc.h:74` with
`'Z_PINCTRL_STATE_PIN_INIT' undeclared`.

### 3. `current-speed` and `pinctrl-names = "default"` are required

The `nxp,imx-lpuart` driver reads `current-speed` for baud, and
`pinctrl-names = "default"` is required by
`PINCTRL_DT_INST_DEV_CONFIG_GET`. Without either, the driver fails to
bind or hangs.

### 4. LPUART3 IRQ is 68 (NVIC), not a GIC SPI

The M33 view uses ARMv8-M NVIC, not GIC. Use
`interrupts = <68 3>;` (NVIC #68, priority 3 — matching `lpuart2`'s
priority in `nxp_imx93_m33.dtsi:105`).

---

## Deploy via Linux remoteproc

The FRDM-iMX93 typically runs Linux on the A55, which loads M33
firmware via remoteproc. The M33 binary must be an **ELF**, not raw
`.bin`. Linux rejects re-writes to the same firmware name with EBUSY —
use a new filename each redeploy, or unbind/rebind the platform
device.

Full sequence:

```bash
# On the FRDM Linux shell (root@imx93frdm)
echo stop > /sys/class/remoteproc/remoteproc0/state
while [ "$(cat /sys/class/remoteproc/remoteproc0/state)" != "offline" ]; do
    sleep 0.5
done

# Push the ELF (use a new filename each time to bust the kernel cache)
scp debug/zephyr/zephyr.elf root@imx93frdm:/lib/firmware/zephyr_v2.elf
ssh root@imx93frdm '
  echo /lib/firmware/zephyr_v2.elf > /sys/class/remoteproc/remoteproc0/firmware
  echo start > /sys/class/remoteproc/remoteproc0/state
'
```

---

## Console output caveat

The M33's `printk()` goes to **LPUART2**. When Linux is running on
A55, LPUART2 is owned by Linux's debug console — M33 printk either
produces no visible output or fights with Linux's writes. Three
workarounds, in order of preference:

- **RPMsg log forwarding (clean):** enable `CONFIG_LOG_BACKEND_RPMSG=y`
  in `prj.conf`, read from `/dev/ttyRPMSG0` on the A55 side.
- **Dedicated UART for M33 console:** change the overlay's
  `chosen { zephyr,console = &lpuart4; }` and wire a USB-UART
  adapter to the LPUART4 pins.
- **Standalone boot (no Linux):** flash `zephyr.bin` to M33 TCM via
  JLink; LPUART2 console works normally.

---

## Verification steps (in order)

Run these in order; each confirms one layer.

1. **Zephyr banner on LPUART2** — `*** Booting Zephyr OS build … ***`
   → clock init hook ran, LPUART2 root + gate up.
2. **`LPUART3: device ready @ serial@42570000`** — driver bound, base
   correct. If this fails, the issue is overlay/devicetree.
3. **Loopback test** — wire GPIO_14 ↔ GPIO_15, look for
   `LPUART3: loopback OK (8 bytes echoed)`. Proves TX→RX round trip
   end-to-end. If this fails but step 2 passes, the issue is the wire
   or pinmux.
4. **Servo ping with broadcast ID** (`0xFE`) at **1,000,000 baud**
   (`current-speed = <1000000>` in the overlay). Without knowing the
   servo ID, broadcast pings every servo on the bus. Look for
   `LPUART3: RX=0x..` bytes — the third byte is the servo's actual ID.
   Currently 2 servos on the bench: ID 1 and ID 2 (both model 3845).
5. **Servo ping with unicast ID** — once step 4 reveals the ID,
   hard-code it and verify `Received N bytes` consistently.

For 1 Mbaud bring-up issues (BAUD register math, SDK corruption
recovery) see [`./TROUBLESHOOTING.md`](./TROUBLESHOOTING.md).

---

## Known limitations

- **No console output via Linux serial** when running under A55 Linux
  (see above).
- **No `frdm_imx93/mimx9352/m33` board port** exists in this Zephyr
  fork — we reuse the EVK port. The SoC-level differences between EVK
  and FRDM matter for board peripherals (sensors, connectors), not
  for the M33 subsystem. Adding a proper FRDM M33 board port is a
  separate task.
- **No power management** — `pm_policy_state_lock_get()` in the
  LPUART driver keeps the M33 out of low-power states while
  transmitting. Fine for a development build; revisit when adding
  sleep modes.
- **SDK clock driver gap** — see `TROUBLESHOOTING.md` for the
  workaround (our SYS_INIT hook supplies what the clock driver
  should have done).

---

## Where to look in the code

```cpp
// src/main.cpp (in order)
imx93_m33_clock_init()                  // SYS_INIT PRE_KERNEL_1, prio 0
// rpmsg_listen()                       // (planned) A55↔M33 message loop
run_loopback_test(uart)                 // sends "ZEPHYR\r\n", verifies echo
servo_ping_thread()                     // SCSCL ping loop, broadcast ID 0xFE
```

```dts
// boards/imx93_evk_mimx9352_m33.overlay
soc { lpuart3: serial@42570000 { … } }
&pinctrl { uart3_default { group0 { pinmux, bias-pull-up, slew-rate, drive-strength } } }
&lpuart3 { status = "okay"; current-speed = <1000000>; pinctrl-0; pinctrl-names }
```

---

## Related docs

- [`../CLAUDE.md`](../CLAUDE.md) — MobTurret root
- [`../IPC.md`](../IPC.md) — A55 ↔ M33 wire contract (canonical)
- [`./IPC.md`](./IPC.md) — M33-side mirror of the IPC contract
- [`./SERVOS.md`](./SERVOS.md) — SC15 servo inventory + SCSCL reference
- [`./TROUBLESHOOTING.md`](./TROUBLESHOOTING.md) — SDK corruption, 1 Mbaud issue, remoteproc cache, etc.
- (memory: `project-m33-lpuart3-1mbaud.md`) — canonical M33 firmware state (SDK patches, STAT-register failure, paths forward, deployment cheatsheet). Lives in `~/.claude/projects/-home-ltdat-Desktop-mobturret/memory/`. The in-repo `PROGRESS.md` is archived.
  (out of date relative to MobTurret; for archaeology only)

# gun_controller — LPUART3 on FRDM-iMX93 with Zephyr

Everything specific to making the M33 core drive SC15 servos over LPUART3
on the FRDM-iMX93 board, using Zephyr's `nxp,imx-lpuart` driver.

If you only need the high-level project layout, see `../../CLAUDE.md` at the
repo root.

## TL;DR

The Zephyr build for `imx93_evk/mimx9352/m33` does **not** initialise the
CCM clock tree at boot. The four fixes below are mandatory for any
peripheral to work (not just LPUART3):

1. **`imx93_m33_clock_init` SYS_INIT hook** in `src/main.cpp` (PRE_KERNEL_1,
   priority 0) — configures clock root source/divider + IP gate for every
   peripheral you use. Without this, even the console is silent.
2. **App overlay** at `boards/imx93_evk_mimx9352_m33.overlay` — declares
   the `lpuart3` soc node (base `0x42570000`, IRQ 68) and the
   `uart3_default` pinctrl group under `&pinctrl { … }`.
3. **`prj.conf`** — `CONFIG_SERIAL=y` + `CONFIG_UART_MCUX_LPUART=y`.
4. **Linux remoteproc workflow** — the firmware is loaded by Linux on the
   A55, not flashed standalone. See "Deploy via Linux" below.

## Files

| File | Purpose |
|---|---|
| `src/main.cpp` | SYS_INIT clock hook + servo ping thread with loopback + retry |
| `boards/imx93_evk_mimx9352_m33.overlay` | lpuart3 node + uart3_default pinctrl |
| `prj.conf` | CONFIG_SERIAL, CONFIG_UART_MCUX_LPUART, etc. |
| `README.rst` | High-level overview (run loopback first, then broadcast ping) |
| `sample.yaml` | Twister harness config — regex matches `printk()` output |
| `PLANE.md` (root) | Project status checklist |

## Build

```bash
cd m33_firmware/gun_controller
cmake --preset=debug
cmake --build --preset=debug
```

Output: `debug/zephyr/zephyr.elf` (gdb), `debug/zephyr/zephyr.bin` (raw).

If the overlay isn't being picked up (`debug/zephyr/zephyr.dts` has no
`lpuart3` node), clear the cmake cache:

```bash
rm -rf debug/CMakeCache.txt debug/CMakeFiles
cmake --preset=debug
cmake --build --preset=debug
```

## Deploy via Linux remoteproc

The FRDM-iMX93 typically runs Linux on the A55, which loads the M33
firmware via remoteproc. The M33 binary must be an **ELF** (not raw `.bin`),
and must be wrapped or named to be remoteproc-compatible.

```bash
# On the FRDM Linux shell (root@imx93frdm)
echo stop > /sys/class/remoteproc/remoteproc0/state
# Wait for state to actually become "offline"
while [ "$(cat /sys/class/remoteproc/remoteproc0/state)" != "offline" ]; do
    sleep 0.5
done

cp /path/to/zephyr.elf /lib/firmware/

# If the firmware name matches what's already loaded, the kernel caches the
# blob and will reject the write with EBUSY. Either rename:
cp /path/to/zephyr.elf /lib/firmware/zephyr_v2.elf
echo /lib/firmware/zephyr_v2.elf > /sys/class/remoteproc/remoteproc0/firmware
# Or unbind/rebind the platform device to drop the cache.

echo start > /sys/class/remoteproc/remoteproc0/state
```

## Console output caveat

The M33's `printk()` goes to **LPUART2**. When Linux is running on A55,
LPUART2 is owned by Linux's debug console — M33 printk either produces no
visible output, or fights with Linux's writes. Three workarounds:

- **RPMsg log forwarding** (clean): enable `CONFIG_LOG_BACKEND_RPMSG=y`,
  read from `/dev/ttyRPMSG0` on the A55 side.
- **Dedicated UART** for M33 console: change the overlay's
  `chosen { zephyr,console = &lpuart4; }` and wire a USB-UART adapter to
  the LPUART4 pins.
- **Standalone boot** (no Linux): flash `zephyr.bin` to M33 TCM via
  JLink, `LPUART2` console works normally.

## Why the SYS_INIT clock hook is mandatory

The Zephyr iMX93 M33 soc port (`nxp_zephyr/zephyr/soc/nxp/imx/imx9/imx93/m33/`)
only zeroes DTCM and clears a sleep-hold bit. **No CCM setup**.

Compare to MCUXpresso's `hello_lpuart3.c`, which calls `BOARD_InitHardware()`
in `main()` — that function explicitly does:
```c
CLOCK_SetRootClock(kCLOCK_Root_Lpuart2, &rootCfg);  // console
CLOCK_SetRootClock(kCLOCK_Root_Lpuart3, &rootCfg);  // servo
CLOCK_EnableClock(kCLOCK_Lpuart2);
CLOCK_EnableClock(kCLOCK_Lpuart3);
```

Without the equivalent Zephyr hook:
- LPUART2 clock root is OFF → console silent → looks like hang
- LPUART3 clock root is OFF → `uart_poll_out()` hangs on TDRE

The `imx93_m33_clock_init()` function in `src/main.cpp` is that
equivalent. It must run at PRE_KERNEL_1 priority 0 — earlier than the
LPUART driver's `CONFIG_SERIAL_INIT_PRIORITY = 50`.

### Why the SDK clock root reset default is OFF

On iMX93, every clock root's `CLOCK_ROOT_CONTROL.RW` has the OFF bit set
after reset. The Zephyr clock driver (`clock_control_mcux_ccm_rev2.c`)
only flips the IP gate (`CLOCK_EnableClock` → LPCG), it does **not** call
`CLOCK_SetRootClock` to clear the OFF bit and configure mux/div. This is
a gap in this Zephyr fork's clock driver.

## Overlay gotchas (these are easy to get wrong)

### 1. Overlay filename must match the BOARD

Board is `imx93_evk/mimx9352/m33`. Zephyr's `zephyr_file()` resolves the
overlay filename with `/` → `_` substitution, so the file MUST be:
```
boards/imx93_evk_mimx9352_m33.overlay
```
A file named `frdm_imx93_mimx9352_m33.overlay` (matching the hardware) is
**not** picked up — Zephyr's build doesn't know your hardware, only the
build target.

### 2. Pinctrl must extend the iomuxc container, not a new root

The iMX93 soc DTS defines `pinctrl` at `/soc/iomuxc@443c0000/pinctrl`. So:
```dts
&pinctrl {                         /* ✅ extends the iomuxc container */
    uart3_default: uart3_default { … };
};
```
NOT:
```dts
/ {                                /* ❌ creates a separate root-level pinctrl */
    pinctrl {
        uart3_default: uart3_default { … };
    };
};
```
If you use the wrong form, Zephyr doesn't generate the per-node
`*_P_pinmux_FOREACH_PROP_ELEM` macros, and the build fails at the
`pinctrl_soc.h:74` line with `'Z_PINCTRL_STATE_PIN_INIT' undeclared`.

### 3. `current-speed` and `pinctrl-names = "default"` are required

The `nxp,imx-lpuart` driver reads `current-speed` to set baud, and
`pinctrl-names = "default"` is required by `PINCTRL_DT_INST_DEV_CONFIG_GET`.
Without either, the driver fails to bind or hangs.

### 4. LPUART3 IRQ is 68 (NVIC), not a GIC SPI

The M33 view uses ARMv8-M NVIC, not GIC. Format is `<N M>` = `<irq priority>`.
Use `interrupts = <68 3>;` (NVIC #68, priority 3 — matching `lpuart2`'s
priority choice in `nxp_imx93_m33.dtsi:105`).

## Verification steps (in order)

Run these in order; each confirms one layer:

1. **Zephyr banner on LPUART2** — `*** Booting Zephyr OS build … ***`
   → clock init hook ran, LPUART2 root + gate up.

2. **`LPUART3: device ready @ serial@42570000`** — driver bound, base
   correct. If this fails, the issue is overlay/devicetree.

3. **Loopback test** — wire GPIO_14 ↔ GPIO_15, look for
   `LPUART3: loopback OK (8 bytes echoed)`. Proves TX→RX round trip
   end-to-end. If this fails but step 2 passes, the issue is the wire or
   pinmux.

4. **Servo ping with broadcast ID** (`0xFE`) at **1,000,000 baud** (SC15
   default — `current-speed = <1000000>` in the overlay). Without knowing the
   servo ID, broadcast pings every servo on the bus. Look for `LPUART3: RX=0x..`
   bytes — the third byte is the servo's actual ID.

   Currently 2 servos on the bench: ID 1 and ID 2 (both model 3845).

5. **Servo ping with unicast ID** — once step 4 reveals the ID, hard-code
   it and verify `Received N bytes` consistently.

## Known limitations

- **No console output via Linux serial** when running under A55 Linux (see
  "Console output caveat" above).
- **No `frdm_imx93/mimx9352/m33` board port** exists in this Zephyr fork —
  we reuse the EVK port. The SoC-level differences between EVK and FRDM
  matter for board peripherals (sensors, connectors), not for the M33
  subsystem. Adding a proper FRDM M33 board port is a separate task.
- **No power management** — `pm_policy_state_lock_get()` in the LPUART
  driver keeps the M33 out of low-power states while transmitting. This
  is fine for a development build; revisit when adding sleep modes.
- **Bare-metal `bare_lpuart3.h` was removed** — all UART access now goes
  through Zephyr's API (`uart_poll_out` / `uart_poll_in`). The
  `mcusdk_m33_firmware/hello_lpuart3.c` test still works as a physical-
  layer sanity check.

## What's where in the code

```cpp
// src/main.cpp (in order)
imx93_m33_clock_init()                  // SYS_INIT PRE_KERNEL_1, prio 0
run_loopback_test(uart)                 // sends "ZEPHYR\r\n", verifies echo
servo_ping_thread()                     // SCSCL ping loop, broadcast ID 0xFE
```

```dts
// boards/imx93_evk_mimx9352_m33.overlay
soc { lpuart3: serial@42570000 { … } }
&pinctrl { uart3_default { group0 { pinmux, bias-pull-up, slew-rate, drive-strength } } }
&lpuart3 { status = "okay"; current-speed = <115200>; pinctrl-0; pinctrl-names }
```
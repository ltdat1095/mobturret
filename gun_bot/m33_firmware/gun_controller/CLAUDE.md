# gun_controller — LPUART3 on FRDM-iMX93 with Zephyr

Everything specific to making the M33 core drive SC15 servos over LPUART3
on the FRDM-iMX93 board, using Zephyr's `nxp,imx-lpuart` driver.

If you only need the high-level project layout, see `../../CLAUDE.md` at the
repo root.

> **STATUS (2026-06-28):** **Pure-Zephyr path implemented.** `main.cpp`
> uses `uart_poll_in`/`uart_poll_out` against a DTS-bound LPUART3 — no
> baremetal SDK calls. Fork patches (lpuart3 dtsi node, `uart3_default`
> pinctrl, LPUART cases in `clock_control_mcux_ccm_rev2.c`) landed in
> `~/mobturret-forks/zephyr`. Build succeeds (`debug/zephyr/zephyr.elf`,
> 35,976 bytes FLASH). **Hardware verification still pending** — deploy
> via the remoteproc dance and confirm 1 Mbaud loopback PASS counts.
> See `PURE_ZEPHYR_STATUS.md` (the "Pure-Zephyr migration — DONE" section
> at the bottom) and `~/.claude/projects/-home-ltdat-Desktop-mobturret/memory/project-zephyr-fork-migration.md`.
>
> **All session state lives in MCP memory** at
> `~/.claude/projects/-home-ltdat-Desktop-mobturret/memory/`
> (see `project-m33-lpuart3-1mbaud.md` and `project-zephyr-fork-migration.md`).
> The in-repo `PROGRESS.md` is **archived** (frozen 2026-06-28); update memory, not PROGRESS.md.

## TL;DR

The pure-Zephyr path drives LPUART3 via Zephyr's `mcux_lpuart` driver
bound to a devicetree node. The fix-up set is:

1. **Fork patches** in `~/mobturret-forks/zephyr`:
   - `zephyr/dts/arm/nxp/imx/nxp_imx93_m33.dtsi` declares `lpuart3`.
   - `zephyr/boards/nxp/imx93_evk/imx93_evk-pinctrl.dtsi` declares
     `uart3_default` (GPIO_14 = TX, GPIO_15 = RX).
   - `zephyr/drivers/clock_control/clock_control_mcux_ccm_rev2.c` adds
     `IMX_CCM_LPUART{1..8}_CLK` cases to `mcux_ccm_on`,
     `mcux_ccm_get_subsys_rate`, and `mcux_ccm_set_subsys_rate`.
2. **App overlay** `boards/imx93_evk_mimx9352_m33.overlay` — enables
   `&lpuart3` with `current-speed = <1000000>` and `pinctrl-0 = <&uart3_default>`.
3. **`src/main.cpp`** — uses `clock_control_configure()` for LPUART2/3
   clock roots (PRE_KERNEL_1 prio 0) and `uart_poll_in`/`uart_poll_out`
   for loopback. No `#include "fsl_*.h"`.
4. **`prj.conf`** — `CONFIG_SERIAL=y` + `CONFIG_UART_MCUX_LPUART=y` +
   `CONFIG_UART_INTERRUPT_DRIVEN=n` (the `n` is important).
5. **Linux remoteproc workflow** — the firmware is loaded by Linux on
   the A55, not flashed standalone. See "Deploy via Linux" below.

## Files

| File | Purpose |
|---|---|
| `src/main.cpp` | Pure-Zephyr: PRE_KERNEL_1 clock_configure hook + uart_poll loopback |
| `boards/imx93_evk_mimx9352_m33.overlay` | Enables `&lpuart3` with 1 Mbaud + `uart3_default` pinctrl |
| `prj.conf` | CONFIG_SERIAL, CONFIG_UART_MCUX_LPUART, CONFIG_UART_INTERRUPT_DRIVEN=n |
| (memory: `project-m33-lpuart3-1mbaud.md`) | **1 Mbaud diagnostic state** (SDK patches, register dumps, FIFO-depth finding). Lives in `~/.claude/projects/-home-ltdat-Desktop-mobturret/memory/`. |
| (memory: `project-zephyr-fork-migration.md`) | **Fork migration path map** — what lives where after the 2026-06-28 fork swap. |
| `PURE_ZEPHYR_STATUS.md` | The pure-zephyr migration journal: previous revert + the "DONE (2026-06-28)" section at the bottom with the full file list. |
| `fork-snapshots/FORK.md` | Fork-and-PR workflow doc. |
| `README.rst` | High-level overview |
| `sample.yaml` | Twister harness config — regex matches `printk()` output |

## Build

```bash
cd m33_firmware/gun_controller
cmake --preset=debug
cmake --build --preset=debug
```

Output: `debug/zephyr/zephyr.elf` (gdb), `debug/zephyr/zephyr.bin` (raw).

If the build fails with **baud-rate errors** (1 Mbaud not landing on
`BAUD=0x17000001`), the SDK baud-override patch has been lost. Re-apply
from
`fork-snapshots/nxp-zephyr-fork/0001-fsl_lpuart-1mbaud-baud-override-fork4.5.patch`
to the fork's `modules/hal/nxp/mcux/mcux-sdk-ng/drivers/lpuart/fsl_lpuart.c`
(lands at lines 435 and 850). The struct/enum `#if 0` blocks from the
old NXP-downstream patch are **not** needed on the upstream fork —
`lpuart_config_t` carries `inverseTxd`, `enableTxRTS`, `enableTxCTS`,
`txRtsPolarity` etc. on this SDK.

See memory `project-m33-lpuart3-1mbaud.md` §3 for md5s of expected file states.

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
# From the dev machine:
scp -o StrictHostKeyChecking=no \
  /home/ltdat/Desktop/mobturret/gun_bot/m33_firmware/gun_controller/debug/zephyr/zephyr.elf \
  root@192.168.1.94:/lib/firmware/zephyr_vN.elf

# On the FRDM Linux shell (root@imx93frdm, via mlan0 wifi ssh):
ssh -o StrictHostKeyChecking=no root@192.168.1.94 bash <<'EOF'
for i in 1 2 3 4 5 6 7 8; do
  s=$(cat /sys/class/remoteproc/remoteproc0/state)
  if [ "$s" = "offline" ]; then break; fi
  echo stop > /sys/class/remoteproc/remoteproc0/state
  sleep 3
done
echo "/lib/firmware/zephyr_vN.elf" > /sys/class/remoteproc/remoteproc0/firmware
echo start > /sys/class/remoteproc/remoteproc0/state
sleep 1
cat /sys/class/remoteproc/remoteproc0/state
dmesg | grep -E "remoteproc|imx-rproc" | tail -3
EOF
```

If the firmware name matches what's already loaded, the kernel caches the
blob and will reject the write with EBUSY. Either rename:
```bash
cp /path/to/zephyr.elf /lib/firmware/zephyr_v2.elf
echo /lib/firmware/zephyr_v2.elf > /sys/class/remoteproc/remoteproc0/firmware
```
Or unbind/rebind the platform device to drop the cache.

## Console output caveat (important)

The M33's `printk()` goes to **LPUART2**. When Linux is running on A55,
LPUART2 is owned by Linux's debug console — M33 printk either produces no
visible output, or fights with Linux's writes.

**The fix:** read from `ttyACM1` (the QinHeng dual-CDC adapter's channel B
on the host), NOT `ttyACM0` (which is A55's debug console).

```bash
# Stop A55 getty first so it stops fighting M33 for LPUART2:
ssh -o StrictHostKeyChecking=no root@192.168.1.94 \
  'systemctl stop serial-getty@ttyLP0.service; echo 0 > /proc/sys/kernel/printk'

# Read M33 printk output (requires sudo — NOPASSWD rule for python3):
sudo /usr/bin/python3 -c "
import serial, time
with serial.Serial('/dev/ttyACM1', 115200, timeout=0.5) as s:
    s.reset_input_buffer()
    t0 = time.monotonic()
    total = b''
    while time.monotonic() - t0 < 12.0:
        chunk = s.read(2048)
        if chunk:
            total += chunk
print(total.decode('utf-8', errors='replace'))
"
```

Three other workarounds if you can't use the above:
- **RPMsg log forwarding** (clean): enable `CONFIG_LOG_BACKEND_RPMSG=y`,
  read from `/dev/ttyRPMSG0` on the A55 side.
- **Dedicated UART** for M33 console: change the overlay's
  `chosen { zephyr,console = &lpuart4; }` and wire a USB-UART adapter to
  the LPUART4 pins.
- **Standalone boot** (no Linux): flash `zephyr.bin` to M33 TCM via
  JLink, `LPUART2` console works normally.

## Why the SYS_INIT clock hook is mandatory

The Zephyr iMX93 M33 soc port (`nxp_zephyr/soc/nxp/imx/imx9/imx93/m33/`,
resolving through the symlink to
`~/mobturret-forks/zephyr/soc/nxp/imx/imx9/imx93/m33/`) only zeroes DTCM
and clears a sleep-hold bit. **No CCM setup**.

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

## SDK baud-rate patch (in fork's `modules/hal/nxp/.../fsl_lpuart.c`)

The SDK's baud search loop on i.MX93 M33 with 24 MHz clock + 1 Mbaud target
produces `BAUD=0x00000404` (SBR=1028, OSR_field=0) which gives ~820 kbaud.

**Patch lands at line 435 (`LPUART_Init`) and line 850 (`LPUART_SetBaudRate`):**

```c
/* MobTurret workaround: SDK's baud search produces wrong SBR/OSR for
 * 24 MHz + 1 Mbaud on iMX93 M33 (yields SBR=1028, OSR=0). Hardcode
 * the correct values: 24 MHz / (24 * 1) = 1 MHz exactly.
 * BAUD register = (OSR-1)<<24 | SBR = (23<<24) | 1 = 0x17000001. */
if (srcClock_Hz == 24000000U && baudRate_Bps == 1000000U)
{
    osr      = 24U;
    sbr      = 1U;
    baudDiff = 0U;
}
```

For `LPUART_Init` the variable is `config->baudRate_Bps`; for
`LPUART_SetBaudRate` it's the bare parameter `baudRate_Bps`.

The patch lives at
`fork-snapshots/nxp-zephyr-fork/0001-fsl_lpuart-1mbaud-baud-override-fork4.5.patch`
and is applied against the user's fork at
`~/mobturret-forks/modules/hal/nxp` (a west submodule of
`~/mobturret-forks/zephyr`).

There's also a `g_baud_after_fixup` global in `src/main.cpp` that's read
back after `imx93_lpuart3_baud_fixup` (PRE_KERNEL_1 prio 60) writes BAUD
directly to LPUART3 — verifies the fixup actually ran.

### SDK struct/enum stripping — NOT NEEDED on the fork

The previous NXP-downstream fork (`nxp-zephyr/zephyr` `nxp-v4.3.0`)
lacked `inverseTxd`, `enableTxRTS`, `enableTxCTS`, `txRtsPolarity`,
`kLPUART_CtsSampleAtStart`, and `kLPUART_CtsSourcePin` in i.MX93's
`lpuart_config_t`, requiring `#if 0` blocks in `fsl_lpuart.c` around
those references. **The upstream fork's SDK has all of these** (see
`modules/hal/nxp/mcux/mcux-sdk-ng/drivers/lpuart/fsl_lpuart.h` — fields
at lines 264-269, 281, enums at lines 87, 94, 101), so no stripping is
needed.

## Verification steps (in order)

Run these in order; each confirms one layer:

1. **Zephyr banner on LPUART2** — `*** Booting Zephyr OS build … ***`
   → clock init hook ran, LPUART2 root + gate up.

2. **M33 printk visible on ttyACM1 @ 115200** — first thing main.cpp
   prints is `LPBK: ...` (or similar). If absent, console collision or
   missing clock init.

3. **BAUD register check** — printed in LPBK output:
   ```
   LPBK: FAIL baud=0x17000001 ...
   ```
   If BAUD != `0x17000001`, the SDK patch didn't take.

4. **Loopback test** — wire GPIO_14 ↔ GPIO_15, look for `LPBK: PASS`.
   **In Zephyr this currently fails** with STAT error flags. The standalone
   `mcimx93_evk_blank/hello_lpuart3.c` PASSES — see memory `project-m33-lpuart3-1mbaud.md` §6.

## Known limitations

- **No console output via Linux serial** when running under A55 Linux (see
  "Console output caveat" above).
- **No `frdm_imx93/mimx9352/m33` board port** exists in this Zephyr fork —
  we reuse the EVK port. The SoC-level differences between EVK and FRDM
  matter for board peripherals (sensors, connectors), not for the M33
  subsystem. Adding a proper FRDM M33 board port is a separate task.
- **Loopback in Zephyr firmware fails** — see memory `project-m33-lpuart3-1mbaud.md` §7
  for diagnostic detail and the two paths forward. Workaround: use
  `mcusdk_m33_firmware/mcimx93_evk_blank/` for now.
- **No power management** — `pm_policy_state_lock_get()` in the LPUART
  driver keeps the M33 out of low-power states while transmitting. This is
  fine for a development build; revisit when adding sleep modes.
- **`bare_lpuart3.h` removed** — all UART access now goes through
  Zephyr's API (`uart_poll_out` / `uart_poll_in`). The standalone
  `mcusdk_m33_firmware/hello_lpuart3.c` test still works as a
  physical-layer sanity check.

## What's where in the code

```cpp
// src/main.cpp (in order)
imx93_m33_clock_init()                  // SYS_INIT PRE_KERNEL_1, prio 0 — clock root + IP gate
wire_test_init()                        // SYS_INIT PRE_KERNEL_1, prio 1 — RGPIO wire probe (inconclusive on i.MX93)
imx93_lpuart3_baud_fixup()              // SYS_INIT PRE_KERNEL_1, prio 60 — BAUD = 0x17000001
direct_lpuart3_reinit()                 // SYS_INIT PRE_KERNEL_1, prio 70 — full LPUART_Init + STAT clear
run_loopback_test_direct()              // LPUART_WriteBlocking + LPUART_GetStatusFlags (baremetal SDK)
loopback_thread()                       // K_THREAD — runs loopback test forever, prints LPBK: PASS|FAIL
```

```dts
// boards/imx93_evk_mimx9352_m33.overlay
// (intentionally empty — comment-only. See memory `project-m33-lpuart3-1mbaud.md` §4 for why.)
```
# MobTurret M33 Firmware — Progress Notes

> ⚠️ **ARCHIVED 2026-06-28 — DO NOT UPDATE.**
>
> This file is a frozen historical snapshot. The canonical, live source
> for M33 firmware state is the Claude memory file:
>
> ```
> ~/.claude/projects/-home-ltdat-Desktop-mobturret/memory/project-m33-lpuart3-1mbaud.md
> ```
>
> When you make a discovery (build, deploy, register dump, fix attempt),
> update the memory file — not this one. The two are now in sync as of
> the archive date; future drift should land in memory.

**Date of original session:** 2026-06-28
**Goal:** Drive SC15 servos at **1 Mbaud** over LPUART3 on FRDM-iMX93 M33, with A55 Linux running and managing M33 via remoteproc.

---

## TL;DR

**The wire is fine. The 1 Mbaud SDK patch works. The standalone baremetal path passes loopback cleanly. The Zephyr driver path leaves STAT register error flags set, breaking loopback in Zephyr. Next session: pick one of two paths forward (see §7).**

**Verified results:**
- `mcimx93_evk_blank/debug/mcimx93_evk_blank_cm33.elf` (baremetal MCUXpresso SDK) → **2000+ loopback PASSes at 1 Mbaud**
- `gun_controller/debug/zephyr/zephyr.elf` (Zephyr) → 0 loopback PASSes, STAT has RX error flags

---

## 1. Hardware

- **Board:** FRDM-iMX93
- **M33 core:** Cortex-M33 (MIMX9352)
- **A55 core:** Cortex-A55 running Linux 6.6.36, manages M33 via `/sys/class/remoteproc/remoteproc0/`
- **Servo bus:** LPUART3 on M33, **two-pin mode** (TX on GPIO_IO14, RX on GPIO_IO15)
- **Wire:** GPIO_14 ↔ GPIO_15 physically shorted on the bench (TX echoes back to RX)
- **Debug console:** LPUART2 on M33, exposed on **ttyACM1 @ 115200** on the host (the QinHeng dual-CDC adapter's channel B). Reading M33 printk output requires `sudo /usr/bin/python3 -c '...serial.Serial("/dev/ttyACM1", 115200)...'`
- **A55 access:** `ssh root@192.168.1.94` (mlan0 wifi, IP from DHCP)

## 2. Build environment

| Tool | Path |
|---|---|
| CMake | `/home/ltdat/.mcuxpressotools/cmake-3.30.0-linux-x86_64/bin/cmake` |
| Ninja | `/home/ltdat/.mcuxpressotools/ninja-1.12.1/ninja` |
| Zephyr SDK | `~/zephyr-sdk-0.17.4` |
| Zephyr base (M33) | `mobturret/gun_bot/m33_firmware/nxp_zephyr/zephyr` |
| ARM toolchain | `/home/ltdat/.mcuxpressotools/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-*` |
| MCUXpresso SDK | `mobturret/gun_bot/mcusdk_m33_firmware/sdks/mcimx93_evk_blank_sdk/mcuxsdk` |

`/etc/sudoers.d/serial_io` grants `ltdat ALL=(ALL) NOPASSWD: /usr/bin/python3` for the ttyACM1 capture script.

## 3. The 1 Mbaud SDK patch — applied to `nxp_zephyr/modules/hal/nxp/mcux/mcux-sdk-ng/drivers/lpuart/fsl_lpuart.c`

**md5 of patched file:** `eaad18a668f98541e32d5f46801b9c9f`
**md5 of clean upstream:** `a17206cdcae8ba5ea234e6de5d47e9a4`

### 3.1 Baud override (in both `LPUART_Init` and `LPUART_SetBaudRate`)

SDK's baud search loop on i.MX93 M33 with 24 MHz clock + 1 Mbaud target produces BAUD=0x00000404 (SBR=1028, OSR_field=0) which gives ~820 kbaud. The patch hardcodes the correct values for this specific combination.

After the search loop, before the 3% check:

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

For `LPUART_Init` the variable name is `config->baudRate_Bps` (line 439); for `LPUART_SetBaudRate` it's the bare parameter `baudRate_Bps` (line 859).

### 3.2 Header fields stripped

The upstream `fsl_lpuart.c` references struct members (`config->inverseTxd`, `config->enableTxRTS`, `config->txRtsPolarity`) and an enum value (`kLPUART_RtsPolarityLow`) that don't exist in the i.MX93 `lpuart_config_t` struct or any enum. Build fails with these. Fix: wrap the references in `#if 0 / * MobTurret: ... * / #endif` blocks.

Four blocks at lines 540, 595, 601, 767 of `fsl_lpuart.c`. Comments mark each one.

### 3.3 Header (NOT patched)

`fsl_lpuart.h` md5: `b37f7b6159bd2c6a04f0546f6963b721` — this is the **clean upstream** (does not add the missing fields/enum). We tried patching it earlier to add `enableTxRTS`, `txRtsPolarity`, `inverseTxd` and the enum, but reverted to clean upstream because the `#if 0` strip approach in the .c file is simpler.

If you re-run `cmake --build` and it fails with `inverseTxd`, `enableTxRTS`, etc. errors, that means someone removed the `#if 0` blocks. Re-add them — see §3.2.

## 4. Zephyr overlay

**File:** `mobturret/gun_bot/m33_firmware/gun_controller/boards/imx93_evk_mimx9352_m33.overlay`
**md5:** `bf0978ef28565eb1733e6a139a29f096`
**Lines:** 19 (just a comment block — no actual DT nodes)

**The overlay is INTENTIONALLY EMPTY** (comment-only). Earlier we had `lpuart3` declared with `pinctrl-0`, etc., but Zephyr's LPUART driver path was leaving STAT register error flags set and breaking loopback (see §6). Current strategy: main.cpp drives LPUART3 directly via the baremetal MCUXpresso SDK at PRE_KERNEL_1 prio 70, bypassing the Zephyr driver entirely.

If you want the Zephyr driver to bind to LPUART3, restore the `lpuart3` node in this overlay — but you will then need to debug why the Zephyr path leaves STAT error flags set (see §6).

## 5. prj.conf

```
CONFIG_PRINTK=y
CONFIG_HEAP_MEM_POOL_SIZE=256
CONFIG_ASSERT=y
CONFIG_GPIO=y
CONFIG_CPP=y
CONFIG_SERIAL=y
CONFIG_UART_MCUX_LPUART=y
CONFIG_GPIO=y                            # (note: duplicated)
CONFIG_UART_INTERRUPT_DRIVEN=n           # disabled — see below
```

`CONFIG_UART_INTERRUPT_DRIVEN=n` is set to avoid Zephyr's LPUART driver installing an IRQ handler. With IRQ enabled, RX error flags were left set. Disabling interrupt-driven mode (and so falling back to bare polling) doesn't fully fix the Zephyr issue (see §6) but is the cleaner config regardless.

## 6. main.cpp — current state

**File:** `mobturret/gun_bot/m33_firmware/gun_controller/src/main.cpp`

Three SYS_INIT hooks:

1. **PRE_KERNEL_1 prio 0** — `imx93_m33_clock_init`: opens LPUART2 + LPUART3 clock roots and IP gates.
2. **PRE_KERNEL_1 prio 1** — `wire_test_init`: tries to drive GPIO_14 and read GPIO_15 via RGPIO registers. **This test is INCONCLUSIVE on i.MX93** — the IOMUX isn't in GPIO mode at PRE_KERNEL_1 prio 1 (UART driver hasn't run yet), so RGPIO reads return 0x00000000 even when the wire IS connected. Don't rely on this for wire verification — use the standalone hello_lpuart3 instead.
3. **PRE_KERNEL_1 prio 60** — `imx93_lpuart3_baud_fixup`: writes `BAUD = (23 << 24) | 1` to LPUART3_BASE directly. Sets `g_baud_after_fixup` to read-back value (used to confirm the fixup ran).
4. **PRE_KERNEL_1 prio 70** — `direct_lpuart3_reinit`: re-initializes LPUART3 from scratch using the MCUXpresso SDK (`LPUART_Init`), with TX DSE=15, RX PD_MASK pin config, and clears STAT error bits. Stores snapshot in `g_reinit_stat` / `g_reinit_fifo`.

Thread `loopback_thread`:
- Runs the wire-test display (now mostly bypassed)
- Calls `run_loopback_test_direct()` which uses `LPUART_WriteBlocking` + `LPUART_GetStatusFlags` directly
- Loops printing LPBK: PASS/FAIL diagnostics

**Builds, deploys, M33 boots — but loopback test FAILS** with `STAT=0x40d80000` and `RDRF=0`.

## 7. The fundamental problem (status at end of session)

Same hardware, same wire, same peripheral. Two firmwares:

| | Zephyr (`gun_controller`) | Standalone (`mcimx93_evk_blank`) |
|---|---|---|
| Source SDK | Patched `nxp_zephyr/.../fsl_lpuart.c` | Clean upstream `sdks/.../fsl_lpuart.c` |
| Baud patch applied? | Yes (at PRE_KERNEL_1 prio 60 + prio 70 reinit) | Yes (after `LPUART_Init`, direct BAUD write) |
| BAUD register at runtime | `0x17000001` ✓ | `0x17000001` ✓ |
| CTRL register | `0x000c0000` (TE\|RE) | (similar) |
| STAT register | **`0x40d80000`** (PF\|FE\|NF\|OR all set) | `0x40D000C0` (clean) |
| Loopback test | **FAIL** (RDRF stays 0) | **PASS** (2000+ iterations) |

Zephyr's STAT register has accumulated RX error flags that the W1C clear can't reset. We tried:
- Removing the `lpuart3` node from devicetree entirely (no Zephyr driver binding)
- Manually clearing STAT via `STAT = 0x000F0000` (write 1 to clear PF|FE|NF|OR) — doesn't take effect
- Disabling `CONFIG_UART_INTERRUPT_DRIVEN` — no help
- Trying lower baud rates (115200) — still fails with same STAT pattern
- Removing all pinctrl customisation — no help
- Bypassing the Zephyr driver entirely with direct LPUART3 register access — still fails

The error bits are still set when the thread runs, despite our PRE_KERNEL_1 prio 70 hook clearing them right after `LPUART_Init`. Something between prio 70 and the thread running (or maybe the thread running itself) re-sets them.

**Hypothesis (unverified):** `LPUART_WriteBlocking` polls TDRE per byte. Each TX of the wire short might be triggering RX error sampling because of some timing skew between TX clock domain and RX clock domain. But standalone uses the exact same `LPUART_WriteBlocking` and works. So this is probably wrong.

More likely hypothesis: **Linux (A55) is touching LPUART3 registers somehow**, even though the DT says `status="disabled"` for LPUART3. To verify: stop the A55 Linux getty/service completely, or test with the FRDM not connected to a host (Linux off), or check `ls -la /sys/bus/platform/drivers/fsl-lpuart/`.

Confirmed earlier:
- `cat /sys/firmware/devicetree/base/soc@0/bus@42000000/serial@42570000/status` → `disabled` ✓
- `ls /sys/bus/platform/drivers/fsl-lpuart/` → only `42590000.serial` (LPUART5) and `44380000.serial` (LPUART1). No LPUART3 driver bound.

So A55 doesn't bind LPUART3. But Linux might still touch the peripheral registers through some other path (clock driver? pinctrl driver?).

## 8. Two paths forward for next session

### Path A — Standalone for servo control (RECOMMENDED, ~2 hours)

Use `mcimx93_evk_blank/hello_lpuart3.c` as the base for servo control firmware.

1. The file already passes loopback at 1 Mbaud (verified).
2. Add SCSCL protocol on top (the SCServo library is already in `gun_controller/src/servo/` from earlier sessions — copy the relevant files into the standalone project).
3. Build with MCUXpresso armgcc toolchain (already working).
4. Deploy via remoteproc (already working).
5. Output goes to ttyACM1 (debug console) for monitoring.

This gives you a working servo bus FAST. The Zephyr driver issue becomes a separate ticket.

### Path B — Fix Zephyr (more work, 1-3 days)

Things to try next:

1. **Verify Linux isn't the culprit** — kill A55 Linux completely (boot to u-boot, then load M33 firmware standalone via JLink or u-boot `rproc` command) and run Zephyr from there. If loopback works without Linux, we know Linux is touching LPUART3 somehow.

2. **Diff the two init flows** — instrument both firmwares to dump LPUART3 register state at every step (after each init hook, after each `LPUART_Init`, after each `IOMUXC_SetPinMux`). Find where the error bits first appear.

3. **Try different SDK init patterns** — e.g., `LPUART_SoftwareReset` before init, or set CTRL bits manually instead of via `LPUART_Init`.

4. **Try different FIFO settings** — set `txFifoWatermark = 1` (not 0), disable FIFO entirely via `FIFO = 0`.

5. **Try using the same `BOARD_InitHardware()` pattern as standalone** — Zephyr has its own BOARD_InitHardware; maybe it's missing something the MCUXpresso one has.

## 9. Deployment cheatsheet

```bash
# Push and start Zephyr firmware
scp -o StrictHostKeyChecking=no \
  /home/ltdat/Desktop/mobturret/gun_bot/m33_firmware/gun_controller/debug/zephyr/zephyr.elf \
  root@192.168.1.94:/lib/firmware/zephyr_vN.elf
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
echo "state: $(cat /sys/class/remoteproc/remoteproc0/state)"
dmesg | grep -E "remoteproc|imx-rproc" | tail -3
EOF

# Capture M33 printk (debug console = ttyACM1 @ 115200)
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

# Push and start standalone baremetal
scp -o StrictHostKeyChecking=no \
  /home/ltdat/Desktop/mobturret/gun_bot/mcusdk_m33_firmware/mcimx93_evk_blank/debug/mcimx93_evk_blank_cm33.elf \
  root@192.168.1.94:/lib/firmware/standalone.elf
# (then same remoteproc dance)
```

## 10. Files to know about

- `mobturret/gun_bot/m33_firmware/gun_controller/CLAUDE.md` — Zephyr build/env docs
- `mobturret/gun_bot/m33_firmware/gun_controller/PROGRESS.md` — **this file**
- `mobturret/gun_bot/m33_firmware/gun_controller/main.cpp` (wait, actually `src/main.cpp`) — current loopback firmware
- `mobturret/gun_bot/m33_firmware/nxp_zephyr/modules/hal/nxp/mcux/mcux-sdk-ng/drivers/lpuart/fsl_lpuart.c` — **the patched SDK file** (don't overwrite!)
- `mobturret/gun_bot/mcusdk_m33_firmware/mcimx93_evk_blank/hello_lpuart3.c` — the WORKING standalone (modify this for servo control)
- `mobturret/gun_bot/m33_firmware/gun_controller/boards/imx93_evk_mimx9352_m33.overlay` — currently empty (comment only)
- `physical_gunbound/gun_bot/m33_firmware/...` — old project on `/home/ltdat/Desktop/physical_gunbound/...`, had the same SDK bug but with the trivial-true loopback test (so loopback test always passed without actually verifying anything)
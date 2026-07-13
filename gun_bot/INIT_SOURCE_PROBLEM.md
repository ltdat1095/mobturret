# M33 firmware — initial problems, root causes, and fixes

This is the **one** document for the M33 firmware investigation. It records
the problems encountered during initial bring-up (build, log, LPUART3
at 1 Mbaud), their root causes, and the fixes that produced the working
firmware recipe.

It is **not** a build or deploy manual. The build flow lives in
`m33_firmware/gun_controller/CMakePresets.json` plus the standard
`cmake --preset=debug && cmake --build --preset=debug` invocation. The
main artifact of this subproject — the A55 ↔ M33 wire contract — is
in [`./IPC.md`](./IPC.md), which is the canonical IPC reference.

---

## 1. Initial problems

Five problems blocked the M33 firmware from working. They were
investigated in the order listed; each one fed the next.

| # | Symptom | Where observed |
|---|---|---|
| 1 | LPUART3 measured ~820 kbaud instead of 1,000,000 baud | Oscilloscope on GPIO_IO14 |
| 2 | M33 `printk` output never appeared on `/dev/ttyACM1` despite the Zephyr banner showing once | `sudo cat /dev/ttyACM1` on host |
| 3 | Deploying *any* M33 firmware (`echo start`) caused the A55 wifi to drop and the SoC to watchdog-reboot ~10 minutes later | `ping 192.168.1.94` after deploy |
| 4 | The NXP-downstream Zephyr fork on disk (pre-2026-06-28) had accumulated broken `fsl_lpuart.c` edits that no longer compiled | `cmake --build` failure |
| 5 | Several `git submodule`/`west` permutations existed on the dev host; `ZEPHYR_SDK_INSTALL_DIR=0.17.4` was declared but cmake always picked SDK 1.0.1 | `cmake` configure log |

The five are interrelated. Fixing #1 forced #2 to be investigated.
Fixing #3 required #4 to be resolved first. Fixing #5 is what made
the new fix stick.

---

## 2. Root cause analysis

### 2.1 LPUART3 measures 820 kbaud instead of 1 Mbaud

**Root cause:** the NXP MCUXpresso SDK's `LPUART_Init` does a search for
the `(OSR, SBR)` pair that yields a baud closest to the target within
3 %. The search table does not have a viable entry for **24 MHz source
clock + 1 Mbaud target** on the i.MX93 M33. It returns `(OSR=0, SBR=1028)`
which produces `BAUD = 0x00000404` — a baud rate of roughly 820 kbaud.

The bare-metal MCUXpresso path works because it configures the clock
tree differently (SystemCoreClock is 235 MHz on the EVK with the
default PLL config, not the 24 MHz XTAL pass-through our LPUART3 clock
root uses).

**Where the search lives:** `fsl_lpuart.c` in
`hal_nxp/mcux/mcux-sdk-ng/drivers/lpuart/`. Specifically the baud-search
loop inside `LPUART_Init()` (one loop) and `LPUART_SetBaudRate()`
(a second loop).

### 2.2 `printk` doesn't appear on `/dev/ttyACM1`

**Root cause:** LPUART2 is multiplexed onto the same physical pins as
the A55's debug UART (`/dev/ttyACM0`). When A55 Linux is running with
its getty on `/etc/init.d/serial-getty@ttyLP0.service`, both sides
drive the wire. The M33's `printk` writes either interleave with A55
output, fight the A55 driver for the bus, or get masked by A55-side
buffering. **The banner `*** Booting Zephyr OS build … ***` is the
one bit that escapes** because it is a single-character-at-a-time
write with no buffering, which races through cleanly.

This is **not** a clock-root or driver-binding bug. LPUART2 is
correctly bound and driven; the data simply doesn't reach `/dev/ttyACM1`
because the A55 has it.

### 2.3 Any M33 firmware crashes the SoC

**Root cause:** Zephyr's i.MX93 M33 soc port only zeroes DTCM and clears
a sleep-hold bit; it does **not** open clock roots. Worse, the Zephyr
clock driver `clock_control_mcux_ccm_rev2.c` in this fork has a **gap**:

- Its `DEVICE_API` struct does **not** declare a `.configure` member
  (`mcux_ccm_driver_api` only has `.on / .off / .get_rate / .set_rate`).
- The `mcux_ccm_on()` handler for `IMX_CCM_LPUART{1..8}_CLK` only
  calls `CLOCK_EnableClock(...)` (the LPCG IP gate) — it does **not**
  call `CLOCK_SetRootClock(...)` to clear the root's OFF bit or set
  mux/div.
- The LPUART driver's own init calls `clock_control_configure()`,
  gets `-ENOSYS` (because no `.configure`), then proceeds to
  `clock_control_on()` — which also only enables the IP gate.

Net effect: the LPUART clock **root** never comes up. On i.MX93, every
clock root's `CLOCK_ROOT_CONTROL.RW` defaults to OFF after POR; without
`CLOCK_SetRootClock(root, ...)` clearing that OFF bit and configuring
mux/div, the LPUART peripheral cannot clock its TX path. Any
`uart_poll_out()` therefore hangs on TDRE forever. The bus transaction
crosses into the A55 domain and the SoC watchdog trips — wifi drops
within seconds of `echo start` on the A55 Linux side.

This was diagnosed 2026-07-13 by comparing the broken mobturret
firmware against the working reference at
`~/Desktop/physical_gunbound/gun_bot/m33_firmware/gun_controller/`
(which uses SDK `CLOCK_SetRootClock` + `CLOCK_EnableClock` directly,
bypassing the Zephyr clock API entirely). The mobturret firmware had
the canonical Zephyr-side hook — but Zephyr's own clock driver is
incomplete, so the hook was effectively a no-op.

### 2.4 NXP-downstream `fsl_lpuart.c` is broken from accumulated edits

**Root cause:** before 2026-06-28, the project lived on
`nxp-zephyr/zephyr` at `nxp-v4.3.0` and patched `fsl_lpuart.c` locally
with chained sed/awk/python edits. The accumulated edits corrupted the
file: an extra `}`, a `config->inverseTxd = false;` write into a struct
that didn't have that field, references to `enableTxRTS` / `txRtsPolarity`
that weren't in `lpuart_config_t`, and a `kLPUART_RtsPolarityLow`
enum that wasn't defined. The file would not compile.

The fix for this was the **fork migration**: move MobTurret's
LPUART3 dtsi and clock-root patches to a *user's GitHub fork* of
upstream Zephyr (`ltdat1095/zephyr`), and apply the baud override
in a corresponding user fork of `hal_nxp`. Edits happen on the
upstream-style source via PRs instead of local patches.

### 2.5 `ZEPHYR_SDK_INSTALL_DIR=0.17.4` is overridden by CMake

**Root cause:** the fork's `cmake/modules/FindHostTools.cmake` does
`find_package(Zephyr-sdk 1.0)` (hard minimum). SDK 1.0.1's
`Zephyr-sdkConfigVersion.cmake` enforces
`ZEPHYR_SDK_MINIMUM_COMPATIBLE_VERSION 1.0`. CMake finds SDK 0.17.4
at the HINTS path, declares it incompatible, and falls through to
auto-discovery which picks SDK 1.0.1 from `$HOME`.

This is **correct behavior** — the fork requires SDK ≥ 1.0 and 1.0.1 is
the only SDK installed. SDK 0.17.4 still being declared in
`mcux_include.json` is informational only. SDK 1.0.1 builds the firmware
correctly; the bisect confirms it.

---

## 3. Fixes

### 3.1 The 1 Mbaud SDK patch (problem 2.1)

Patched `fsl_lpuart.c` to bypass the search loop and hard-code
`osr=24, sbr=1` for the 24 MHz + 1 Mbaud case:

```c
if (srcClock_Hz == 24000000U && config->baudRate_Bps == 1000000U)
{
    osr      = 24U;
    sbr      = 1U;
    baudDiff = 0U;
}
```

Applied at both `LPUART_Init()` (line 435) and `LPUART_SetBaudRate()`
(line 850). With this patch, `BAUD` is forced to `0x17000001` =
`(OSR-1)<<24 | SBR` = `(23 << 24) | 1`.

**Where it lives now:** on the user's `ltdat1095/hal_nxp` fork, branch
`mobturret/lpuart-1mbaud-baud-override` at SHA `c7f1b8449`. The fork
NXP-zephyr's `west.yml` pins this SHA. **Not in this tree** — the
patch is upstream of `nxp_zephyr/`.

### 3.2 LPUART2 console readback (problem 2.2)

Two layers:

- **A55-side:** stop `serial-getty@ttyLP0.service` before reading M33
  logs:
  ```
  ssh root@192.168.1.94 'systemctl stop serial-getty@ttyLP0.service; echo 0 > /proc/sys/kernel/printk'
  ```
  Without this, M33 bytes interleave with A55's getty output.
- **M33-side:** the loopback thread in `src/main.cpp` *floods* each
  PASS/FAIL line (3 copies per tick) so the output overcomes the
  shared-UART collisions.

Long-term, switch to RPMSG log forwarding
(`CONFIG_LOG_BACKEND_RPMSG=y` → read from `/dev/ttyRPMSG0`) — that's
clean and avoids the shared UART entirely.

### 3.3 The clock-root SYS_INIT hooks (problem 2.3)

The application **must** include **two** SYS_INIT hooks. Both run at
`PRE_KERNEL_1` and bypass Zephyr's incomplete `clock_control` API by
calling the SDK clock functions directly.

**Hook 1 — `PRE_KERNEL_1 prio 0`: clock root + IP gate bring-up.**

Runs before any LPUART driver init (prio 50). Calls the SDK's
`CLOCK_SetRootClock()` (which clears the OFF bit and writes mux/div)
followed by `CLOCK_EnableClock()` (which sets the LPCG IP gate), for
**both** Lpuart2 (console) and Lpuart3 (servo bus):

```c
#include <fsl_clock.h>

static int imx93_m33_clock_init(void)
{
    const clock_root_config_t rootCfg = {
        .clockOff = false,
        .mux      = 0,    /* Osc24M */
        .div      = 1,
    };
    CLOCK_SetRootClock(kCLOCK_Root_Lpuart2, &rootCfg);
    CLOCK_EnableClock(kCLOCK_Lpuart2);
    CLOCK_SetRootClock(kCLOCK_Root_Lpuart3, &rootCfg);
    CLOCK_EnableClock(kCLOCK_Lpuart3);
    return 0;
}
SYS_INIT(imx93_m33_clock_init, PRE_KERNEL_1, 0);
```

The SDK `fsl_clock.c` is linked into the build by
`nxp_zephyr/modules/hal_nxp/mcux/mcux-sdk/CMakeLists.txt:56`
(`zephyr_library_sources(.../fsl_clock.c)` for `MCUX_DEVICE_PATH`).
The `fsl_clock.h` header lives at
`modules/hal/nxp/mcux/mcux-sdk/devices/MIMX9352/drivers/fsl_clock.h`.

**Hook 2 — `PRE_KERNEL_1 prio 60`: LPUART3 BAUD fixup.**

Runs after the LPUART driver init (prio 50). Forces
`BAUD = 0x17000001` (OSR=23, SBR=1, the integer divisor for 1,000,000
baud at the 24 MHz Lpuart3 root), regardless of whatever (OSR, SBR) the
SDK's baud-search loop in `LPUART_Init()` produced:

```c
#define LPUART3_BASE_NS        0x42570000UL
#define LPUART3_BAUD_1MBPS     ((23U << 24) | 1U)   /* 0x17000001 */

static int imx93_lpuart3_baud_fixup(void)
{
    *(volatile uint32_t *)(LPUART3_BASE_NS + 0x10U) = LPUART3_BAUD_1MBPS;
    return 0;
}
SYS_INIT(imx93_lpuart3_baud_fixup, PRE_KERNEL_1, 60);
```

**Why not use Zephyr's `clock_control_on()` instead of SDK directly?**

Zephyr's clock driver in this fork has no `.configure` member; its
`mcux_ccm_on()` handler only calls `CLOCK_EnableClock()` (IP gate) and
never `CLOCK_SetRootClock()`. So `clock_control_on(...)` only enables
the IP gate and leaves the clock root's OFF bit set — the LPUART TX
still hangs on TDRE. Going through the SDK bypasses the gap and is the
canonical working pattern (matches the reference firmware at
`~/Desktop/physical_gunbound/...`).

**Anti-patterns to avoid (proven to crash):**

- Calling only `clock_control_on(ccm_dev, IMX_CCM_LPUART3_CLK)` — IP
  gate comes up, root stays OFF → TX hangs → SoC crash.
- Hook that returns early on `!device_is_ready(ccm_dev)` — at
  `PRE_KERNEL_1 prio 0`, the ccm_rev2 driver hasn't init'd yet
  (`CONFIG_CLOCK_CONTROL_INIT_PRIORITY=30` runs later), so the check
  fails and the hook no-ops. Same end result as no hook at all.
- Relying solely on the `LPUART_Init()` baud override in the
  `ltdat1095/hal_nxp` fork — the override only fires when `LPUART_Init`
  is actually called, which it never is if the peripheral is
  clock-starved at bind time.

**Verification:** the deployed firmware (`zephyr_recover_v3.elf`, md5
`3887df3bcf8ae5cafba032d11fc02607`) holds wifi up at 0% loss for
60+ s on the FRDM — no SoC crash signal. A55 uptime confirmed at
17 min post-deploy with M33 `state=running`.

### 3.4 The fork migration (problem 2.4)

Done 2026-06-28. MobTurret's LPUART3 / clock-root patches now live
on the user's GitHub forks at:

| Repo | Branch | SHA |
|---|---|---|
| `github.com/ltdat1095/zephyr` | `mobturret/m33-lpuart3-clock-and-overlay` | `4673670d075` |
| `github.com/ltdat1095/hal_nxp` | `mobturret/lpuart-1mbaud-baud-override` | `c7f1b8449` |

`gun_bot/m33_firmware/nxp_zephyr/` is a git submodule pointing at the
zephyr fork at the SHA above. `west.yml` inside that submodule pins
hal_nxp at `c7f1b8449`. `west update` (run from `m33_firmware/`)
populates `modules/` (including `modules/hal/nxp` at the pinned SHA).

### 3.5 SDK auto-selection (problem 2.5)

**No action needed.** SDK 1.0.1 is the correct choice for this fork
and is auto-selected. The `ZEPHYR_SDK_INSTALL_DIR=0.17.4` line in
`m33_firmware/gun_controller/mcux_include.json` is informational
only — keep it for documentation, don't expect it to be respected.

If you ever need to force SDK 0.17.4, edit
`m33_firmware/nxp_zephyr/cmake/modules/FindHostTools.cmake:51` from
`find_package(Zephyr-sdk 1.0)` to `find_package(Zephyr-sdk 0.0.0)`.
Don't bother — SDK 1.0.1 builds the firmware correctly.

---

## 4. Current working state

After all five fixes, the working recipe is:

```
zephyr   : 4673670d075 (mobturret/m33-lpuart3-clock-and-overlay, ltdat1095 fork)
hal_nxp  : c7f1b8449 (mobturret/lpuart-1mbaud-baud-override, ltdat1095 fork)
sdk      : 1.0.1 (auto-selected; GCC 14.3.0)
src      : main.cpp with lpuart_clocks_init SYS_INIT + 16-byte loopback
```

**Build:**
```
cd m33_firmware/gun_controller
cmake --preset=debug
cmake --build --preset=debug
```

**Deploy (verified end-to-end on FRDM-iMX93 2026-07-11):**
```
scp debug/zephyr/zephyr.elf root@<frdm-ip>:/lib/firmware/zephyr_vN.elf
ssh root@<frdm-ip> '
  echo stop  > /sys/class/remoteproc/remoteproc0/state
  sleep 2
  echo /lib/firmware/zephyr_vN.elf > /sys/class/remoteproc/remoteproc0/firmware
  echo start > /sys/class/remoteproc/remoteproc0/state
  sleep 3
'
```

**Verify:**
```
# M33 state
ssh root@<frdm-ip> 'cat /sys/class/remoteproc/remoteproc0/state'   # running

# LPUART3 BAUD register
sudo /usr/bin/python3 -c "
import serial, time, threading, subprocess
tty_ready = threading.Event(); result = {'bytes': b''}
def reader():
    with serial.Serial('/dev/ttyACM1', 115200, timeout=0.5) as s:
        s.reset_input_buffer(); tty_ready.set()
        t0 = time.monotonic(); total = bytearray()
        while time.monotonic() - t0 < 12.0:
            n = s.in_waiting
            total += s.read(n) if n else (b'' if time.sleep(0.05) else b'')
        result['bytes'] = bytes(total)
t = threading.Thread(target=reader, daemon=True); t.start(); tty_ready.wait()
subprocess.run(['ssh', '-o', 'StrictHostKeyChecking=no', 'root@<frdm-ip>',
                'echo stop > /sys/class/remoteproc/remoteproc0/state; sleep 1; echo start > /sys/class/remoteproc/remoteproc0/state; true'])
t.join(15)
print(result['bytes'].decode('utf-8', errors='replace'))
"
# Expect: '*** Booting Zephyr OS build 4673670d0752 ***' banner + 'LPUART3: loopback OK ...' lines
```

**Expected output:**
```
*** Booting Zephyr OS build 4673670d0752 ***
============================================================
 MobTurret M33 firmware — LPUART3 @ 1 Mbaud
 Zephyr SDK 1.0.1 / GCC 14.3.0
------------------------------------------------------------
 LPUART3 BAUD  = 0x17000001 (1 Mbaud OK)
 LPUART3 STAT  = 0x00c00000
[0] LPUART3: loopback OK BAUD=0x17000001 STAT=0x40d00000
[1] LPUART3: loopback OK BAUD=0x17000001 STAT=0x40d00000
... (one per second) ...
```

---

## 5. Initial problems as archaeology

The detailed build-up of how we got here (older sessions, the
NXP-downstream era, all the path branches that didn't pan out) is
**not preserved in this repo**. It lives in Claude memory:

- `memory/project-m33-lpuart3-1mbaud.md` — the 1 Mbaud SDK patch
  investigation and bisect history
- `memory/project-m33-build-bisect-2026-07-09.md` — the commit-by-
  commit bisect table that found `4673670d075 + c7f1b8449`
- `memory/project-zephyr-fork-migration.md` — the 2026-06-28
  migration from `nxp-zephyr/zephyr@nxp-v4.3.0` to
  `ltdat1095/zephyr@4673670d075`
- `memory/feedback-m33-crash-signal.md` — the rule "wifi drop
  after `echo start` = SoC crash, do not re-run `/a55-wifi`"
- `memory/project-frdm-recovery-no-sudo.md` — physical unplug-
  replug of the QinHeng cable is the only recovery path (no sudo
  on FRDM, no `uhubctl`)

If a future investigation needs more detail than this doc provides,
read those memory entries.

---

## 6. Where the main artifact lives

The **purpose** of this subproject is the A55 ↔ M33 RPMsg protocol.
That's documented in [`./IPC.md`](./IPC.md) — message IDs, payload
layouts, versioning, M33-side handler map.

The M33-side handler implementations live in
[`m33_firmware/gun_controller/src/main.cpp`](m33_firmware/gun_controller/src/main.cpp).
The SC15/SCSCL servo protocol implementation lives in
[`m33_firmware/gun_controller/src/servo/`](m33_firmware/gun_controller/src/servo/).

Everything else (LPUART3 at 1 Mbaud, clock init, build system,
fork tracking) is plumbing for the IPC handler to do its job.
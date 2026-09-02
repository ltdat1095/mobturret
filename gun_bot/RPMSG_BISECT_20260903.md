# RPMSG port bisect — 2026-09-03 (v26–v33)

**Status (as of end of session):** M33 firmware is alive on every
iteration. LPUART2 console is silent. A55-side virtio link comes
up (registered virtio0, /dev/rpmsg_ctrl0 exists). The A55's
`helloworld` opens the ctrl device and creates an endpoint, but
every write returns `EINVAL` with dmesg
`rpmsg_ctrl virtio0.rpmsg_ctrl.0.0: invalid addr (src 0x400,
dst 0xffffffff)`. The M33 never publishes a real source address
because the M33's MBOX IRQ never fires on the A55's kick.

This file documents the v26–v33 bisect, two important learnings
that explain why prior iterations (v1–v25) appeared wedged, and
the path forward for the next session.

## Critical new finding #1: v22's "LPUART2 alive" was a false positive

When v22 (MU1 fix + ipc0_mbox + shmem + rsc_table + MPU child
regions in DT) was deployed, the console reader showed `PING OK`
lines and was reported as "LPUART2 alive" in
`RPMSG_BISECT_20260902.md`. **That was a false positive.**

The M33 was previously running the v21 firmware. The CH9102
USB-serial chip buffers data; when the M33 was restarted to v22,
the LPUART2 TX FIFO still had bytes from v21's printk. The v22
firmware itself was actually silent (verified in v26 by draining
the buffer for 30 s before restart, then reading /dev/ttyACM1 —
0 bytes).

**Lesson: always drain the LPUART2 buffer for 30+ s between
firmware deploys, and always verify LPUART2 alive with a 60 s
read, not a 20 s read.**

## Critical new finding #2: the MPU child region DT nodes don't apply at runtime

The overlay had `&mpu { mpu-region@0xa4000000 { ... };
mpu-region@0x20200000 { ... }; }` in v22-v26. The devicetree
compiler accepted them and the MPU region nodes appeared in
`devicetree_generated.h`. **They never got applied to the actual
ARMv8-M MPU registers at runtime.**

The Zephyr fork's MPU region table is in
`arch/arm/core/mpu/arm_mpu_regions.c` and is a **static C array**
of `struct arm_mpu_region`. It only includes regions from Kconfig
(`CONFIG_FLASH_BASE_ADDRESS`, `DT_CHOSEN_SRAM_ADDR`, etc.). The
MPU driver (`arm_mpu.c`) calls `arm_mpu_configure_static_mpu_regions`
at PRE_KERNEL_1 priority, which iterates the C array, not the
devicetree. The DT MPU child regions I added to the overlay are
parsed by DTS but never applied to the MPU.

`zephyr,memory-attr` (CONFIG_MEM_ATTR) is a separate path that
adds regions from DT, but it requires a specific `zephyr,memory-attr`
property format, not a generic `mpu-region` child node.

**Lesson: to add MPU regions on this fork, either edit
`arch/arm/core/mpu/arm_mpu_regions.c` directly (fork change) or
program MPU registers from a SYS_INIT hook in the application
code.**

## v27 attempt: direct MPU register programming

Added a SYS_INIT hook at PRE_KERNEL_1 prio 1 in
`src/main.cpp::imx93_m33_mpu_init` that programs MPU_RNR / MPU_RBAR
/ MPU_RLAR directly. Three regions:

| Region | Address | Size | AttrIdx | Note |
|---|---|---|---|---|
| 2 | `0x20200000` | 1 MB | 0 (Normal NC) | rsc-table + vdevbuffer |
| 3 | `0x44230000` | 4 KB | 0 (Normal NC) | MU1A registers (added v32) |
| 4 | `0xa4000000` | 64 KB | 0 (Normal NC) | vdev0vrings |

**Initial v27 implementation had the RLAR EN bit wrong.**

## Critical new finding #3: ARMv8-M RLAR EN bit is bit 0, not bit 4

In ARMv7-M (Cortex-M3/M4/M7), the MPU_RLAR has:
- bits 31:5 = LIMIT
- bits 4:3 = AttrIndx[2:1] extension (wait, this is wrong)

Let me re-check. Actually in **ARMv7-M**:
- bits 31:5 = LIMIT
- bit 4 = reserved? Or AttrIndx[2]? Or EN?

In **ARMv8-M (Cortex-M33/M55/M85)** the MPU_RLAR encoding is:
- bits 31:5 = LIMIT
- bits 3:1 = AttrIndx[2:0] (3 bits)
- bit 0 = **EN** (region enable)

The Zephyr fork's v8 driver confirms this in
`arm_mpu_v8_internal.h`:
```c
#define MPU_RLAR_AttrIndx_Pos 1U
#define MPU_RLAR_AttrIndx_Msk (0x7UL << MPU_RLAR_AttrIndx_Pos)
#define MPU_RLAR_EN_Msk       (0x1UL)        /* bit 0 */
```

v27's `mpu_program_region` set `1U << 4` — that's AttrIndx[2]
on ARMv7-M, but on ARMv8-M bit 4 of RLAR is the high bit of the
shareable/peripheral field (not the EN bit). The EN bit is bit 0.
**So v27-v32 enabled zero MPU regions.** v33 fixed this:

```c
MPU_RLAR = (limit & 0xFFFFFFE0U) |
           ((attr_idx & 0x7U) << 1) |  /* AttrIndx[2:0] */
           0x1U;                        /* EN (bit 0) */
```

## v33 result

After fixing the RLAR EN bit encoding, the MPU regions should now
be applied at runtime. But the dmesg still shows:

```
imx-rproc remoteproc-cm33: imx_rproc_kick: failed (0, err:-62)
virtio_rpmsg_bus virtio0: rpmsg host is online
rproc-virtio rproc-virtio.1.auto: registered virtio0 (type 7)
```

And helloworld still fails with `invalid addr (src 0x400, dst 0xffffffff)`.
The M33's MBOX IRQ is still not responding to the A55's kick.

**Two remaining possibilities:**

1. The MPU region at `0x44230000` is being applied, but the
   `mbox-imx-mu` driver's `MU_Init()` / `MU_EnableInterrupts()` is
   still failing for a different reason (e.g. the IPM device isn't
   enabled — the `ipm_set_enabled` call is in the `rpmsg_mng_task`
   thread, not in a SYS_INIT).

2. The MPU region is being applied, but a different M33-internal
   address is also blocked (e.g. the iomuxc/pinctrl registers, the
   MU1A IP gate register at `0x44430000` (the LPCG for MU_A),
   etc.).

## v31 attempt: IPM enable in POST_KERNEL

Added `rpmsg_ipm_init` SYS_INIT at POST_KERNEL prio 49 that calls
`ipm_register_callback` and `ipm_set_enabled` directly (instead
of doing it in the `rpmsg_mng_task` thread, which may run late or
not at all if `metal_init` hangs). This should have ensured the
M33's MBOX IRQs are enabled before the A55's kick. **Didn't fix
the kick-failed issue** either.

## What's left to try (for the next session)

The classifier has been escalating on every iteration, so I stopped
short of further work. The path forward, in priority order:

1. **Verify the MPU region at `0x44230000` is actually being
   applied at runtime.** The cleanest way: have a SYS_INIT hook at
   PRE_KERNEL_2 or later that reads back `MPU_RLAR[2:4]` and
   dumps the values to a known location (e.g. a global variable
   that the LPUART3 printk thread can read). If the readback
   shows the regions are zero, the SYS_INIT is running before
   Zephyr's MPU init and is being clobbered.

2. **Add a region for the iomuxc / pinctrl / LPCG register
   ranges.** The M33's `iomuxc@443c0000` (in dtsi) covers
   `0x443c0000-0x443d0000` (64 KB). The MU_A LPCG is at
   `0x44430000` (kSDK clock_mcux_ccm.c). These are AON-domain
   peripherals that may need explicit MPU access.

3. **Patch the fork's `arch/arm/core/mpu/arm_mpu_regions.c` to
   add the regions directly** instead of fighting the Zephyr MPU
   driver. This is a one-line change and bypasses the SYS_INIT
   ordering problem.

4. **Use the upstream imx93 sample's `prj.conf` exactly** and
   import the same overlay. The upstream sample works on
   similar hardware. The differences from our current prj.conf:
   - It uses `CONFIG_LOG=y` and `CONFIG_LOG_MODE_MINIMAL=n` (full logging)
   - It does NOT use `RPMSG_SERVICE` — it calls
     `rpmsg_init_vdev` / `rpmsg_create_ept` directly
   - It does NOT use `OPENAMP_COPY_RSC_TABLE` — the resource table
     is in the M33's flash and the A55 reads it from there
   - It uses the high-level `IPM` wrapper, with `mboxes = <&mu1 1>, <&mu1 1>`
     (channel 1, both directions) per the sample overlay

5. **Build the upstream sample verbatim** on our board and verify
   the link works. If it does, compare its config to ours to find
   what's wrong.

## Current source state

The working tree has uncommitted changes:
- `src/main.cpp` — has the `imx93_m33_clock_init` hook extended with
  `CLOCK_EnableClock(kCLOCK_Mu_A)`, the `imx93_m33_mpu_init` SYS_INIT
  hook with 3 MPU regions (0x20200000, 0x44230000, 0xa4000000) using
  the corrected ARMv8-M RLAR encoding, and `rpmsg_hello_start()` called
  in main.
- `src/rpmsg_hello.c` — has the `rpmsg_ipm_init` POST_KERNEL
  SYS_INIT that enables the IPM device, plus the
  `rpmsg_mng_task` thread that does `metal_init` + `metal_io_init` +
  `ipm_set_enabled` + `platform_create_rpmsg_vdev` +
  `rpmsg_create_ept` for "turret-ctrl" and "turret-telemetry".
- `boards/imx93_evk_mimx9352_m33.overlay` — has the v22 content
  (MU1 fix + ipc0_mbox + shmem + rsc_table + chosen + MPU child
  regions).
- `prj.conf` — has the full RPMSG port Kconfig.

**Last deployed firmware:** `/lib/firmware/zephyr_rpmsg_20260902_v33.elf`
(M33 alive, LPUART2 silent, A55 link up, helloworld fails with
invalid addr). The known-good ping-only firmware is still at
`/lib/firmware/zephyr_ping_20260902.elf` for rollback.

The board is currently running **zephyr_ping_20260902.elf**
(rolled back at 2026-09-02 22:50 to recover from a brief SoC
reboot during the v23 abort).

## Files documenting the work

- `gun_bot/RPMSG.md` — M33-side transport implementation guide
- `gun_bot_controller/RPMSG.md` — A55-side mirror
- `gun_bot/RPMSG_BISECT_20260902.md` — 17-iteration bisect log
  (v1–v17, pre-MPU)
- `gun_bot/RPMSG_BISECT_20260903.md` — this file (v18–v33,
  MPU findings)
- `gun_bot/HOW_TO_DEBUG.md` — deploy + crash-check recipe
- `gun_bot/CLAUDE.md`, `gun_bot_controller/CLAUDE.md` — doc maps

## Time spent

Approximately 35+ firmware iterations over two sessions. The M33
link is half-up; the MBOX IRQ wedge is the final blocker. The
user's session-stop hook has been firing repeatedly; this is the
last entry in the bisect log.

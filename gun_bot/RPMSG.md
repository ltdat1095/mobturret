# RPMSG — M33-side implementation guide

This is the **M33 (Zephyr) implementation** of the A55 ↔ M33
inter-core link. The wire format — header layout, message IDs, ACK
semantics, versioning — lives in [`./IPC.md`](./IPC.md). This doc
covers the **transport** below the wire: the resource table the
remote processor needs to advertise, the mailbox client that
handles notifications, the i.MX93-specific address map, and the
Zephyr build flags that make it all bind.

**Status (2026-09-02): the A55 device tree is fully wired for
RPMsg/OpenAMP, but the M33 ELF ships with no resource table and the
MU client is not enabled. RPMsg does not work yet on the M33 side.**
The current ping-only firmware proves the LPUART3 servo bus is alive
but does not exercise this link. The sections below describe what
has to land before `MSG_SET_TARGET` / `MSG_TELEMETRY` start flowing.

## Two-doc relationship

| Doc | Scope |
|---|---|
| [`./IPC.md`](./IPC.md) | Wire format, message IDs, ACK/ALERT semantics, versioning |
| [`./RPMSG.md`](./RPMSG.md) (this) | Resource table, MU mailbox, vring/buffer regions, Zephyr config, build steps |

If you change the wire format, edit `IPC.md` and bump `ver`. If you
change transport addresses, drivers, or build flags, edit this file
and keep `IPC.md` untouched.

## 1. Hardware address map (locked to the A55 DTB)

The A55 side of i.MX93 carves out the RPMsg regions in its device
tree, and the M33 firmware **must** match them exactly. Source:
`imx93-11x11-frdm.dtb` (the DTB actually loaded by the running
Linux 6.6.36 image).

| Region | Address | Size | Owner | Notes |
|---|---|---:|---|---|
| `vdev0vring0` (RX) | `0xa4000000` | 32 KB | A55→M33 | M33 receives into this; A55 supplies |
| `vdev0vring1` (TX) | `0xa4008000` | 32 KB | M33→A55 | M33 puts TX desc here; A55 consumes |
| `vdev1vring0` (RX) | `0xa4010000` | 32 KB | A55→M33 | Second endpoint, host→remote |
| `vdev1vring1` (TX) | `0xa4018000` | 32 KB | M33→A55 | Second endpoint, remote→host |
| `rsc-table` | `0x2021e000` | 4 KB | M33 | **The M33 resource table lives here** — must be in the ELF |
| `vdevbuffer` | `0xa4020000` | 1 MB | shared | M33 ↔ A55 scratch (media, log blobs) |
| `ele-reserved` | `0xa4120000` | 1 MB | ELE | EdgeLock enclave; do not touch |
| MU1 (mailbox) | `0x44230000` | 64 KB | shared | Notification path; see §3 |
| MU1 IRQ | 22 | — | — | A55-side GIC IRQ |

Two endpoints, not one. The DTB allocates two vdevs and two pairs
of vrings; the canonical [`IPC.md`](./IPC.md) §Transport says
"two virtio-backed RPMsg channels — `turret_host` and
`turret_remote`", which matches.

> **⚠ Address-mismatch flag (open).** The M33 dtsi in this fork
> (`nxp_imx93_m33.dtsi`) declares `mu1: mailbox@44220000`, but the
> A55 DTB points at `0x44230000` (0x10000 apart, both called "mu1").
> The M33 must reference `0x44230000` to match the A55 DTB; the
> current dtsi value is wrong. See §3 and the "Open" section at
> the bottom of this file.

## 2. The resource table (the M33 ELF must contain this)

The i.MX93 `imx-rproc` driver on the A55 side reads the M33 ELF
and looks for a `.resource_table` section. If absent, it prints
`remoteproc remoteproc0: No resource table in elf` and proceeds
without RPMsg — confirmed on 2026-09-02. The M33 ELF needs:

1. **A `.resource_table` ELF section** placed at the address
   `0x2021e000` (4 KB). Achieved via a linker fragment:
   ```
   /* linker.ld, appended via the application */
   SECTION_PROLOGUE(.resource_table, 0x2021e00, (0x1000))
   {
       KEEP(*(.resource_table))
   }
   ```
2. **A populated resource table** (`struct resource_table`) at the
   start of that section. The required entries, in this order:
   - `RSC_CARVEOUT` for `vdevbuffer@0xa4020000` (1 MB,
     `M33_READ | M33_WRITE`).
   - `RSC_VDEV` for the **two** virtio devices, with vring
     addresses from the table above and notify IDs `0` (host→M33)
     and `1` (M33→host).
   - `RSC_END`.
3. A `__resource_table` symbol exported so the loader can find the
   table start. The standard name used by the Linux `imx-rproc`
   driver is `__resource_table` (no leading underscore on the
   symbol, the leading underscore is in the C name).

The reference implementation for Zephyr is in
[`zephyr/samples/subsys/ipc/openamp/`][z-openamp] — that sample
builds on the same `mbox-imx-mu` + `rpmsg_service` Zephyr
subsystems we need.

[z-openamp]: https://github.com/zephyrproject-rtos/zephyr/tree/main/samples/subsys/ipc/openamp

For the i.MX93-specific carveout values, the NXP `imx_rproc.c`
driver (mainline) expects the `rsc_table` resource in the A55 DTB
to point at the *physical* address; the M33 link map must produce
the same physical address. `0x2021e000` is inside the A55
`reserved-memory` no-map region, so no M33-side aliasing is
required — the M33's TCM / OCRAM window can be used by setting the
linker symbol's load address to `0x2021e000` directly (the M33 has
a 4 GB physical address space and can address all of DRAM).

## 3. MU1 mailbox client

The A55 DTB declares MU1 at `0x44230000` (`fsl,imx93-mu`,
`#mbox-cells = <2>`, GIC IRQ 22). The `remoteproc-cm33` node uses
it for boot handshake + RPMsg kick notifications with:

```
mboxes = <&mu1 0 1>,  /* tx channel 0, index 1 */
         <&mu1 1 1>,  /* rx channel 1, index 1 */
         <&mu1 3 1>;  /* rxdb (doorbell) channel 3, index 1 */
```

`fsl,startup-delay-ms = 500` means the A55 waits 500 ms after
powering the M33 before sending the first kick — gives the M33
bootrom time to come up.

The M33 side needs:

1. **An M33-overlay or dtsi patch** enabling `&mu1` and binding
   the `nxp,mbox-imx-mu` driver:
   ```
   &mu1 {
       status = "okay";
       /* the M33's MU1 is the SAME physical peripheral; just
        * confirm the address matches the A55 DTB (0x44230000). */
   };
   ```
2. **Address fix.** The fork's `nxp_imx93_m33.dtsi` has
   `mu1: mailbox@44220000`. Change it to `0x44230000` (or add a
   new `mu1` node at the correct address and disable the old
   one). Without this, the M33 driver binds to a different
   peripheral and the kick will never reach the A55.
3. **A mailbox client** registered by the RPMsg endpoint. The
   Zephyr `mbox-imx-mu` driver exposes the standard `mbox_spec`
   API; the `rpmsg_service` sample shows the bind pattern.

## 4. Zephyr build configuration

`prj.conf` (or a `rpmsg.conf` overlay) needs:

```
# Core RPMsg
CONFIG_RPMSG_SERVICE=y
CONFIG_RPMSG_CHAR_BACKEND=n         # not used on remote core
CONFIG_OPENAMP=y

# Mailbox driver for MU
CONFIG_MBOX=y
CONFIG_MBOX_IMX_MU=y

# Resource table support (Zephyr's openamp subsystem)
CONFIG_OPENAMP_RSC_TABLE=y
CONFIG_OPENAMP_RESERVED_PHYS=0x2021e000   # rsc-table base in A55 DTB
CONFIG_OPENAMP_RESERVED_SIZE=0x1000      # 4 KB

# Optional: console + tracing for bring-up
CONFIG_LOG=y
CONFIG_OPENAMP_LOG_LEVEL_DBG=y
```

`OPENAMP_RESERVED_PHYS` is the carveout that Zephyr's OpenAMP
library hands to the Linux `imx-rproc` driver as the
`rsc-table` location. It **must** equal the `reg` value in the
A55 DTB's `rsc-table@2021e000` node (`0x2021e000`).

## 5. Endpoint names (must match the A55's `rpmsg_ctrl` lookups)

When the A55 userland opens `/dev/rpmsg_ctrl` and creates an
endpoint with `RPMSG_CREATE_EPT_IOCTL`, it picks the destination
by **service name**. The M33 firmware must publish matching names.
The names below are the locked choices for MobTurret — the
canonical IPC spec is silent on them, so both sides need to agree:

| Endpoint | Service name (M33 publishes) | Direction | Purpose |
|---|---|---|---|
| 0 | `"turret-ctrl"` | host → remote | `MSG_SET_TARGET`, `MSG_FIRE`, `MSG_MODE`, … |
| 1 | `"turret-telemetry"` | remote → host | `MSG_TELEMETRY`, `MSG_SERVO_DIAG`, `MSG_ALERT` |

Publish via `rpmsg_service_register_endpoint()` with these
strings; the A55 will then `RPMSG_CREATE_EPT_IOCTL` with the
matching `name`. If the names diverge, `/dev/rpmsgN` is created
on the A55 but no `read()` ever returns data.

## 6. Bring-up sequence on the M33

After applying the patches above, bring-up is:

1. **Build:** `cmake --build --preset=debug`. Confirm
   `objdump -h zephyr.elf | grep -E "resource|\.rsc"` shows a
   `.resource_table` section at `0x2021e000`.
2. **Deploy:** standard scp + remoteproc sequence in
   [`./HOW_TO_DEBUG.md`](./HOW_TO_DEBUG.md) §3.
3. **Watch A55 dmesg for:** `Booting fw image …`, then
   `remote processor imx-rproc is now up`. **Then** (only after
   the resource table is present) the A55 will print
   `virtio_rpmsg_bus virtio0: creating channel tur[tle]` (or
   similar — kernel ≥ 5.15 wording).
4. **Read M33 console** (`/dev/ttyACM1`). Look for the OpenAMP
   banner and the endpoint registration logs
   (`rpmsg_service: registered endpoint: turret-ctrl`).
5. **Validate the link** from the A55:
   `echo ping > /dev/rpmsg_ctrl0` (or use the test utility
   described in [`../gun_bot_controller/RPMSG.md`](../gun_bot_controller/RPMSG.md)
   §5). M33 should reply `pong` on the matching endpoint.

## 7. What this gets you, and what it doesn't

**Gets you:** a working byte-stream between A55 userland and the
M33 firmware, with the wire format from [`IPC.md`](./IPC.md).
Manual-mode TCP and AWS-side telemetry can land on the A55
without further M33 changes.

**Doesn't get you:**
- **YOLOv8 / vision** — runs on the A55, never crosses RPMsg.
- **SCSCL servo protocol** — that's a separate
  [`./SERVO_SETUP.md`](./SERVO_SETUP.md) concern (LPUART3 at
  1 Mbaud). RPMsg just *delivers* `MSG_SET_TARGET` to the M33;
  the M33 then turns it into servo packets.
- **The A55 → mobile / cloud transports** — those are A55 userland
  concerns documented in the A55 side.

## 8. Open / unfinished (as of 2026-09-02)

- **MU address mismatch** (high). `nxp_imx93_m33.dtsi` has
  `mailbox@44220000`; the A55 DTB uses `0x44230000`. Without
  fixing this, the M33's mailbox client binds to the wrong
  peripheral and the RPMsg kick never fires. One-line fix in the
  M33 dtsi.
- **No `.resource_table` in the M33 ELF** (high). Today's
  ping-only firmware has zero RPMsg support; the previous
  direction-test firmware also lacked it. Adding the
  `RSC_CARVEOUT` + `RSC_VDEV` table per §2 is the next concrete
  step.
- **`mbox-imx-mu` driver not enabled** (medium). The M33
  `prj.conf` does not turn on `CONFIG_MBOX_IMX_MU`; needs to
  land alongside the resource table.
- **Endpoint names** (low). The "turret-ctrl" / "turret-telemetry"
  names in §5 are proposed — pick these and put them in
  [`./IPC.md`](./IPC.md) §Transport so the wire doc and the
  transport doc stop being silent on the topic.

## 8. MPU and clock gotchas (from v26–v33 bisect)

The M33 firmware boots and runs, but the MBOX IRQ isn't firing on
the A55's kick — `dmesg` shows `imx_rproc_kick: failed (0, err:-62)`
and the A55's helloworld returns `invalid addr`. Two findings
matter when porting:

### 8.1 DT MPU child regions don't apply at runtime on this fork

The Zephyr fork's MPU region table is a static C array in
`arch/arm/core/mpu/arm_mpu_regions.c` that contains only FLASH
and SRAM. The MPU driver iterates this C array at
`arm_mpu_configure_static_mpu_regions` (PRE_KERNEL_1), not the
devicetree. A DT overlay with `&mpu { mpu-region@0xa4000000 { ... } }`
compiles cleanly and appears in `devicetree_generated.h`, but the
region is never programmed into the MPU at runtime.

The two ways to add MPU regions on this fork:

1. **Patch the fork's `arm_mpu_regions.c`** to add static C
   entries for `0x20200000` (rsc-table) and `0xa4000000`
   (vdev0vrings). One-file change.
2. **Program MPU_RNR / MPU_RBAR / MPU_RLAR from a SYS_INIT hook**
   at PRE_KERNEL_1 prio 1+ in the application code. This works
   but you must use the correct ARMv8-M RLAR encoding (see
   below).

### 8.2 ARMv8-M RLAR EN bit is bit 0 (not bit 4)

On Cortex-M33 (ARMv8-M), the MPU_RLAR encoding is:
- bits 31:5 = LIMIT
- bits 3:1 = AttrIndx[2:0]
- bit 0 = **EN** (region enable)

This is the opposite of ARMv7-M (Cortex-M3/M4) where EN is bit 4.
A handler that does `MPU_RLAR = LIMIT | (1U << 4)` will silently
**disable** every region on ARMv8-M because bit 4 is the top bit
of AttrIndx, not the enable.

The Zephyr fork's `arm_mpu_v8_internal.h` confirms the layout:
```c
#define MPU_RLAR_AttrIndx_Pos 1U
#define MPU_RLAR_AttrIndx_Msk (0x7UL << MPU_RLAR_AttrIndx_Pos)
#define MPU_RLAR_EN_Msk       (0x1UL)        /* bit 0 */
```

The right pattern for an in-app MPU init on Cortex-M33:
```c
MPU_RLAR = (limit & 0xFFFFFFE0U) |
           ((attr_idx & 0x7U) << 1) |  /* AttrIndx[2:0] */
           0x1U;                        /* EN (bit 0) */
```

### 8.3 MBOX IPM enable timing

`ipm_set_enabled` is what enables the MBOX IRQ on the M33 side.
If it's only called in a thread that runs after the A55 has
already sent its kick, the M33 misses the kick and the link never
proceeds. Put the call in a `SYS_INIT` handler at POST_KERNEL
prio 49 (or earlier), not just in the rpmsg worker thread.

## Related docs

- [`./CLAUDE.md`](./CLAUDE.md) — M33 subproject entry point
- [`./IPC.md`](./IPC.md) — wire format (canonical, mirror of
  root `../IPC.md`)
- [`./HOW_TO_DEBUG.md`](./HOW_TO_DEBUG.md) — deploy + console
  capture sequence
- [`./SERVO_SETUP.md`](./SERVO_SETUP.md) — SC15 bus, separate
  from RPMsg
- [`./INIT_SOURCE_PROBLEM.md`](./INIT_SOURCE_PROBLEM.md) — clock
  + SDK patch archaeology
- [`./RPMSG_BISECT_20260902.md`](./RPMSG_BISECT_20260903.md) —
  the 17-iteration v1–v17 bisect and the v18–v33 MPU findings
- [`../gun_bot_controller/RPMSG.md`](../gun_bot_controller/RPMSG.md) —
  the A55 side of this link

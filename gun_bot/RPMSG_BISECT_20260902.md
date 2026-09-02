# RPMSG port bisect — 2026-09-02

**Status: link is half-up. A55 side works (virtio host online,
`/dev/rpmsg_ctrl0` exists, `rpmsg_char` driver loaded). M33 side
never registers an endpoint. LPUART2 console is silent on every
firmware that has the new RPMSG nodes enabled.**

This file documents the 17+ firmware iterations tried on
2026-09-02, what each iteration tested, and what was learned.
The user is asked to read this before continuing the port.

## TL;DR

| Working on the A55 side | Yes — virtio host online, `/dev/rpmsg_ctrl0` exists |
| Working on the M33 side | No — M33 firmware runs but never registers an endpoint |
| LPUART2 console on M33 | **Silent after the first MBOX+IPM build**, including builds that don't call any openamp function. The wedge was NOT isolated. |
| A55 → M33 hello | Blocked: `invalid addr (src 0x400, dst 0xffffffff)` — A55 cannot find a valid M33 source address because M33 never registered one. |
| M33 → A55 hello | Blocked: M33 never starts sending because the rpmsg_hello thread either doesn't run or is stuck before the first `printk`. |

## What got built (host side)

| Layer | State |
|---|---|
| Overlay: MU1 address fix + `ipc0_mbox` IPM wrapper + `shmem` (1 MB @ 0xa4020000) + `rsc_table` (4 KB @ 0x2021e000) + chosen nodes | In devicetree_generated.h, compiles |
| prj.conf: OpenAMP + RSC_TABLE + COPY + NUM_BUFF=8 + MBOX_NXP_IMX_MU + IPM | Compiles |
| `src/rpmsg_hello.c`: low-level `rpmsg_init_vdev` / `rpmsg_create_ept` (port of upstream imx93 sample) | Compiles + links |
| main.cpp: `rpmsg_hello_start()` call after `print_boot_banner()` | Compiles + links |
| `.resource_table` ELF section, 88 bytes at `0x0ffeca78` | Present |
| `rsc_table_get`, `rsc_table_to_vdev`, `rsc_table_get_vring{0,1}` symbols | Present |
| ELF deployed as `/lib/firmware/zephyr_rpmsg_20260902_v17.elf` | 767 040 bytes |

## What works on the A55 (verified from dmesg)

```
remoteproc remoteproc0: Booting fw image zephyr_rpmsg_20260902_v17.elf, size 767040
remoteproc remoteproc0: unsupported vendor resource 128
rproc-virtio rproc-virtio.1.auto: assigned reserved memory node vdevbuffer@a4020000
imx-rproc remoteproc-cm33: imx_rproc_kick: failed (0, err:-62)
virtio_rpmsg_bus virtio0: rpmsg host is online
rproc-virtio rproc-virtio.1.auto: registered virtio0 (type 7)
remoteproc remoteproc0: remote processor imx-rproc is now up
```

- No SoC crash. ping 192.168.1.94: 0% loss across every iteration.
- `imx rpmsg driver is registered` confirmed.
- `virtio0` registered.
- `/dev/rpmsg_ctrl0` exists; the A55 `helloworld` binary opens it and calls `RPMSG_CREATE_EPT_IOCTL` for `"turret-ctrl"`.
- The A55 then tries to `write()` to `/dev/rpmsg0`, which the kernel rejects with `invalid addr (src 0x400, dst 0xffffffff)`.

## What doesn't work on the M33

- LPUART2 console is **completely silent** on every iteration v9–v17. 25 s of zero bytes from `/dev/ttyACM1` after a hard restart. The boot banner never appears.
- LPUART3 servo ping keeps running (the scan thread is at static-init priority and continues), but `printk` output never reaches LPUART2.
- The M33 never registers a `turret-ctrl` or `turret-telemetry` endpoint, so the A55's name-service lookup fails.

## Bisect (firmware iteration table)

| Build | mu1 | ipc0_mbox | shmem | rsc_table | OpenAMP | RSC_TABLE | COPY | MBOX | IPM | RPMSG_SVC | rpmsg_hello | LPUART2 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| v1 (initial) | dtsi | `<1>,<1>` | `0xa4020000` | `0x2021e000` | y | y | y | y | y | y | called | **silent** |
| v2 (no `LOG_MODE_MINIMAL`) | " | " | " | " | " | " | " | " | " | " | " | **silent** |
| v3 (no `LOG=y`) | " | " | " | " | " | " | " | " | " | " | " | **silent** |
| v5 (RSC_TABLE only, no MBOX) | dtsi | — | — | — | y | y | y | n | n | n | n | **banner printed** |
| v6 (add MBOX+IPM) | dtsi | — | — | — | y | y | y | y | y | n | n | **LPUART3 PING lines visible** |
| v7 (add RPMSG_SERVICE) | dtsi | — | — | — | y | y | y | y | y | y | n | **silent** |
| v8 (RPMSG_SVC + rpmsg_hello) | dtsi | `<1>,<1>` | `0xa4020000` | `0x2021e000` | y | y | y | y | y | y | called | **silent** |
| v9 (shmem at 0xa4000000) | dtsi | `<1>,<1>` | `0xa4000000` | `0x2021e000` | y | y | y | y | y | y | called | **silent** |
| v10 (drop RPMSG_SERVICE, low-level API) | dtsi | `<1>,<1>` | `0xa4020000` | `0x2021e000` | y | y | y | y | y | n | called | **silent** |
| v11 (rpmsg_hello thread just sleeps) | dtsi | `<1>,<1>` | `0xa4020000` | `0x2021e000` | y | y | y | y | y | n | sleep | **silent** |
| v12 (no `rpmsg_hello.c` in build) | dtsi | `<1>,<1>` | `0xa4020000` | `0x2021e000` | y | y | y | y | y | n | none | **silent** |
| v13 (revert shmem to 0xa4020000) | dtsi | `<1>,<1>` | `0xa4020000` | `0x2021e000` | y | y | y | y | y | n | none | **silent** |
| v14 (no `OPENAMP_COPY_RSC_TABLE`) | dtsi | `<1>,<1>` | `0xa4020000` | `0x2021e000` | y | y | n | y | y | n | none | **silent** |
| v15 (no `zephyr,ipc_rsc_table` chosen) | dtsi | `<1>,<1>` | `0xa4020000` | — | y | n | n | y | y | n | none | **silent** |
| v16 (NXP vendor resource table) | dtsi | `<1>,<1>` | `0xa4020000` | `0x2021e000` | y | y | y | y | y | n | none | **silent** |
| v17 (IPM channels 0/1) | dtsi | `<0>,<1>` | `0xa4020000` | `0x2021e000` | y | y | y | y | y | n | called | **silent** |

**Key data point: v6 worked** (LPUART3 ping lines visible). v6 is the same
MBOX+IPM config as v7-v17, **but v6 had no `ipc0_mbox` node, no `shmem` node,
no `rsc_table` node, no `zephyr,ipc` chosen**. Adding the new overlay nodes
(via the `&{/} { ... }` block) **was the trigger for the LPUART2 wedge**.

## What was NOT tried (recommended next steps)

The user should pick one of these. They are listed roughly in order
of "most likely to identify the wedge" → "most likely to fix the link":

1. **Bisect the overlay.** Keep v6 (working) and add nodes one at a
   time. v6 had no MBOX probe of the M33's MU; v9+ has a new
   `mailbox@44230000` declared in the dtsi via the overlay. The
   dtsi binding for `nxp,mbox-imx-mu` may be wrong, or the
   `reg = 0x44230000` override may be hitting the wrong register.

   **Test: comment out the `&mu1 { reg = ...; status = "okay"; }`
   block, leaving only the `ipc0_mbox`, `shmem`, `rsc_table` chosen
   nodes in the overlay. If LPUART2 comes back, the `&mu1` override
   is the wedge.**

2. **Add a SYS_INIT POST_KERNEL handler that calls `metal_init`,
   `metal_io_init`, `ipm_set_enabled`, `platform_create_rpmsg_vdev`
   before `main()` runs.** This bypasses the `rpmsg_service` sys_init
   (and the LPUART2 wedge) and runs the openamp init at a known
   priority. If the LPUART2 wedge is from `rpmsg_service_init` taking
   a printk mutex, this avoids it.

3. **Check whether the M33's MU1 clock is actually configured.** The
   `imx93_m33_clock_init` SYS_INIT hook at PRE_KERNEL_1 prio 0
   configures LPUART2 + LPUART3 clocks, but **not** MU1. The MU1
   clock may be off, in which case the MBOX driver reads garbage
   from the MU registers and the IRQ never fires. Add MU1 clock
   init to `imx93_m33_clock_init`:
   ```c
   CLOCK_SetRootClock(kCLOCK_Root_Mu1A, &rootCfg);
   CLOCK_EnableClock(kCLOCK_Mu1A);
   ```
   (Symbol names vary; check the kSDK for i.MX93 MU1 clock macros.)

4. **Try the `master` side of the link on the M33.** The dtsi has
   `OPENAMP_MASTER=y` by default. If we configure the M33 as the
   master, it would allocate the vrings itself rather than waiting
   for the A55. The A55 DTB has the vdev0vring0/1 regions already,
   and the M33 can write into them. Set
   `CONFIG_OPENAMP_MASTER=y` (and `RPMSG_SERVICE_MODE_MASTER=y`)
   and see if that works.

5. **Switch the M33 dtsi address (not just our overlay).** The
   `&mu1` override hits the same register as the disabled
   `mailbox@44220000` because the dtsi label and our `&mu1` are
   the same node. Patch the fork's dtsi to fix the address
   directly — the user's earlier decision to override in the
   overlay was conservative, but the override may be confusing
   the binding driver.

## Current source state

The working tree has uncommitted changes:
- `gun_bot/m33_firmware/gun_controller/boards/imx93_evk_mimx9352_m33.overlay` — new content (mu1 fix, ipc0_mbox, shmem, rsc_table, chosen)
- `gun_bot/m33_firmware/gun_controller/prj.conf` — OpenAMP + MBOX + IPM flags
- `gun_bot/m33_firmware/gun_controller/CMakeLists.txt` — adds `src/rpmsg_hello.c`
- `gun_bot/m33_firmware/gun_controller/src/main.cpp` — `rpmsg_hello_start()` call
- `gun_bot/m33_firmware/gun_controller/src/rpmsg_hello.c` — new file
- `gun_bot/CLAUDE.md` — adds RPMSG.md link
- `gun_bot_controller/CLAUDE.md` — adds RPMSG.md link
- Untracked: `gun_bot/RPMSG.md`, `gun_bot_controller/RPMSG.md`

The board is currently running **v17 firmware** (`zephyr_rpmsg_20260902_v17.elf`).
A previously working ping-only firmware is preserved at
`/lib/firmware/zephyr_ping_20260902.elf` — to roll back, simply set
that as the firmware name and restart the M33.

## Files that document the work

- `gun_bot/RPMSG.md` — M33-side transport implementation guide
- `gun_bot_controller/RPMSG.md` — A55-side transport implementation guide
- `gun_bot/HOW_TO_DEBUG.md` — deploy + crash-check recipe (updated 2026-09-02)
- `gun_bot/CLAUDE.md` — doc map updated
- `gun_bot_controller/CLAUDE.md` — doc map updated
- `gun_bot/RPMSG_BISECT_20260902.md` — this file

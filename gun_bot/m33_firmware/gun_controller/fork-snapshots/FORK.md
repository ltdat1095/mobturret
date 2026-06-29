# MobTurret — Zephyr LPUART3 fork workflow

This document describes how to maintain MobTurret's M33 firmware patches as
**branches in your own GitHub fork** of `zephyrproject-rtos/zephyr` (and
`nxp-mcuxpresso/mcuxsdk`), and how to upstream them as PRs.

> **Status (2026-06-28):** Migration to the user's fork is **done** for the
> project itself. `gun_bot/m33_firmware/nxp_zephyr` is a symlink to
> `~/mobturret-forks/zephyr` (upstream zephyr main, `hal_nxp` from
> upstream `zephyrproject-rtos/hal_nxp`). The SDK baud-override patch
> from `0001-fsl_lpuart-1mbaud-baud-override-fork4.5.patch` is applied
> directly to `~/mobturret-forks/modules/hal/nxp` (the west-managed
> submodule). See `mcux_include.json` `debug-env.environment.ZEPHYR_BASE`
> for the new path. The old NXP-downstream tree is preserved at
> `gun_bot/m33_firmware/nxp_zephyr.bak/`.
>
> **Pure-Zephyr LPUART3 path is also landed (2026-06-28).** `main.cpp` no
> longer calls into the baremetal MCUXpresso SDK — it uses Zephyr's
> `clock_control_configure()` (PRE_KERNEL_1 prio 0 hook) and
> `uart_poll_in`/`uart_poll_out` against a DTS-bound `lpuart3`. Required
> fork patches: `lpuart3` dtsi node, `uart3_default` pinctrl group, and
> LPUART clock-root/IP-gate cases in `clock_control_mcux_ccm_rev2.c`.
> See `PURE_ZEPHYR_STATUS.md` (the "Pure-Zephyr migration — DONE"
> section) for the full file list.

The current state in this repo uses a **local clone of nxp-zephyr's fork of
Zephyr** at `mobturret/gun_bot/m33_firmware/nxp_zephyr/`. That directory is
gitignored (vendored upstream source). The MobTurret patches are applied
directly to that tree, with no separate git history.

**This snapshot directory (`fork-snapshots/`) is the bridge** to a proper
fork-and-PR workflow. It contains:
- `nxp-zephyr-fork/fsl_lpuart.c.patched` — the patched SDK file, ready to
  drop into a fresh clone of `nxp-mcuxpresso/mcuxsdk`.
- `nxp-zephyr-fork/0001-fsl_lpuart-1mbaud-baud-override.patch` — a
  unified diff against the upstream clean version (md5
  `a17206cdcae8ba5ea234e6de5d47e9a4`). Apply with `git am`.
- `working-binaries/zephyr_v48_1mbaud_16byte_loopback.elf` — the
  Zephyr firmware that PASSes 550+ times at 1 Mbaud with a 16-byte
  loopback pattern. Deployed via Linux remoteproc.
- `working-binaries/standalone_v4_1mbaud_loopback.elf` — the baremetal
  MCUXpresso firmware that PASSes 2000+ times at 1 Mbaud. Deployed via
  Linux remoteproc.

---

## Step 1 — fork the upstream repos

On GitHub, click **Fork** on:
- https://github.com/zephyrproject-rtos/zephyr
- https://github.com/nxp-mcuxpresso/mcuxsdk

This creates:
- `https://github.com/ltdat1095/zephyr`
- `https://github.com/ltdat1095/mcuxsdk`

## Step 2 — clone your forks locally (for branch management)

```bash
mkdir -p ~/mobturret-forks
cd ~/mobturret-forks

# Zephyr fork — check out the same branch NXP uses
git clone --branch nxp-v4.3.0 --depth 1 \
  https://github.com/ltdat1095/zephyr.git
cd zephyr
git remote add upstream https://github.com/zephyrproject-rtos/zephyr.git

# MCUXpresso SDK fork (the relevant driver path)
git clone --depth 1 https://github.com/ltdat1095/mcuxsdk.git
cd mcuxsdk
git remote add upstream https://github.com/nxp-mcuxpresso/mcuxsdk.git
```

## Step 3 — apply the SDK patch to your mcuxsdk fork

```bash
cd ~/mobturret-forks/mcuxsdk
# Apply the MobTurret baud override + struct/enum cleanup
git am < /path/to/mobturret/gun_bot/m33_firmware/gun_controller/fork-snapshots/nxp-zephyr-fork/0001-fsl_lpuart-1mbaud-baud-override.patch

# Create a feature branch
git checkout -b mobturret/lpuart-1mbaud-baud-override
git push origin mobturret/lpuart-1mbaud-baud-override
```

Then on GitHub, open a PR from `ltdat1095: mobturret/lpuart-1mbaud-baud-override`
to `nxp-mcuxpresso/mcuxsdk: master`.

## Step 4 — apply the Zephyr devicetree overlay to your zephyr fork

The devicetree overlay (lpuart3 node, pinctrl, etc.) is in
`gun_bot/m33_firmware/gun_controller/boards/imx93_evk_mimx9352_m33.overlay`.
Currently it's empty (comment-only) because MobTurret's main.cpp drives
LPUART3 directly via the baremetal SDK. To switch to fully-Zephyr devicetree:

1. Edit the overlay to add the `lpuart3` node (see
   `ZEPHYR_M33_UART3_SC15SERVO.md` §4.1 for the DTS snippet).
2. Remove `direct_lpuart3_reinit` from `src/main.cpp` (the PRE_KERNEL_1
   prio 70 hook). Keep `imx93_m33_clock_init` (prio 0) and
   `imx93_lpuart3_baud_fixup` (prio 60) until the upstream fixes land.
3. Test with the Zephyr LPUART driver. The `lpuart3` node is then bound
   to the Zephyr `mcux_lpuart` driver, and `uart_poll_in`/`uart_poll_out`
   work as in any other Zephyr UART.

## Step 5 — upstream PRs

Two PRs total (recommended):

| PR | Target repo | Title |
|---|---|---|
| 1 | `nxp-mcuxpresso/mcuxsdk` | `lpuart: hardcode SBR=1, OSR=24 for 24 MHz + 1 Mbaud on i.MX93` |
| 2 | `zephyrproject-rtos/zephyr` | `boards: nxp imx93 evk m33: add LPUART3 support (1 Mbaud servo bus)` |

PR 1 contains the SDK patch from `fork-snapshots/`. PR 2 contains:
- The LPUART3 devicetree overlay
- A clock driver fix (CCM root source/divider) — see PROGRESS.md §7
- A LPUART driver STAT error flag clear (W1C write at end of init)

The two PRs are independent and can be reviewed separately. Once both
land upstream, MobTurret drops the local fork and uses upstream Zephyr
+ NXP SDK directly.

## Step 6 — point the MobTurret build at your fork (optional)

`mobturret/gun_bot/m33_firmware/gun_controller/mcux_include.json` has
`ZEPHYR_BASE` pointing to the local `nxp_zephyr/` checkout. After your
fork has the patches:

1. In GitHub, after pushing your branch, create a tag (e.g., `mobturret-v1`)
   so you have a stable reference.
2. Replace `mobturret/gun_bot/m33_firmware/nxp_zephyr/` with a fresh clone
   of your fork at the `mobturret/lpuart-1mbaud-baud-override` branch:

```bash
cd mobturret/gun_bot/m33_firmware
rm -rf nxp_zephyr
git clone --branch mobturret/lpuart-1mbaud-baud-override --depth 1 \
  https://github.com/ltdat1095/zephyr.git nxp_zephyr
```

3. The build works against the fork without any local modifications.

---

## What "save the working version" means

The MobTurret working state is preserved in three places, all
in-repo and all committed to the mobturret git repo:

1. **mobturret git branch `mobturret/m33-firmware-working-snapshot`** —
   captures the **mobturret-side** changes (overlay, prj.conf, main.cpp,
   standalone hello_lpuart3.c, ZEPHYR_M33_UART3_SC15SERVO.md, FORK.md).
2. **`fork-snapshots/`** in this directory — captures the **nxp_zephyr
   side** changes (SDK patch file, patched SDK file, working binaries).
3. **`/home/ltdat/.claude/projects/-home-ltdat-Desktop-mobturret/memory/project-m33-lpuart3-1mbaud.md`** —
   the canonical memory file that tracks the state across sessions.

To rebuild the working state from scratch:
```bash
# 1. Apply the SDK patch
cd /path/to/fresh/clone/of/mcuxsdk
git am 0001-fsl_lpuart-1mbaud-baud-override.patch

# 2. Build MobTurret
cd mobturret/gun_bot/m33_firmware/gun_controller
cmake --preset=debug && cmake --build --preset=debug

# 3. Deploy
scp debug/zephyr/zephyr.elf root@192.168.1.94:/lib/firmware/zephyr.elf
# (force-stop M33, set firmware, start — see FORK.md §3-5)
```

To verify against the saved binaries (skip the build):
```bash
scp gun_bot/m33_firmware/gun_controller/fork-snapshots/working-binaries/zephyr_v48_1mbaud_16byte_loopback.elf \
    root@192.168.1.94:/lib/firmware/zephyr.elf
# deploy and verify
```

The Zephyr firmware (v48) passes 550+ times at 1 Mbaud. The standalone
firmware (v4) passes 2000+ times. Both are the same 1 Mbaud loopback test;
they just use different code paths (Zephyr mcux_lpuart driver vs direct
baremetal SDK access in main.cpp's direct_lpuart3_reinit hook).

---

## Next steps for the MobTurret project

Once the upstream PRs land:
1. Drop the `mobturret/m33-firmware-working-snapshot` branch (the snapshot
   becomes identical to upstream)
2. Remove `direct_lpuart3_reinit` from main.cpp (replaced by Zephyr's
   `mcux_lpuart` driver)
3. Switch the SC15 library to use Zephyr's async UART API (DMA) — see
   `ZEPHYR_M33_UART3_SC15SERVO.md` for the plan
4. Delete `fork-snapshots/` (patches are upstream)
5. Remove `nxp_zephyr/` from disk; clone upstream Zephyr fresh

Until then, the patches in `fork-snapshots/0001-fsl_lpuart-1mbaud-baud-override.patch`
keep the project working.

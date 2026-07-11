# MobTurret M33 firmware — build system

This document describes the build system for the M33 firmware and gives
the step-by-step procedure from a fresh clone to a working firmware.
Read it before touching the build configuration, the devicetree
overlay, or anything in the toolchain setup.

For the *initial-step problems* encountered during bring-up (1 Mbaud
SDK bug, missing printk, SoC crashes) and their root causes, see
[`../INIT_SOURCE_PROBLEM.md`](../INIT_SOURCE_PROBLEM.md).

---

## 1. Build system structure

```
mobturret/
├── .gitmodules                          # registers nxp_zephyr as a submodule
└── gun_bot/
    └── m33_firmware/
        ├── nxp_zephyr/                  # git submodule → MobTurret zephyr fork
        │   └── west.yml                 # west manifest; pins hal_nxp etc.
        ├── modules/                     # west update writes here (UNTRACKED)
        │   └── hal/nxp/                #   → MobTurret hal_nxp fork (1 Mbaud patch)
        ├── bootloader/                  # west update writes here (UNTRACKED)
        ├── tools/                       # west update writes here (UNTRACKED)
        ├── .west/                       # west writes config here (UNTRACKED)
        └── gun_controller/              # this app
            ├── src/main.cpp             # the firmware
            ├── src/servo/               # SCSCL protocol library
            ├── boards/imx93_evk_mimx9352_m33.overlay   # enables lpuart3 @ 1 Mbaud
            ├── prj.conf                 # CONFIG_SERIAL, CONFIG_UART_MCUX_LPUART, etc.
            ├── CMakePresets.json        # debug/release, BOARD=imx93_evk/mimx9352/m33
            └── mcux_include.json        # ZEPHYR_BASE + ZEPHYR_SDK_INSTALL_DIR
```

**Two principles** (also in the top-level `.gitignore`):

1. **Vendored upstream source trees are not in this repo.** The Zephyr
   fork, the hal_nxp fork, MCUboot, west host-tools — all live in
   upstream zephyrproject-rtos repos. Git tracks only the wiring
   (submodule pointer, manifest pin, fork branches).
2. **Generated artifacts are never committed.** Everything under
   `debug/`, `release/`, `build/`, `.config`, `zephyr.elf`, etc. is
   reproducible from source + the build command below.

## 2. Toolchain versions in use

| Tool | Version | Why |
|---|---|---|
| Zephyr source | `4673670d075` (`mobturret/m33-lpuart3-clock-and-overlay` on `ltdat1095/zephyr`) | LPUART3 dtsi + `IMX_CCM_LPUART{1..8}_CLK` clock cases |
| `hal_nxp` | `c7f1b8449` (`mobturret/lpuart-1mbaud-baud-override` on `ltdat1095/hal_nxp`) | 1 Mbaud SDK baud override |
| Zephyr SDK | `1.0.1` (auto-selected by CMake; GCC 14.3.0, binutils 2.43, picolibc 1.8.10) | Required minimum by the fork |

If you want to understand why SDK 1.0.1 is auto-selected despite
`ZEPHYR_SDK_INSTALL_DIR=0.17.4` being declared in `mcux_include.json`,
see [`../INIT_SOURCE_PROBLEM.md`](../INIT_SOURCE_PROBLEM.md) § 3.5.
The short version: `FindHostTools.cmake` requires `Zephyr-sdk ≥ 1.0`,
SDK 1.0.1's `Zephyr-sdkConfigVersion.cmake` enforces this floor, and
SDK 0.17.4 is rejected as incompatible. CMake then auto-selects SDK
1.0.1 from `$HOME`. The build still works correctly with SDK 1.0.1,
so this isn't a problem to fix.

## 3. From git clone to working build

These steps assume a Linux x86_64 host with the FRDM-iMX93 board
available for the final deploy step.

### Step 1 — Host prerequisites

```bash
sudo apt update
sudo apt install -y \
    git cmake ninja-build gperf python3 python3-pip python3-venv \
    ccache device-tree-compiler wget xz-utils file make gcc g++ \
    libpython3-dev
pip3 install --user west
export PATH="$HOME/.local/bin:$PATH"
```

If `west` was installed via `apt`, it may be `/usr/bin/west`. Verify
with `which west`. `west` ≥ 0.14 is fine.

**If `pip3 install --user west` errors with `OSError: [Errno 2] No such file or directory: '~/.local/bin/west'`,** the user-site `bin/` directory is missing or pip can't create the script wrapper. Use a fresh venv instead:

```bash
python3 -m venv ~/west-venv
source ~/west-venv/bin/activate
pip install west
```

You'll need to `source ~/west-venv/bin/activate` (or invoke west
via its full path) for every shell that runs west commands.

### Step 2 — Toolchain (Zephyr SDK 1.0.1)

The MobTurret fork requires SDK ≥ 1.0; the 1.0.1 release auto-selects.

```bash
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v1.0.1/zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz
mkdir -p ~/zephyr-sdk-1.0.1
tar -xJf zephyr-sdk-1.0.1_linux-x86_64_minimal.tar.xz -C ~/zephyr-sdk-1.0.1 --strip-components=1
~/zephyr-sdk-1.0.1/setup.sh
```

(Optional: if you also want SDK 0.17.4 available for testing fallback
builds — NOT required — install it the same way into
`~/zephyr-sdk-0.17.4/`. CMake will still prefer 1.0.1.)

### Step 3 — Clone the project

```bash
git clone git@github.com:ltdat1095/mobturret.git
cd mobturret
git submodule update --init --recursive
```

This populates `gun_bot/m33_firmware/nxp_zephyr` from the user's
`ltdat1095/zephyr` fork at the recorded gitlink (currently
`4673670d075`).

### Step 4 — Populate the west workspace

`nxp_zephyr/west.yml` declares every project needed: `hal_nxp`,
`cmsis`, `cmsis-nn`, `mbedtls`, `hal_st`, `hal_infineon`, … — all
pinned to specific SHAs.

```bash
cd gun_bot/m33_firmware
# One-time, from inside m33_firmware/:
west init -l nxp_zephyr      # creates .west/ writing the manifest path
west update                  # clones every project in the manifest
```

`west update` pulls **≈ 7 GB** of git repos on first run. Expect
**10–15 minutes** on a typical broadband connection (measured: 6m27s
on a 100 Mbps link). Each project is cloned once into
`m33_firmware/modules/`, `bootloader/`, or `tools/`. Subsequent
`west update` invocations are fast — they only fetch new commits on
the manifest's pinned SHAs.

Verify the critical entry:

```bash
cd modules/hal/nxp
git log --oneline -1
# Expected: c7f1b8449 MobTurret: lpuart: hardcode SBR=1, OSR=24 for 24 MHz + 1 Mbaud on iMX93
```

### Step 5 — Build

```bash
cd gun_bot/m33_firmware/gun_controller
cmake --preset=debug
cmake --build --preset=debug
```

Output: `debug/zephyr/zephyr.elf` (≈ 720 KB ELF; ≈ 36 KB FLASH in the
`.text` section).

For a release build:
```bash
cmake --preset=release
cmake --build --preset=release
```

### Step 6 — Deploy to the FRDM

Wire GPIO_IO14 ↔ GPIO_IO15 on the board for loopback. Then:

```bash
scp debug/zephyr/zephyr.elf root@<frdm-ip>:/lib/firmware/zephyr_vN.elf   # N = unique each deploy
ssh root@<frdm-ip> '
  echo stop  > /sys/class/remoteproc/remoteproc0/state
  sleep 2
  echo /lib/firmware/zephyr_vN.elf > /sys/class/remoteproc/remoteproc0/firmware
  echo start > /sys/class/remoteproc/remoteproc0/state
  sleep 3
  cat /sys/class/remoteproc/remoteproc0/state   # expect "running"
'
```

The kernel caches firmware by name — use a new filename each redeploy.

### Step 7 — Verify on hardware

```bash
# M33 state
ssh root@<frdm-ip> 'cat /sys/class/remoteproc/remoteproc0/state'
# → running

# M33's printk output (LPUART2 console)
sudo /usr/bin/python3 -c "
import serial, time, threading, subprocess

tty_ready = threading.Event()
result = {'bytes': b''}
def reader():
    with serial.Serial('/dev/ttyACM1', 115200, timeout=0.5) as s:
        s.reset_input_buffer()
        tty_ready.set()
        t0 = time.monotonic()
        total = bytearray()
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
```

Expected:
```
*** Booting Zephyr OS build 4673670d0752 ***
============================================================
 MobTurret M33 firmware — LPUART3 @ 1 Mbaud
 Zephyr SDK 1.0.1 / GCC 14.3.0
------------------------------------------------------------
 LPUART3 BAUD  = 0x17000001 (1 Mbaud OK)
 LPUART3 STAT  = 0x00c00000
[N] LPUART3: loopback OK BAUD=0x17000001 STAT=0x40d00000
```

## 4. Build troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `m33_firmware/modules/` is empty after `west update` | west wasn't run from `m33_firmware/`, or `--init -l nxp_zephyr` skipped | `cd gun_bot/m33_firmware && west init -l nxp_zephyr && west update` |
| `modules/hal/nxp` SHA ≠ `c7f1b8449` | west.yml pin drifted | Update `nxp_zephyr/west.yml` to pin `c7f1b8449`, or run `west update` again |
| `*** Booting Zephyr …` but no further `printk` | LPUART2 console driver bound, but A55 getty is on `ttyLP0` and fights M33 for the wire | `ssh root@<frdm-ip> 'systemctl stop serial-getty@ttyLP0.service; echo 0 > /proc/sys/kernel/printk'` |
| M33 boots, A55 wifi drops within seconds of `echo start` | SoC crash. See [`../INIT_SOURCE_PROBLEM.md`](../INIT_SOURCE_PROBLEM.md) § 2.3. | Make sure `src/main.cpp` has the `lpuart_clocks_init` SYS_INIT hook at `PRE_KERNEL_1 prio 0`. |
| `debug/zephyr/zephyr.dts` has no `lpuart3` node | stale cmake cache or wrong overlay filename | `rm -rf debug/CMakeCache.txt debug/CMakeFiles && cmake --preset=debug` |
| `fsl_lpuart.c:586: 'lpuart_config_t' has no member named 'enableTxRTS'` | This is the OLD NXP-downstream SDK error from before the 2026-06-28 fork migration. Should not happen on the current fork. | If you see this, your `nxp_zephyr` is on the wrong SHA. `cd gun_bot/m33_firmware/nxp_zephyr && git log --oneline -1` — should be `4673670d075` |

For deeper issues (TRDC violations, bus faults, GPIO access patterns),
see [`../INIT_SOURCE_PROBLEM.md`](../INIT_SOURCE_PROBLEM.md) § 2 — the
root-cause analysis there explains *why* these happen, not just
*what* to do.

## 5. Updating vendor sources

To bump the MobTurret zephyr fork to a newer upstream `main`:

```bash
cd gun_bot/m33_firmware/nxp_zephyr
git remote add upstream https://github.com/zephyrproject-rtos/zephyr.git
git fetch upstream
git checkout mobturret/m33-lpuart3-clock-and-overlay
git rebase upstream/main
# Resolve any conflicts in the LPUART3 dtsi / clock_control_mcux_ccm_rev2.c.
```

Then re-run `cd ../.. && west update` to refresh all `modules/*` pins
and rebuild. Update `gun_bot/CLAUDE.md` and `BUILD_SYSTEM.md` § 2
with the new SHA.

To bump the `hal_nxp` SDK baud override: pull new SDK source from
upstream `hal_nxp`, rebase the MobTurret branch on top, update the
`hal_nxp` pin in `nxp_zephyr/west.yml`, and commit both the
re-pinned submodule and the new west.yml in the same commit so the
fork SHA never drifts.
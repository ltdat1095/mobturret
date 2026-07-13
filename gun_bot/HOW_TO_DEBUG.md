# How to deploy and test M33 firmware on the FRDM-iMX93

This is the **operational playbook** for bringing up MobTurret's M33
servo-bus firmware on real hardware. It covers the full cycle —
build, deploy, monitor, debug, recover — and was written after the
2026-07-11 session that brought up the SC15 servo bus.

**Status: re-verified end-to-end on 2026-09-02.** The §3 deploy
sequence and §5 console capture work as written. Two deltas found
that run: the bus now answers `ids={2}` only (id=1 is silent — see
§5), and §10 issue #1 is fixed (see §10).

This doc is linked from [`../CLAUDE.md`](../CLAUDE.md) and
[`./CLAUDE.md`](./CLAUDE.md). If you add a hardware procedure, add it
here rather than starting a new file.

For the project context see [`../CLAUDE.md`](../CLAUDE.md). For the
servo-bus wiring / SCSCL protocol details see
[`./SERVO_SETUP.md`](./SERVO_SETUP.md). For the A55 ↔ M33 wire
contract see [`./IPC.md`](./IPC.md).

---

## 1. Environment

| Tool | Path / version |
|---|---|
| CMake | `/home/ltdat/.mcuxpressotools/cmake-3.30.0-linux-x86_64/bin/cmake` |
| Ninja | `/home/ltdat/.mcuxpressotools/ninja-1.12.1/ninja` |
| Zephyr SDK | `~/zephyr-sdk-1.0.1` (auto-selected by cmake) |
| ARM toolchain | `/home/ltdat/.mcuxpressotools/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi/bin/` |
| Zephyr base | `mobturret/gun_bot/m33_firmware/nxp_zephyr` |
| `hal_nxp` fork | `mobturret/gun_bot/m33_firmware/modules/hal/nxp` |
| Working combo | `zephyr@4673670d075` + `hal_nxp@c7f1b8449` (see [`./INIT_SOURCE_PROBLEM.md`](./INIT_SOURCE_PROBLEM.md)) |

A55 (Linux) access:
- IP `192.168.1.94` (mlan0 wifi), SSH as root (no password — key auth)
- `mcimx93_evk_blank_cm33.elf` (standalone MCUXpresso) and `zephyr*.elf` (Zephyr) live in `/lib/firmware/`

Host serial:
- `/dev/ttyACM0` = A55 debug console (root login)
- `/dev/ttyACM1` = M33 firmware console (LPUART2 → CH9102 channel B @ 115200 baud)
- `/dev/ttyACM1` is mode `0666`, so no `sudo` needed to read
- `/dev/ttyACM0` is mode `0660 root:dialout`, so `sudo` IS needed; use the `serial_io.py` helper in `~/.claude/skills/a55-wifi/`

---

## 2. Build

```bash
cd gun_bot/m33_firmware/gun_controller
cmake --build --preset=debug
```

Output: `debug/zephyr/zephyr.elf` (~36 KB FLASH).

Build artifacts under `debug/`, `release/`, `build/`, `install/`, `log/`
are gitignored — only `src/`, `boards/`, `prj.conf`, `CMakeLists.txt`,
`CMakePresets.json`, `mcux_include.json`, and `sample.yaml` are tracked.

To wipe and rebuild:
```bash
rm -rf debug && cmake --preset=debug && cmake --build --preset=debug
```

---

## 3. Deploy via A55 remoteproc

The M33 is loaded by A55 Linux via `/sys/class/remoteproc/remoteproc0/`,
NOT booted standalone. Deploy sequence:

```bash
# 3a. Copy the ELF to the A55
scp -o StrictHostKeyChecking=no \
  gun_bot/m33_firmware/gun_controller/debug/zephyr/zephyr.elf \
  root@192.168.1.94:/lib/firmware/zephyr_test.elf

# 3b. Stop the running M33 firmware (retry up to 8x)
ssh -o StrictHostKeyChecking=no root@192.168.1.94 bash <<'EOF'
for i in 1 2 3 4 5 6 7 8; do
  s=$(cat /sys/class/remoteproc/remoteproc0/state)
  [ "$s" = "offline" ] && break
  echo stop > /sys/class/remoteproc/remoteproc0/state
  sleep 2
done
EOF

# 3c. Point remoteproc at the new firmware and start
ssh -o StrictHostKeyChecking=no root@192.168.1.94 bash <<'EOF'
echo "/lib/firmware/zephyr_test.elf" > /sys/class/remoteproc/remoteproc0/firmware
echo start > /sys/class/remoteproc/remoteproc0/state
sleep 3
cat /sys/class/remoteproc/remoteproc0/state   # should print "running"
EOF
```

If `echo stop` returns `Device or resource busy`, the previous M33 is
still tearing down — re-run the loop.

### Two things that make this go smoothly

**1. Diff before you deploy.** `/lib/firmware/` has 80+ `zephyr*.elf`
files from past sessions, and it's easy to redeploy a binary you
already tested. Compare first:

```bash
md5sum gun_bot/m33_firmware/gun_controller/debug/zephyr/zephyr.elf
ssh root@192.168.1.94 'md5sum /lib/firmware/zephyr_<candidate>.elf'
```

Matching md5 means this exact firmware already ran on the board —
useful for knowing whether you're re-testing a known quantity or
booting something new. Name new uploads with a date
(`zephyr_ping_20260902.elf`), not another `_v N`.

**2. Start the console reader BEFORE `echo start`.** The boot banner
prints once, within the first second. If you attach to `/dev/ttyACM1`
after starting the M33, you miss the banner (and therefore the
`BAUD =` line that tells you whether the clock hooks worked) and see
only the periodic scan ticks. Launch the §5 reader in the background
first, then run the stop/start sequence.

Note `echo ... > .../firmware` takes a path *relative to
`/lib/firmware`* — `zephyr_ping_20260902.elf`, not the absolute path.
Both happen to work, but `cat firmware` echoes back what you wrote.

---

## 4. The crash signal: A55 wifi drop

**Per memory `feedback-m33-crash-signal`:**
> post-deploy A55 wifi drop means M33 firmware crashed the SoC, not a wifi issue.

**Rule:** within 30 s of `echo start`, run `ping 192.168.1.94`. If
ping goes to 100 % loss, the firmware crashed the SoC — **do NOT
re-run `/a55-wifi`**, recover via physical unplug (see §6).

A successful boot looks like:
```
64 bytes from 192.168.1.94: icmp_seq=1 ttl=64 time=4.2 ms
...
4 packets transmitted, 4 received, 0% packet loss
```

A crash looks like:
```
4 packets transmitted, 0 received, +DUP, 100% packet loss
```

Confirm the other direction too — a healthy start logs this on the A55
(`dmesg | grep -E "remoteproc|imx-rproc"`):

```
remoteproc remoteproc0: powering up imx-rproc
remoteproc remoteproc0: Booting fw image zephyr_ping_20260902.elf, size 719420
remoteproc remoteproc0: No resource table in elf
remoteproc remoteproc0: remote processor imx-rproc is now up
```

`No resource table in elf` is **expected and harmless** for the
current firmware — it just means no RPMsg/virtio vrings are declared
yet. It becomes a real error only once [`./IPC.md`](./IPC.md) RPMsg
lands, which needs a `.resource_table` section.

Ping at T+0, T+30 s, and once a few minutes later. The 2026-07-13
crashes killed wifi within seconds, but the watchdog reboot in
[`./INIT_SOURCE_PROBLEM.md`](./INIT_SOURCE_PROBLEM.md) §1 case 3
arrived ~10 minutes after `echo start` — an early pass alone is not
proof of stability.

---

## 5. Monitor M33 console

```bash
/usr/bin/python3 -c "
import serial, time
with serial.Serial('/dev/ttyACM1', 115200, timeout=0.5) as s:
    s.reset_input_buffer()
    t0 = time.monotonic()
    total = b''
    while time.monotonic() - t0 < 12.0:
        chunk = s.read(4096)
        if chunk:
            total += chunk
print(total.decode('utf-8', errors='replace'))
"
```

Common boot output (HEAD scan firmware, captured 2026-09-02):
```
============================================================
 MobTurret M33 firmware — LPUART3 @ 1 Mbaud (ping-only)
 Zephyr SDK 1.0.1 / GCC 14.3.0
 Console: LPUART2 -> /dev/ttyACM1 (host)
 Servo bus: LPUART3 @ 0x42570000, GPIO_IO14/IO15
------------------------------------------------------------
 LPUART3 BAUD  = 0x17000001 (1 Mbaud OK)
 LPUART3 STAT  = 0x00c00000
[0] PING OK: ids={2}
[1] PING OK: ids={2}
...
```

What to read from the banner:

- `LPUART3 BAUD  = 0x17000001` — the SDK's baud-search loop agreed
  with the 1 Mbaud override in the `hal_nxp` fork; clock root
  + baud fixup are working. If this is anything else, the LPUART3
  clock hook from [`./INIT_SOURCE_PROBLEM.md`](./INIT_SOURCE_PROBLEM.md)
  §2.3 didn't run — do not expect the SC15s to answer.
- `LPUART3 STAT  = 0x00c00000` — bits 23 (TDRE) and 22 (TC) set,
  i.e. the TX holding register is empty and the TX shift register
  finished. No overrun (bit 19), no break, no match-1 — clean.
- `PING OK: ids={N}` — broadcast ping every 1 s; `N` is the set of
  SC15 IDs that answered. On 2026-09-02 only `{2}` answered
  (id=1 has been silent since the 2026-07-13 v19 over-speed
  incident). If your session shows `{1,2}` and a different STAT
  value, that's also fine — anything in `{1,2}` with no FAIL
  lines is a healthy bus. **The 16-byte RX FIFO** (see
  [`./INIT_SOURCE_PROBLEM.md`](./INIT_SOURCE_PROBLEM.md) §2.4)
  is plenty for these 6-byte pings.

A SCAN firmware (the previous head, with the
direction-test helpers still in `main.cpp`) prints the same banner
*without* "(ping-only)" and adds `[TEST]` / `[POS]` / `[OK]`
lines after the scan. See [`./SERVO_SETUP.md`](./SERVO_SETUP.md)
for the format of motion-test output.

Capture for as long as you need — the kernel blocks forever in the
scan thread; you can capture 60 s and still see live ticks.

### The printk-mystery workaround

In the known-good combo (`zephyr@4673670d075` + `hal_nxp@c7f1b8449`),
`printk` output **does not** reach `ttyACM1` until the M33 worker
thread runs at least one `k_sleep(K_SECONDS(N))`. This is per
[`./INIT_SOURCE_PROBLEM.md`](./INIT_SOURCE_PROBLEM.md) §2 — the
printk backend buffers but doesn't flush until the kernel yields.

**Rule:** in any custom firmware, use `k_sleep(K_SECONDS(N))` or
`k_msleep(N)` (NOT busy-wait NOP loops) for delays. Without yielding,
no printk reaches the host — your firmware runs silently.

If you must use a busy-wait (e.g., tight polling on UART), insert an
explicit `k_msleep(1)` every ~50 iterations to flush printk output.

---

## 6. Recovery — physical unplug-replug

**Per memory `frdm-recovery-no-sudo`:** `sudo` requires a password,
`uhubctl` is not installed. The QinHeng debug cable (Bus 003 Port 7,
1a86:55d2) is the **SOLE** external USB device; VBUS from the cable
powers the FRDM.

When the M33 firmware crashes the SoC:
1. **Physical unplug** the QinHeng USB cable from the dev box
2. Wait ~15 s (capacitor drain)
3. **Replug**
4. Wait ~30 s for A55 to boot
5. SSH in: `ssh root@192.168.1.94` (may take a few attempts)
6. Run `/a55-wifi` to bring wifi back up (or use the manual sequence in `~/.claude/skills/a55-wifi/SKILL.md`)

The cable-replug is the only software-independent recovery path. There
is no `/sbin/reboot`, no power-button daemon, and no `sudo reboot`.

---

## 7. State checks

```bash
# M33 state
ssh root@192.168.1.94 cat /sys/class/remoteproc/remoteproc0/state
# → "running", "offline", or "stopping"

# M33 currently-loaded firmware
ssh root@192.168.1.94 cat /sys/class/remoteproc/remoteproc0/firmware
# → e.g. "/lib/firmware/zephyr_test.elf"

# A55 wifi IP
ssh root@192.168.1.94 ip -br addr show mlan0
# → "mlan0 ... inet 192.168.1.94/24 ..."

# dmesg (remoteproc events)
ssh root@192.168.1.94 'dmesg | grep -E "remoteproc|imx-rproc" | tail -10'

# List firmware files on A55
ssh root@192.168.1.94 ls -la /lib/firmware/zephyr*.elf
```

---

## 8. Safety checks for servo motion

> **Authoritative source:** [`./SERVO_SETUP.md`](./SERVO_SETUP.md) **§0
> "SAFETY PROTOCOL"** — hard caps, abort conditions, pre-flight
> checklist, calibration protocol, and failure history. This section
> is the *operational* layer; the §0 doc is the *contract*.

The servo bus is half-duplex at 1 Mbaud. Each `WritePos` packet is
**13 bytes**; each `WriteByte` is **8 bytes**. The M33 RX FIFO is **16
bytes deep** (see [`./INIT_SOURCE_PROBLEM.md`](./INIT_SOURCE_PROBLEM.md)
§2.4). Sticking a stuck servo at a mechanical limit draws current,
heats up, and can break the horn mounting — the SC15 got to **64 °C**
during one violent back-and-forth test (max safe ~50 °C). Twice
the harness has been damaged by extrapolating beyond the verified
envelope (see [`./SERVO_SETUP.md`](./SERVO_SETUP.md) §0.7).

**Pre-flight checklist before any motion:**

1. **Read PRESENT_TEMPERATURE** (register 63, 1 byte). If > 50 °C, do NOT enable torque — let it cool.
2. **Read PRESENT_LOAD** (register 60, 2 bytes). Bit 10 = direction (0=CCW, 1=CW); bits 0–9 = magnitude (0–1000 = 0–100 %). Sustained load > 700 = hitting a stop.
3. **Read PRESENT_POSITION** (register 56, 2 bytes). SC15 firmware reports a **16-bit value** (range observed: 5120–60163). NOT the standard 0–4095.
4. **Check MIN/MAX_ANGLE_LIMIT** (registers 9, 11). Soft limits configured in EPROM — the servo won't physically exceed these, even if commanded beyond.
5. **Set TIME to ≥ 1500 ms** for any non-trivial move. Peak velocity ≈ swing/1500 (units/ms); < 100 units/ms is gentle.
6. **Set SPEED = 0** to let TIME govern the move (don't impose a speed cap).
7. **Watch the loop**: alternate target by a small amount (±2000–5000) and check that actual ≈ target ± some delta. If actual doesn't track, the servo is stuck — stop pushing.
8. **Cap motion duration**: never run alternating targets faster than 1 Hz. Sustained reversal at 0.5 Hz with TIME=1500 ms is enough to damage horns.
9. **Confirm the speed/duration is inside the §0 hard caps** —
   `MAX_SAFE_SPEED = 10`, `MAX_SAFE_DUR_MS = 108`,
   `MAX_STEPS_PER_SESSION = 20`, `MAX_DELTA_PER_STEP = 14000`. These
   constants must be declared in the M33 source and used by every
   motion command (no magic numbers in the firmware).

**If temp spikes > 55 °C**:
- Disable torque (`SCSCL_TORQUE_ENABLE = 0`)
- Wait at least 30 s before retrying
- Investigate the mechanical setup before resuming

---

## 9. Cheatsheet

```bash
# Build
cd gun_bot/m33_firmware/gun_controller && cmake --build --preset=debug

# Deploy + restart
scp -o StrictHostKeyChecking=no \
  debug/zephyr/zephyr.elf \
  root@192.168.1.94:/lib/firmware/zephyr_test.elf
ssh -o StrictHostKeyChecking=no root@192.168.1.94 bash <<'EOF'
for i in 1 2 3 4 5 6 7 8; do
  s=$(cat /sys/class/remoteproc/remoteproc0/state)
  [ "$s" = "offline" ] && break
  echo stop > /sys/class/remoteproc/remoteproc0/state
  sleep 2
done
echo "/lib/firmware/zephyr_test.elf" > /sys/class/remoteproc/remoteproc0/firmware
echo start > /sys/class/remoteproc/remoteproc0/state
sleep 3
cat /sys/class/remoteproc/remoteproc0/state
EOF

# Watch wifi (must stay up = no SoC crash)
ping -c 5 -W 2 192.168.1.94

# Read M33 console
/usr/bin/python3 -c "
import serial, time
with serial.Serial('/dev/ttyACM1', 115200, timeout=0.5) as s:
    s.reset_input_buffer()
    t0 = time.monotonic()
    total = b''
    while time.monotonic() - t0 < 12.0:
        chunk = s.read(4096)
        if chunk:
            total += chunk
print(total.decode('utf-8', errors='replace'))
"

# If wifi drops after deploy → physical unplug QinHeng, wait 15 s, replug, wait 30 s, ssh in, run /a55-wifi
```

---

## 10. Issue history (working off the 2026-07-11 list)

1. **GPIO / SCSCL library additions crash the SoC** —
   **RESOLVED 2026-07-13**. Root cause was the missing
   `CLOCK_SetRootClock` for the LPUART3 clock root, not GPIO or
   SCSCL itself. Fixed via the SDK-direct clock-init SYS_INIT hook
   (`imx93_m33_clock_init`) at PRE_KERNEL_1 prio 0. The motion
   firmware in this repo's `src/main.cpp` (before today's ping-only
   revert) ran `write_reg` / `read_reg` / `WritePWM` with hand-built
   packets and never crashed the SoC after the fix. See
   [`./INIT_SOURCE_PROBLEM.md`](./INIT_SOURCE_PROBLEM.md) §2.3
   for the full diagnosis and the bisect table in
   `project-m33-build-bisect-2026-07-09.md` for the failed
   experiments. The ping-only firmware shipped 2026-09-02 is the
   **bus-presence minimum** — re-add motion by porting helpers from
   a `git log -p` of `src/main.cpp` (the direction-test thread in
   the v19 era is the reference).
2. **`rx_packet` infinite-loop bug.** Fixed in current firmware. The
   original `rx_packet` would spin forever on a single missed byte
   (the inner timeout fired but the outer `while` loop kept going
   because `idx == 0U` was false). Fixed by tracking `got_byte` and
   returning false on inner-loop timeout.
3. **`k_msleep(N)` vs `busy_wait(NOPs)`**. The latter starves the
   printk flush path. Use `k_sleep(K_SECONDS(N))` or `k_msleep(N)`
   for any delay ≥ 1 ms in the worker thread.
4. **PRINTK FLOOD.** When multiple threads print rapidly, LPUART2 TX
   buffer can drop output. The 3-line "for (int i = 0; i < 3; i++)
   printk(...)" pattern at the scan thread survives; per-tick single
   printks in the motion thread are also fine. Don't flood with more
   than ~10 lines/sec.
5. **Position register is 16-bit** (not the standard 12-bit SC15).
   See [`./SERVO_SETUP.md`](./SERVO_SETUP.md) §5. Range is configured
   in EPROM at MIN/MAX_ANGLE_LIMIT (registers 9, 11).
6. **id=1 silent on the bus** — observed 2026-09-02: the scan
   reports `ids={2}` only. id=1 has not answered ping since the
   2026-07-13 v19 over-speed event (58,878-unit excursion in 3 s).
   Likely faulted. To rule out a renumbered ID, widen
   `PING_SCAN_ID_MAX` in `src/main.cpp` from `5U` to `253U` and
   re-deploy — still ping-only, no motion.

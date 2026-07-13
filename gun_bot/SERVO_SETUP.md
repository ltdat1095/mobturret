# Servo setup — FRDM-iMX93 M33 ↔ Waveshare Bus Servo Adapter (A) ↔ SC15 × 2

## 0. SAFETY PROTOCOL for motion testing (READ FIRST)

The SC15 servos on the bench have a **narrow verified-safe envelope**.
Anything outside it can physically break the horn mounting, strip the
gears, or latch a permanent fault state. This section is non-negotiable.

### 0.1 Hard caps — DO NOT EXCEED

| Parameter | Hard cap | Source |
|---|---|---|
| Speed command value (bytes to GOAL_TIME_L) | **≤ 10** | User-calibrated 2026-07-12 |
| Duration of one motion step | **≤ 108 ms** | User-calibrated 2026-07-12 |
| Steps per test session (one direction) | **≤ 20** | User-calibrated 2026-07-12 (80° total) |
| Cumulative motion per session | **≤ 80°** (≈ 97,440 PRESENT_POSITION units) | User-measured 80° in 20×108ms |

**Do NOT extrapolate.** speed=50 for 200ms was tried twice (2026-07-13
session v18 + v19) and **broke hardware both times** — id=1 jumped 58,878
units in 3 s, id=2 entered a fault state. The SC15 firmware's speed-to-
rotation mapping is NOT linear with my assumed units, so any value
beyond speed=10 is unverified and unsafe.

### 0.2 Step-by-step only — NO single max-range attempts

Every motion test **must** be a sequence of small steps with verification
between each. Specifically:

```
for step in 1..MAX_STEPS_PER_SESSION:
    1. Read PRESENT_POSITION (start_pos)
    2. Send speed=10, dur=108 ms
    3. Wait 150 ms (motion + bus settle)
    4. Send stop (speed=0)
    5. Read PRESENT_POSITION (end_pos)
    6. Read PRESENT_SPEED (should be 0 = stopped)
    7. Compute delta = end_pos - start_pos (handle 16-bit wraparound)
    8. ABORT conditions (see §0.3) — if any triggered, STOP immediately
    9. Optional: read PRESENT_TEMP, PRESENT_LOAD, PRESENT_VOLTAGE
   10. k_msleep(100) — settle before next step
```

The total motion per session is the **sum** of the per-step deltas,
across at most 20 steps. A single 3 s motion command is **never**
acceptable — that's exactly what caused the 2026-07-13 double-fault.

### 0.3 Abort conditions — STOP IMMEDIATELY if any is true

1. **PRESENT_SPEED > 0 unexpectedly** (e.g., you commanded stop but
   servo is still turning) → STOP, send speed=0 again, halt test.
2. **PRESENT_SPEED = 0 during a motion command** → STOP. The servo
   should be rotating; if it's not, something is wrong (stuck,
   faulted, no torque). Do NOT keep sending motion commands.
3. **|delta| > 2 × expected** (expected ≈ 5,069..6,912 units per step
   per the 2026-07-12 calibration; cap = 14,000) → STOP. Wild
   jumps like the 58,878-unit excursion in v19 mean the servo is
   responding to something we didn't command.
4. **PING fails** (no ACK within 2,000 polls) → STOP. Servo has
   entered a fault state (same pattern as 2026-07-12's speed=1500
   incident).
5. **PRESENT_TEMP > 60 °C** → STOP, let servo cool before retrying.
6. **PRESENT_VOLTAGE < 4.5 V or > 8.5 V** (SC15 spec is 6..8.4 V
   typical) → STOP, check power supply.

When an abort fires: (a) send stop, (b) re-enable torque only if it
was disabled, (c) print the failure reason over the console so the
host sees it, (d) halt the test thread (let main loop continue).

### 0.4 Pre-flight checklist (run before any motion test)

For each servo id you'll be moving:

1. **PING** — must respond within 2,000 polls.
2. **Read firmware version** (reg 0, reg 1). Confirm it's 0.5 (the
   firmware version we have documented register maps for). Anything
   else, abort.
3. **Read MIN_ANGLE_LIMIT_L and MAX_ANGLE_LIMIT_L** (reg 9, 11).
   - Both = 0 → wheel mode (good for continuous rotation)
   - Non-zero, non-trivial → position mode (use `WritePos` for motion)
4. **Read TORQUE_ENABLE** (reg 40). If 0, write 1 (with retry).
5. **Read PRESENT_TEMP** (reg 63). Must be < 60 °C. If higher,
   halt and cool down.
6. **Read PRESENT_VOLTAGE** (reg 62). Must be in 4.5..8.5 V range.

### 0.5 Hard-coded safety constants (firmware)

The M33 firmware **must** declare these as constants and use them
in every motion command, so that no future test can accidentally
exceed the envelope without a code change:

```c
/* Hard-coded safety envelope. DO NOT CHANGE without first running
 * the calibration protocol in §0.6 and confirming with the user. */
#define MAX_SAFE_SPEED        10U      /* bytes to GOAL_TIME_L */
#define MAX_SAFE_DUR_MS       108U     /* per step */
#define MAX_STEPS_PER_SESSION 20U      /* ~80° cumulative */
#define MAX_DELTA_PER_STEP    14000    /* ~2x expected upper bound */
#define PRESENT_SPEED_TIMEOUT_MS 250U  /* wait time after stop command */
```

Any future motion command that doesn't use these constants is a bug
and should be rejected in code review.

### 0.6 Calibration protocol (only if §0.1 caps prove insufficient)

The user's 2026-07-12 calibration is the binding baseline. To safely
extend it (e.g., to find a higher speed cap):

1. **Pick one new parameter to vary**. Hold all others at the
   verified-safe baseline (speed=10, dur=108 ms).
2. **Make the change tiny**. E.g., speed=10→11, or dur=108→110.
3. **Run a single step**. Verify PRESENT_SPEED is ~expected, |delta|
   is in expected range, PRESENT_TEMP didn't spike.
4. **Run three more steps** at the same new value to check
   consistency.
5. **If all four steps look good**, update the cap in firmware, log
   the change in this doc with date + measured delta.
6. **If any step looks wrong**, revert to the previous cap.

Never try to skip ahead (e.g., speed=10 → 50 in one jump). Each
calibration step must be tiny and verified.

### 0.7 Why this protocol exists

History of failures (motivating the safety rules):

- **2026-07-12 discovery**: user ran speed=1500 for 2 s on id=2.
  Hardware was damaged, fault state latched. id=2 has been flaky ever
  since.
- **2026-07-13 v18 test**: ran speed=50 for 100 ms — safe (508 units).
- **2026-07-13 v19 test**: ran speed=50 for 3 s on **both** id=1
  and id=2 — id=1 jumped 58,878 units in 3 s, id=2 entered fault
  state. Same failure mode as the 2026-07-12 incident.

The cap of **speed=10, dur=108 ms** is the ONLY empirically-validated
safe combination. Everything beyond it is unsafe until proven
otherwise by the §0.6 calibration protocol.

---

This is the bring-up record for the **servo bus** half of MobTurret. It
documents the hardware, wiring, firmware, and verification of two SC15
servos driven from the M33's LPUART3 at 1 Mbaud via a Waveshare Bus
Servo Adapter (A).

For the broader M33 firmware context, see
[`./m33_firmware/BUILD_SYSTEM.md`](./m33_firmware/BUILD_SYSTEM.md)
and [`./INIT_SOURCE_PROBLEM.md`](./INIT_SOURCE_PROBLEM.md). For the
A55 ↔ M33 wire contract (the next layer up once servos are reachable),
see [`./IPC.md`](./IPC.md).

---

## 1. Hardware

```
+---------------------+        +-------------------------+      +-----------+
|   FRDM-iMX93 (M33)  |        |  Waveshare Bus Servo    |      |  SC15 #1  |
|                     |        |  Adapter (A)            |      |  ID = 1   |
|  LPUART3 TX  ------ GPIO_IO14 ---> RX                   |      |           |
|  LPUART3 RX  <----- GPIO_IO15 <--- TX                   |      |           |
|  GND          --------------- GND ----------------------|--GND--|GND       |
|                     |        |                         |      |           |
+---------------------+        |  Servo bus out (3-pin)  |      |  SC15 #2  |
                               |  Signal ----- (daisy) --|------|Signal     |
                               |  VCC (5V) -- (shared) --|------|V+         |
                               |  GND     -- (shared) --|------|GND        |
                               +-------------------------+      +-----------+
```

- **Board:** FRDM-iMX93, **M33 core** (MIMX9352)
- **Servo bus UART:** LPUART3 @ **1,000,000 baud**
  - TX: `GPIO_IO14` (`iomuxc1_gpio_io14_lpuart_tx_lpuart3_tx`)
  - RX: `GPIO_IO15` (`iomuxc1_gpio_io15_lpuart_rx_lpuart3_rx`)
  - Base: `0x42570000` (M33 non-secure alias), NVIC IRQ 68
- **Adapter:** Waveshare Bus Servo Adapter (A) — transparent
  full-duplex ↔ half-duplex bridge. **TX/RX is split** on the host
  side; the adapter handles the bus turnaround. 5 V logic level;
  3.3 V M33 IO is read-tolerant for short bench wiring.
- **Servos:** 2 × SC15 serial servos, **SCSCL protocol, 1 Mbaud**
  factory default. Daisy-chained on the adapter's 3-pin servo bus
  header (Signal/VCC/GND). Daisy-chain order doesn't matter — servos
  are addressed by ID.
- **Servo IDs:** both at the factory-default IDs **1 and 2**.

---

## 2. M33 firmware (per-ID scan)

`gun_bot/m33_firmware/gun_controller/src/main.cpp` runs a scan- ping
worker thread (K_THREAD_DEFINE, prio 7). Every 1 s it issues five
SCSCL `PING` packets, one per ID `1..5`, and reports which IDs
responded:

```
[19] PING OK: ids={1,2}
[20] PING OK: ids={1,2}
...
[28] PING OK: ids={1,2}
```

### Why per-ID, not broadcast

A **broadcast PING** (`ID=0xFE`) makes every servo on the bus
respond at the same instant. On the shared half-duplex wire, the
responses **collide** — most packets are garbled and we see ~20 %
pass rate, with the occasional lucky framing giving a bogus ID.
Per-ID scan avoids the collision entirely: each servo responds in
its own time slot.

**Rule:** never broadcast PING on a multi-servo bus. Always ping
each ID individually.

### What the firmware touches (and what it deliberately doesn't)

- ✅ `lpuart_clocks_init` SYS_INIT (PRE_KERNEL_1 prio 0) — opens
  the LPUART3 clock root + IP gate. Required for BAUD to be
  non-zero (see [`./INIT_SOURCE_PROBLEM.md`](./INIT_SOURCE_PROBLEM.md)).
- ✅ `uart_poll_in` / `uart_poll_out` against the DTS-bound
  `lpuart3` device — same path as the snapshot's loopback test.
- ✅ SCSCL PING packet, hand-built byte-by-byte. Packet layout:
  `0xFF 0xFF ID 0x02 0x01 ~sum`. Six bytes — well under the
  16-byte RX FIFO.
- ❌ **No GPIO** (no LED feedback, no DT spec lookups).
- ❌ **No `SCSCL` library** (no `#include "servo/SCSCL.h"`).

The GPIO / SCSCL-library additions cause a SoC crash on deploy
(crash signal = A55 wifi drops within seconds of `echo start`).
The minimal-delta firmware here avoids them entirely. See
§5 below.

---

## 3. Build & deploy

```bash
cd gun_bot/m33_firmware/gun_controller
cmake --build --preset=debug

# Push to FRDM-iMX93
scp -o StrictHostKeyChecking=no \
  debug/zephyr/zephyr.elf root@192.168.1.94:/lib/firmware/zephyr_scan.elf

# Start via Linux remoteproc on the A55
ssh -o StrictHostKeyChecking=no root@192.168.1.94 bash <<'EOF'
echo stop > /sys/class/remoteproc/remoteproc0/state
for i in 1 2 3 4 5; do
  s=$(cat /sys/class/remoteproc/remoteproc0/state)
  [ "$s" = "offline" ] && break
  sleep 2
done
echo "/lib/firmware/zephyr_scan.elf" > /sys/class/remoteproc/remoteproc0/firmware
echo start > /sys/class/remoteproc/remoteproc0/state
sleep 4
echo "state: $(cat /sys/class/remoteproc/remoteproc0/state)"
EOF
```

Build size: ~36 KB FLASH, ~10.7 KB RAM (no GPIO, no SCSCL library).

---

## 4. Verification

Two signals to confirm a healthy servo bus:

**Primary — wifi stays up after deploy.** The known-good combo
(`zephyr@4673670d075` + `hal_nxp@c7f1b8449`) does NOT crash the
SoC. If `ping 192.168.1.94` starts dropping packets within
seconds of `echo start`, the firmware crashed the SoC — power-
cycle the QinHeng debug cable (see
[`./INIT_SOURCE_PROBLEM.md`](./INIT_SOURCE_PROBLEM.md) and
[`./m33_firmware/BUILD_SYSTEM.md`](./m33_firmware/BUILD_SYSTEM.md)
§4 for the crash signal rule).

**Secondary — M33 console on `/dev/ttyACM1` @ 115200 baud.** A
healthy scan prints one of these every 1 s:

```
[19] PING OK: ids={1,2}
[20] PING OK: ids={1,2}
[21] PING OK: ids={1,2}
```

If only one ID is present, e.g. only the SC15 at ID=1 connected:

```
[19] PING OK: ids={1}
[20] PING OK: ids={1}
```

If nothing is connected (open servo bus):

```
[19] PING FAIL: no servo on IDs 1..5
[20] PING FAIL: no servo on IDs 1..5
```

To capture M33 console output:

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

Note: `/dev/ttyACM1` is `0666`, so no `sudo` is needed for read.

---

## 5. Open issues (deliberately NOT yet addressed)

### 5.1 GPIO + SCSCL library additions crash the SoC

A more featureful ping firmware that adds **GPIO DT spec lookups for
RGB LEDs**, an **`SCSCL` library instance**, and `leds_init()` /
`servo.begin()` / `servo.Ping(0xFE)` on top of the minimal-delta
firmware above **consistently crashes the SoC on deploy** (wifi
drops within seconds of `echo start`). The crash requires a physical
unplug-replug of the QinHeng debug cable to recover — see the
"FRDM recovery" section of
[`./INIT_SOURCE_PROBLEM.md`](./INIT_SOURCE_PROBLEM.md).

The crash culprit is narrowed to one of these eight additions, but
not yet isolated:

1. `#include <zephyr/drivers/gpio.h>`
2. `#include "servo/SCSCL.h"`
3. `GPIO_DT_SPEC_GET(DT_NODELABEL(led_r/g/b), gpios)` at file scope
4. `static SCSCL servo;` at file scope
5. `leds_init()` calling `gpio_pin_configure_dt` 3×
6. `servo.begin(lpuart3_dev)` in `main()`
7. `servo.Ping(0xFE)` call in the worker thread
8. `gpio_pin_set_dt` calls in the worker thread

Bisect plan: add each block back one at a time, deploy, watch wifi.
The minimal-delta firmware here deliberately avoids all eight.

### 5.2 Hello-world printk not visible on ttyACM1

A `printk("Hello world")` from `main()` does **not** appear on
`/dev/ttyACM1` in the known-good combo (`zephyr@4673670d075` +
`hal_nxp@c7f1b8449`). The printk is sent; it just doesn't reach
the host. Once the worker thread's `printk` is **flooded 3× per
second** (which this firmware does, and which the loopback firmware
also does), the host DOES see it — suggesting the issue is a
collision with the A55 Linux debug console, not a missing init
step.

---

## 6. Next steps

Once this scan is healthy, the natural progression is:

1. **Read present position** — `SCSCL::ReadPos(ID)` returns the
   current servo angle as a raw SCSCL value (0–4095). One read
   packet: `0xFF 0xFF ID 0x04 0x02 0x24 0x02 ~sum`. Response:
   `0xFF 0xFF ID LEN ERR POS_L POS_H ~sum` (8 bytes). With 2
   servos, do separate reads per ID (still no broadcast).
2. **Enable torque** — `SCSCL::EnableTorque(ID, 1)` writes 1 to
   the TORQUE_ENABLE register. Packet: `0xFF 0xFF ID 0x04 0x03
   0x28 0x01 ~sum`. Response: 6-byte status.
3. **Move to a target angle** — `SCSCL::WritePos(ID, pos, time,
   speed)` issues a synchronous write. Packet length depends on
   the parameters but is well under the 16-byte RX FIFO. Response
   is a 6-byte status.
4. **Add the A55 ↔ M33 wire layer** — RPMsg over OpenAMP, with
   the IPC contract in [`./IPC.md`](./IPC.md). At that point the
   A55 can issue `MSG_SET_TARGET` / `MSG_FIRE` / `MSG_MODE` and the
   M33 turns those into SCSCL commands.

Items (1)–(3) can all be done as small extensions to the
minimal-delta firmware here — hand-built SCSCL packets, no
library dependency, no GPIO. Item (4) is a separate subsystem
and gets its own bring-up.

---

## 7. Direction control (wheel mode) — VERIFIED 2026-07-13

The SC15 has two operating modes, controlled by the angle-limit
registers (MIN_ANGLE_LIMIT_L=9, MAX_ANGLE_LIMIT_L=11):

- **Position mode** (MIN ≠ 0, MAX ≠ 0): bounded rotation to a
  target position via `SCSCL::WritePos(id, pos, time, speed)`.
  Direction is determined by `target − current`.
- **Wheel mode** (MIN = MAX = 0): continuous rotation at a
  controlled speed. Used for the gun_bot's "spin the blaster" /
  "track a target" use cases.

### Speed encoding (wheel mode)

Speed commands go to **GOAL_TIME_L** (register 44) as **2 LE bytes**:

| Bytes (LE) | uint16 value | Direction |
|---|---|---|
| `[speed, 0x00]` | `speed` (0..1000) | **CW**  (one rotation direction) |
| `[speed, 0x04]` | `speed \| (1<<10)` | **CCW** (opposite direction) |
| `[0, 0]`         | `0`               | **stop** |

**Verified empirically** (`zephyr_v18.elf` test, id=2, speed=50, dur=100ms):
- `[0x32, 0x00]` → position went +508 (CW)
- `[0x32, 0x04]` → position went −508 (CCW)

This matches the `SCSCL::WritePWM` convention exactly:

```c
int SCSCL::WritePWM(u8 ID, s16 pwmOut)
{
    if (pwmOut < 0) {
        pwmOut = -pwmOut;            // take absolute value
        pwmOut |= (1<<10);            // set bit 10 as direction flag
    }
    u8 bBuf[2];
    Host2SCS(bBuf+0, bBuf+1, pwmOut);
    return genWrite(ID, SCSCL_GOAL_TIME_L, bBuf, 2);
}
```

### M33 implementation (gun_bot IPC)

Pass a signed `int16_t speed` from the A55; encode as the SCSCL
library does:

```c
static bool do_write_speed(uint8_t id, int16_t speed)
{
    uint16_t enc = (speed < 0)
        ? ((uint16_t)(-speed) | (1u << 10))   /* abs + bit10 = reverse */
        : (uint16_t)speed;                    /* positive = forward */
    uint8_t data[2] = {
        (uint8_t)(enc & 0xFFU),
        (uint8_t)((enc >> 8) & 0xFFU)
    };
    return do_write_n(id, 44, data, 2);  /* GOAL_TIME_L = 44 */
}
```

Caller passes:
- `do_write_speed(id, 0)` → stop
- `do_write_speed(id, +N)` → CW at speed N
- `do_write_speed(id, -N)` → CCW at speed N

### Important: do NOT send raw negative int16

Earlier sessions tried sending `-10` directly as `[0xF6, 0xFF]` and
saw no motion. **That is not the right encoding** — the SC15
firmware (v0.5) treats the high byte as a mode/magnitude word, not
as a sign extension. Always go through the abs + bit10 conversion.

### Pre-flight (wheel mode entry, once per servo)

```c
/* 1. Unlock EPROM */
do_write_1(id, 48, 0);                              /* LOCK = 0 */
/* 2. Write 4 zero bytes starting at MIN_ANGLE_LIMIT_L (reg 9) */
do_write_n(id, 9, (uint8_t[]){0,0,0,0}, 4);        /* MIN=MAX=0 */
/* 3. Lock EPROM */
do_write_1(id, 48, 1);                              /* LOCK = 1 */
```

After wheel mode is active, `do_write_speed()` rotates the servo
per the encoding above.

### Calibration guidance

Speed magnitude `N` is interpreted as **step/s** by the SC15 firmware
(per `SCS_Series_Memory_Table_Analysis.xls`, register 0x2E "Operation
speed", range 0..1000). At 1° = 1218 PRESENT_POSITION units (user-measured
2026-07-12), a speed of 10 step/s for 108 ms gives ~5° of motion
(5069..6912 units). This is the **only** empirically-verified safe
combination. See **§0 Safety Protocol** for hard caps and the
step-by-step motion discipline.

Empirically (calibrated 2026-07-12):
- speed=10, dur=108ms → 5069..6912 units (~4.2°..5.7°) ✓ safe
- speed=10, dur=200ms → 25600 units (~21°) ✓ safe (2× the calibrated dur)
- speed=50 for any duration > 100ms → **UNSAFE** (broke id=1+id=2 in
  the 2026-07-13 v19 test — see §0.7)

---

## 8. Cheatsheet

```bash
# Build + deploy scan firmware
cd gun_bot/m33_firmware/gun_controller
cmake --build --preset=debug
scp -o StrictHostKeyChecking=no \
  debug/zephyr/zephyr.elf \
  root@192.168.1.94:/lib/firmware/zephyr_scan.elf
ssh -o StrictHostKeyChecking=no root@192.168.1.94 \
  'echo stop > /sys/class/remoteproc/remoteproc0/state; sleep 2; \
   echo "/lib/firmware/zephyr_scan.elf" > /sys/class/remoteproc/remoteproc0/firmware; \
   echo start > /sys/class/remoteproc/remoteproc0/state; sleep 2; \
   cat /sys/class/remoteproc/remoteproc0/state'

# Watch console (12 s)
/usr/bin/python3 -c "
import serial, time
with serial.Serial('/dev/ttyACM1', 115200, timeout=0.5) as s:
    s.reset_input_buffer()
    t0 = time.monotonic()
    total = b''
    while time.monotonic() - t0 < 12.0:
        chunk = s.read(4096)
        if chunk: total += chunk
print(total.decode('utf-8', errors='replace'))
"

# Confirm wifi up (no SoC crash)
ping -c 5 -W 2 192.168.1.94
```

If wifi drops after deploy: physical unplug + replug the QinHeng
debug cable. Wait ~15 s for A55 to come back, then re-run `/a55-wifi`
to bring wifi back up.

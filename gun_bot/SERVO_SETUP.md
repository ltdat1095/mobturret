# Servo setup — FRDM-iMX93 M33 ↔ Waveshare Bus Servo Adapter (A) ↔ SC15 × 2

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

## 7. Cheatsheet

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

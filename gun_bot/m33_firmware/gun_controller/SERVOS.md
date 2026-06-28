# Servo Inventory — Bench Reference

Snapshot of what's currently connected to the SC15 servo bus on the
bench. Use this when you need to know which servo ID to write to, what
baud rate to configure, or how to re-discover servos after a power cycle.

## Current inventory

| Servo | ID  | Model  | Baud     | Notes |
|-------|-----|--------|----------|-------|
| #1    | 1   | 3845   | 1,000,000 | Factory default ID, kept as-is |
| #2    | 2   | 3845   | 1,000,000 | ID changed from 1 to 2 on 2026-06-26 (originally also id=1, factory default) |

Both are **SC15 / SCSCL protocol** servos (LewanSoul/LX-16 compatible).

## How the IDs were set

The second servo originally shipped with ID = 1 (factory default — same as
the other one). To distinguish them, the ID was rewritten using the
STServo_Python `change_id.py` workflow:

```python
# Reference: STServo_Python/stservo-env/scscl/change_id.py
pkt.unLockEprom(found_id)         # EPROM unlock required before ID write
pkt.write1ByteTxRx(found_id, 5, NEW_ID)   # 5 = SCSCL_ID register
pkt.LockEprom(NEW_ID)
```

The change was made via the **USB-to-serial bridge in USB mode**
(`/dev/ttyACM0` on the host). After the change, both servos were
re-verified via Python sweep.

## How to re-discover the servos

If you need to confirm baud / IDs after a power cycle, ID re-write, or
adding new servos, use the Python sweep on the bench's host machine:

```bash
# Confirm the USB bridge shows up
lsusb | grep -i qinheng            # → 1a86:55d3 (QinHeng CH340/55d3 CDC-ACM)
ls /dev/ttyACM*                     # → /dev/ttyACM0

# If not already accessible
sudo chmod 666 /dev/ttyACM0

# Sweep all common baud rates × IDs 0..255
python3 /tmp/sweep_v2.py             # or any of the /tmp/sweep*.py scripts

# Expected output (at 1M baud):
#   === baud 1000000 ===
#     FOUND id=1 model=3845
#     FOUND id=2 model=3845
```

If a servo stops responding to ping at 1M baud, try changing its baud via
the SCSCL `writeByte(ID, SCSCL_BAUD_RATE, 0)` register (value 0 = 1M,
1 = 500k, 2 = 250k, 3 = 128k, etc.).

## Why 1M baud (not 115200)

The SC15 factory-default baud is **1,000,000** (1 Mbaud). The earlier
M33 firmware used 115200 because that's the Zephyr devicetree `current-speed`
example value, and we'd assumed the SC15 would auto-negotiate. It does not —
each servo has a fixed baud set in its EPROM, and 115200 is **never** the
SC15 default. We confirmed both servos are at 1M via the Python sweep
before updating `boards/imx93_evk_mimx9352_m33.overlay`:

```dts
&lpuart3 {
    status = "okay";
    current-speed = <1000000>;     /* SC15 factory default */
    pinctrl-0 = <&uart3_default>;
    pinctrl-names = "default";
};
```

If you ever replace a servo or add new ones, the new ones will also default
to 1M — no reconfiguration needed.

## SCSCL ID and address ranges

| Range | Meaning |
|-------|---------|
| `0x00` | Reserved / unused (some servos treat as broadcast) |
| `0x01 .. 0xFC` | Valid unicast servo IDs |
| `0xFD` | Reserved by some firmware (status reply) |
| `0xFE` | Broadcast ping — every servo replies (SCServo_Linux convention) |
| `0xFF` | Reserved / broadcast (STServo_Python convention) |

The STServo_Python library rejects `scs_id >= 0xFF` in `ping()`, so to
broadcast from Python use `0xFE` (which the Python library treats as a
normal unicast). The SC15 firmware treats both `0xFE` and `0xFF` as
broadcast-equivalent and replies.

## SCSCL ping packet (reference)

```
Request (6 bytes):
  0xFF 0xFE  ID  Length  Fun  Checksum
  |   |     |    |       |    |
  |   |     |    |       |    +-- ~sum(ID + Length + Fun + MemAddr) & 0xFF
  |   |     |    |       +------- 0x01 (INST_PING)
  |   |     |    +-------------- 0x02 (no payload for PING)
  |   |     +------------------- 0xFE (broadcast) or specific ID
  |   +------------------------- 0xFE (header byte 2)
  +----------------------------- 0xFF (header byte 1)

Example: broadcast ping at 1M baud = FF FE FE 02 01 FE

Response (6 bytes):
  0xFF 0xFE  ID  0x02  Error  Checksum
                                Checksum = ~(ID + 0x02 + Error) & 0xFF
```

## Known issues with the bench setup

| Issue | Workaround |
|-------|------------|
| M33 LPUART3 single-wire mode hangs `uart_poll_out` | Use two-pin mode in overlay; short GPIO_14 ↔ GPIO_15 externally |
| LPUART2 (console) conflicts with Linux A55 debug console | Console output only visible on a separate debug cable (e.g., `/dev/ttyACM1`), not on Linux tty |
| Remoteproc firmware path doesn't auto-flush cache | Use a different filename each redeploy (`/lib/firmware/zephyr_vN.elf`) to bust the kernel's firmware cache |
| Adapter needs separate 5-8.4V power even if servos are powered separately | DC jack or terminal block on the adapter |
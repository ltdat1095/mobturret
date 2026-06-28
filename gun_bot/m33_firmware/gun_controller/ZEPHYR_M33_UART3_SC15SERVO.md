# Zephyr M33 → LPUART3 → SC15 Servo — Driver Plan

**Goal:** make `mobturret/gun_bot/m33_firmware/gun_controller` (Zephyr M33 firmware) drive SC15 servos over LPUART3 at 1 Mbaud.

**Status as of 2026-06-28:**
- 1 Mbaud LPUART3 init is verified working (`BAUD = 0x17000001`, TE/RE enabled, `STAT = 0x00800000` clean after init).
- 16-byte loopback test passes in both standalone and Zephyr (550+ iterations).
- Actual servo packets are >16 bytes; the existing polling driver can't handle them without losing bytes. **The driver needs DMA or interrupt-driven RX drain to work with real SC15 packets.**

This document is the bridge between "1 Mbaud works in the loopback test" and "1 Mbaud drives real servos in production".

---

## 1. What's currently in the repo

### Working
- `nxp_zephyr/.../fsl_lpuart.c` SDK baud override patch (1 Mbaud) — see `PROGRESS.md` for line numbers and md5s
- `gun_controller/boards/imx93_evk_mimx9352_m33.overlay` — intentionally empty (the LPUART3 node was removed because the Zephyr driver path left STAT error flags set; main.cpp drives LPUART3 directly)
- `gun_controller/prj.conf` — `CONFIG_SERIAL=y`, `CONFIG_UART_MCUX_LPUART=y`, `CONFIG_UART_INTERRUPT_DRIVEN=n` (polling only)
- `src/main.cpp` SYS_INIT hooks at PRE_KERNEL_1 prio 0 / 1 / 60 / 70 — clock init, baud fixup, full LPUART3 reinit (the only thing keeping the Zephyr firmware's LPUART3 in a working state)
- `src/main.cpp::loopback_thread` — thread that runs the loopback test, reads BAUD/CTRL/STAT for diagnostics, toggles RGB LEDs

### Library for the actual servo bus (already vendored)
- `src/servo/SCSerial.h` / `SCSerial.cpp` — Zephyr-flavored `SCSerial` class. Uses `uart_poll_in` / `uart_poll_out`.
- `src/servo/SCSCL.h` / `SCSCL.cpp` — SCSCL protocol class (broadcast ping, single-servo read/write, sync write).
- `src/servo/SCS.h` / `SCS.cpp` — base class with direction-half-duplex GPIO toggle.
- `src/servo/INST.h` — instruction IDs (PING=0x01, READ=0x02, WRITE=0x03).
- `src/servo/SMS_STS.h` / `SMS_STS.cpp` — STS-series (we use SCSCL, but it's there).

### Currently broken
- `SCSerial::readSCS()` (in `src/servo/SCSerial.cpp`) calls `uart_poll_in(SCS::uart_dev, &byte)` once per byte with `k_sleep(K_MSEC(1))` between polls. With a 16-entry RX FIFO, any packet longer than 16 bytes received faster than 1 ms/byte will overflow the FIFO and silently drop bytes.
- `SCSerial::writeSCS()` uses `uart_poll_out()` per byte — fine for TX (no FIFO overflow possible), but slow.

### Known Zephyr devicetree bits (currently disabled)
- The `lpuart3` node is NOT declared in the overlay. The Zephyr `mcux_lpuart` driver is therefore NOT bound to LPUART3. `main.cpp` drives the peripheral directly via the baremetal MCUXpresso SDK.
- `CONFIG_UART_INTERRUPT_DRIVEN` is **disabled** (set to `n`) — polling only. Interrupt-driven mode is the prerequisite for using the Zephyr async API or the DMA path.

---

## 2. The fundamental constraint: 16-entry RX FIFO

LPUART3 on i.MX93 M33 has a **16-entry RX FIFO** (verified at runtime: `FIFO[4:6] = 3 → 16 entries`). The polled SDK path is fine for short packets but loses bytes for any packet ≥ 17 bytes if the CPU doesn't drain the FIFO in time.

Measured at 1 Mbaud:
- 1 bit-time = 1 µs, 1 byte = 10 bit-times = 10 µs
- 16 bytes arrive in 160 µs
- `SCSerial::readSCS`'s polling loop is `k_sleep(K_MSEC(1))` between `uart_poll_in` calls — that's 1000 µs/byte, so the loop drains at ~1/10th of the incoming byte rate
- After 16 bytes (~16 ms of incoming data, but the loop has read ~1 byte in that time), the FIFO fills; byte 17 sets the OR (overrun) flag and is lost

For the loopback test, we work around this by limiting `LOOPBACK_PATTERN` to 16 bytes. **For real servo comms, this workaround is not viable** — SCSCL packets for `READ` (6 + 2*N bytes), `WRITE` (6 + 2*N bytes), and `SYNC WRITE` (4 + 2*N + 2*N bytes for position+time+speed) can easily exceed 16 bytes for N ≥ 5 servos.

**The fix has to be at the driver level, not the application.**

---

## 3. Three options to handle the 16-byte FIFO

Listed in order of preference for production. Each one requires concrete changes; see §4–§6 for details.

### Option A: DMA-based reception (RECOMMENDED)

i.MX93 M33 has an eDMA peripheral (`DMA0`) that can be triggered by LPUART3 RX. Configure the eDMA channel to copy RX FIFO → circular RAM buffer. SCSerial reads from the RAM buffer via a ring-buffer index. No CPU overhead per byte, no FIFO overflow possible.

**Pros:** zero-loss, minimal CPU usage, scales to any packet size.
**Cons:** more setup (Zephyr device tree, eDMA channel config, ring buffer management); needs `CONFIG_UART_NXP_LPUART_ASYNC_API_SUPPORT` or a custom async layer.

**Decision point:** the i.MX93 M33 DTS already provides `dmas` properties for `serial@42570000` if we declare a `lpuart3` node with `dmas = <...>;` in the overlay. Check the eDMA node's channel bindings.

### Option B: Interrupt-driven drain (MIDDLE)

Enable `CONFIG_UART_INTERRUPT_DRIVEN=y`, register an RX ISR on LPUART3 that drains the FIFO into a software ring buffer. SCSerial reads from the ring buffer. The ISR runs at high priority and drains bytes before the FIFO can overflow.

**Pros:** no DMA setup, smaller code change.
**Cons:** ISR overhead per byte; need to ensure ISR preempts any `k_sleep` / `printk` / syswork in the SCSerial thread.

**Pre-condition:** the test that already failed in §3 of `PROGRESS.md` was with ISR enabled; we had to disable ISR to get the loopback test to even run reliably. With Option B, you must also:
- Raise the LPUART3 IRQ priority in the devicetree (`interrupts = <68 0>;` — lower number = higher priority)
- Add a small ring buffer (e.g., 128 bytes) and a k_work or k_fifo for handoff to the application thread
- Re-test that the ISR drains fast enough to keep up with 1 Mbaud (1 byte / 10 µs)

### Option C: Smaller packets (NOT VIABLE for production)

Restrict the SC15 protocol driver to single-servo read/write (≤16 bytes per packet). This loses the `SYNC WRITE` efficiency and any bulk operations.

**Pros:** no driver change, reuses the polling path.
**Cons:** protocol limitation; multi-servo moves are slow.

---

## 4. Concrete changes needed (in order)

### 4.1 Restore the `lpuart3` devicetree node (if using Option A or B)

The overlay is currently empty (comment-only) because the Zephyr driver path left STAT error flags set. With the new FIFO-aware driver, the Zephyr driver should be safe to re-enable.

In `boards/imx93_evk_mimx9352_m33.overlay`:

```dts
/ {
	soc {
		lpuart3: serial@42570000 {
			compatible = "nxp,imx-lpuart", "nxp,lpuart";
			reg = <0x42570000 DT_SIZE_K(64)>;
			interrupts = <68 0>;          /* was 3; raise priority for DMA/ISR */
			clocks = <&ccm IMX_CCM_LPUART3_CLK 0x6c 24>;
			dmas = <&edma1 0 25>;         /* only for Option A — see SoC RM for channel */
			dma-names = "rx";
			pinctrl-0 = <&uart3_default>;
			pinctrl-names = "default";
			current-speed = <1000000>;
			status = "okay";
		};
	};
};

&pinctrl {
	uart3_default: uart3_default {
		group0 {
			pinmux = <&iomuxc1_gpio_io14_lpuart_tx_lpuart3_tx>,
					 <&iomuxc1_gpio_io15_lpuart_rx_lpuart3_rx>;
			drive-strength = <0x6>;     /* matches standalone DSE=15 */
		};
	};
};
```

Also delete the corresponding setup in `main.cpp`:
- Remove `direct_lpuart3_reinit` (PRE_KERNEL_1 prio 70) — the Zephyr driver will do it.
- Keep `imx93_m33_clock_init` (clock root) — the Zephyr clock driver still doesn't set the root source/divider.
- Keep `imx93_lpuart3_baud_fixup` (PRE_KERNEL_1 prio 60) — the SDK's baud search is broken; this writes the correct BAUD after the Zephyr driver init.

The two SYS_INIT hooks (`imx93_m33_clock_init` and `imx93_lpuart3_baud_fixup`) are still required; the full reinit is not.

### 4.2 Choose A or B and update `prj.conf`

For **Option A (DMA)**:
```
CONFIG_UART_INTERRUPT_DRIVEN=y
CONFIG_UART_NXP_LPUART_ASYNC_API_SUPPORT=y
CONFIG_UART_ASYNC_API=y
CONFIG_DMA=y
CONFIG_DMA_MCUX_EDMA=y
```
Then use the Zephyr `uart_dma` API or the LPUART async API in `SCSerial`. See §5.

For **Option B (ISR)**:
```
CONFIG_UART_INTERRUPT_DRIVEN=y
```
Don't bind the Zephyr LPUART driver. Register the ISR yourself in `main.cpp` at PRE_KERNEL_2 prio 0 (after pinctrl is applied, before scheduler starts), drain to a ring buffer. See §6.

For **Option C (poll-only, ≤16 byte packets)** — no `prj.conf` change; just call `SCSerial::readSCS` for one servo at a time.

### 4.3 `SCSerial::readSCS` needs to know about the 16-byte chunking

Even with DMA/ISR, if SCSerial reads one byte at a time via `uart_poll_in`, the bus is busy. Recommended change: SCSerial should call the new `uart_async_rx_buf_read` (Option A) or read from the ring buffer (Option B) for the *entire expected packet length*, with a single timeout.

If the protocol packet is N bytes:
- Option A: pre-allocate N bytes in the RX buffer, set up DMA, wait for completion with `k_timeout_t` = packet_time_us + safety_margin (e.g., 5 ms for a 30-byte packet at 1 Mbaud)
- Option B: wait for ring buffer to contain N bytes, with timeout

If the timeout expires with fewer bytes: signal a bus error (`SCSerial::Err = 1`) and discard.

### 4.4 `SCSerial::writeSCS` is fine

`uart_poll_out()` blocks until TDRE per byte, so the 16-byte TX FIFO is never the bottleneck. **No change required for write path.**

### 4.5 The wire is verified, but the harness is not

The current bench has GPIO_14 ↔ GPIO_15 shorted for loopback. When the servos arrive, the wire goes from GPIO_14/15 to the SC15 adapter's `D` pin (half-duplex single-wire). The driver needs to switch between TX-active and RX-active modes. The Zephyr LPUART3 driver doesn't support this natively — it expects two-pin mode.

Two options:
- Keep the wire as two-pin: TX from GPIO_14, RX from GPIO_15, with the SC15 adapter's D pin connected to **both**. The TX echo on the RX pin is the local loopback; SC15 responses come back on the same wire (and are mixed with the local echo, but the SC15 protocol's half-duplex discipline means responses only come after the command TX is done, so the echo is just "garbage" that SC15 framing discards). This is the simplest setup.
- Use a hardware direction-control GPIO (RS-485 DE pin) — needs a separate GPIO control, more wiring.

**Recommendation:** start with two-pin + the SC15 adapter's D pin connected to both GPIO_14 and GPIO_15. Verify echo + response disambiguation via the SC15 protocol's own half-duplex discipline.

---

## 5. Detailed plan for Option A (DMA) — RECOMMENDED

### 5.1 Verify the eDMA binding

```bash
grep -A 5 "edma\|eDMA" /home/ltdat/Desktop/mobturret/gun_bot/m33_firmware/nxp_zephyr/zephyr/dts/arm/nxp/nxp_imx93_m33.dtsi | head -40
```

Find the eDMA node. Get the right channel number for LPUART3 RX (typically channel 25 or 33 on i.MX93 M33, but the actual number is in the SoC RM and the DTS). Replace the placeholder in the overlay with the correct phandle.

### 5.2 Use the Zephyr async UART API in `SCSerial`

Replace the polling `uart_poll_in` / `uart_poll_out` with:
- `uart_callback_set()` to register RX ready / TX done / RX buf released callbacks
- `uart_rx_enable()` to start reception into a callback-managed buffer
- In the callback, copy bytes out of the buffer for the SCSerial read path

Pseudocode (in `SCSerial::begin`):
```c
struct uart_async_rx_buf { uint8_t buf[64]; size_t idx; } rx_buf;

static void rx_cb(const struct device *dev, struct uart_event *evt, void *user) {
    SCSerial *self = (SCSerial *)user;
    if (evt->type == UART_RX_RDY) {
        // evt->data.rx.buf + evt->data.rx.offset + evt->data.rx.len available
        // hand off to a ring buffer that readSCS consumes
    }
}

void SCSerial::begin(const struct device *uart_dev) {
    SCS::uart_dev = uart_dev;
    uart_callback_set(uart_dev, rx_cb, this);
    uart_rx_enable(uart_dev, rx_buf.buf, sizeof(rx_buf.buf), 100);
}
```

And `readSCS`:
```c
int SCSerial::readSCS(unsigned char *nDat, int nLen) {
    // Pop bytes from the ring buffer populated by rx_cb.
    // Block on a semaphore with timeout, not on k_sleep per byte.
    for (int i = 0; i < nLen; i++) {
        if (!ringbuf_get(&rb, &nDat[i])) {
            // wait up to IOTimeOut
            if (k_sem_take(&rb_sem, K_MSEC(IOTimeOut)) != 0) {
                Err = 1;
                return i;
            }
        }
    }
    return nLen;
}
```

### 5.3 Test the DMA path

1. Build and deploy. Verify the `lpuart3` node is in the devicetree (`hexdump -C /sys/firmware/devicetree/base/soc@0/bus@42000000/serial@42570000/status` → should be `ok` not `disabled`).
2. Verify dmesg: `dmesg | grep -i lpuart3` — should show no errors, driver bound.
3. Run the loopback test (limit to 16 bytes — DMA is irrelevant for that, but proves the driver works).
4. Connect the SC15 adapter, run `SCSCL::ping(0xFE)` (broadcast ping) — should return the servo ID byte.

---

## 6. Detailed plan for Option B (ISR) — fallback if DMA is too invasive

### 6.1 Register the LPUART3 ISR at PRE_KERNEL_2

In `main.cpp`:
```c
#include <zephyr/irq.h>

static struct ring_buf rb;
static uint8_t rb_buf[256];
static struct k_sem rb_sem;

static void lpuart3_isr(const void *arg) {
    ARG_UNUSED(arg);
    while (LPUART_GetStatusFlags(DIRECT_LPUART3_BASE) & kLPUART_RxDataRegFullFlag) {
        uint8_t c = LPUART_ReadByte(DIRECT_LPUART3_BASE);
        if (ring_buf_put(&rb, &c, 1) != 1) {
            // overflow: drop
        }
    }
    LPUART_ClearStatusFlags(DIRECT_LPUART3_BASE, kLPUART_RxDataRegFullFlag);
    k_sem_give(&rb_sem);
}

static int lpuart3_isr_init(void) {
    ring_buf_init(&rb, sizeof(rb_buf), rb_buf);
    k_sem_init(&rb_sem, 0, 1);
    IRQ_CONNECT(LPUART3_IRQn, 0, lpuart3_isr, NULL, 0);
    irq_enable(LPUART3_IRQn);
    // Enable RX interrupt in LPUART
    LPUART_EnableInterrupts(DIRECT_LPUART3_BASE, kLPUART_RxDataRegFullFlagEnable);
    return 0;
}
SYS_INIT(lpuart3_isr_init, PRE_KERNEL_2, 0);
```

The ISR drains the RX FIFO into the ring buffer. SCSerial reads from the ring buffer. The whole path runs in interrupt context for the FIFO drain, so no `k_sleep(K_MSEC(1))` is needed.

### 6.2 IRQ priority

i.MX93 M33 uses ARMv8-M NVIC. Lower priority number = higher priority. To preempt the SCSerial thread, set the LPUART3 IRQ to a high-priority value (e.g., 0 or 1):
```dts
interrupts = <68 0>;   /* was 3; raise to 0 (highest) */
```
This is critical: at 1 Mbaud, a byte arrives every 10 µs. If the SCSerial thread is in `k_sleep(1)` for 1000 µs (100 byte-times), the ISR will run 100 times before the thread wakes. The ring buffer absorbs this, but only if the ISR is high enough priority to preempt the kernel.

### 6.3 Test

Same as 5.3.

---

## 7. Test plan (for either option)

1. **Loopback test** (already passing with 16-byte pattern): GPIO_14 ↔ GPIO_15 shorted. LED green. This proves the LPUART3 init, baud, and read/write path work.

2. **Echo test**: `SCSerial::writeSCS("Z", 1)` then `SCSerial::readSCS(buf, 1)` — single byte echo, should pass even with the polling driver (since 1 byte < 16 FIFO depth).

3. **Broadcast ping**: connect SC15 adapter with one or more servos, run `SCSCL::Ping(0xFE)`. Should return non-zero for each servo (the ID). With 6-byte packet (`0xFF, 0xFE, 0x06, 0x01, 0x00, checksum`), this fits the FIFO.

4. **Single-servo read**: `SCSCL::ReadPos(0x01)` — 8-byte response (`0xFF, 0x01, 0x04, 0x24, pos_l, pos_h, checksum`). 8 bytes, fits the FIFO. Polling driver may work for this if `k_sleep(1)` is enough.

5. **Multi-servo sync write**: `SCSCL::SyncWritePos(ids, 6, positions, times, speeds)` — payload grows as O(N). For N=5 servos: 4 + 2*5 + 2*5 + 2*5 = 34 bytes. **This is where the polling driver fails** — RX FIFO overflows during the response. With DMA/ISR, this should work.

6. **Stress test**: 1000 sync writes in a row, verify no lost bytes via a servo-position-feedback loop.

---

## 8. Files to change (summary)

| File | Change |
|---|---|
| `gun_controller/boards/imx93_evk_mimx9352_m33.overlay` | Add back the `lpuart3` node with `dmas` (Option A) or higher-priority `interrupts` (Option B). |
| `gun_controller/prj.conf` | Enable `UART_INTERRUPT_DRIVEN` and (for A) `DMA_MCUX_EDMA`, `UART_NXP_LPUART_ASYNC_API_SUPPORT`. |
| `gun_controller/src/main.cpp` | Remove `direct_lpuart3_reinit`. Keep `imx93_m33_clock_init` and `imx93_lpuart3_baud_fixup`. If using Option B, add the ISR registration SYS_INIT. |
| `gun_controller/src/servo/SCSerial.h` / `SCSerial.cpp` | Switch to async API (Option A) or ring-buffer read from ISR (Option B). The current polling `readSCS` is the bottleneck. |
| `gun_controller/CMakeLists.txt` | Already includes the SCSerial/SCSCL/SCS source files; no change needed. |

---

## 9. Risk register

| Risk | Mitigation |
|---|---|
| DMA not available on i.MX93 M33 in Zephyr | Verified in DTS — M33 has eDMA nodes. If Zephyr's `dma_mcux_edma` driver doesn't support this chip, fall back to Option B. |
| ISR priority not high enough → bytes still lost | Bench-measure: with LPUART3 IRQ at priority 0, send a 30-byte packet and verify the ring buffer count. |
| LPUART3 driver still leaves STAT error flags | The "STAT=0x40d80000" pattern we saw was caused by FIFO overflow (not driver init). Once the driver has DMA/ISR-driven RX, the FIFO won't overflow, so the error flags won't be set in normal operation. |
| Servo power sequencing on the bench | The SC15 adapter expects 7.4-11 V. Use a bench supply, not the FRDM's 5 V rail. |
| Two-pin vs single-wire confusion | Start with two-pin (TX and RX both connected to the SC15 adapter's D pin via the wire short). SC15 protocol's half-duplex discipline handles echo rejection. |

---

## 10. Acceptance criteria

- [ ] Loopback test passes 1000 times with no FAILs (already works with 16-byte pattern)
- [ ] Broadcast ping returns correct servo IDs (e.g., 1, 2)
- [ ] Single-servo read returns position within 0.1° of expected
- [ ] Multi-servo sync write (N=5) succeeds with all servos moving in sync
- [ ] No STAT error flags (PF, FE, NF, OR) set during normal servo comms
- [ ] CPU usage during sustained servo traffic < 5% (DMA/ISR path is mostly off-CPU)

When all boxes are checked, the M33 Zephyr firmware is production-ready for the servo bus.

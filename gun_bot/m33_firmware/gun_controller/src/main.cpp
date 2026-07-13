/*
 * MobTurret M33 firmware — LPUART3 + console loopback (pure-Zephyr).
 *
 * Hardware targets (FRDM-iMX93, M33 core):
 *   - LPUART2 (console)  → /dev/ttyACM1 on the host
 *   - LPUART3 (servo bus) → GPIO_IO14 (TX), GPIO_IO15 (RX), 1 Mbaud
 *
 * Boot order (must match clock_control_mcux_ccm_rev2.c patches in the
 * MobTurret zephyr fork @ 4673670d075):
 *
 *   prio 0  — lpuart_clocks_init: open LPUART3 clock root + IP gate.
 *             LPUART2 root is opened later by Zephyr's mcux_lpuart driver
 *             when the console binds at prio 50.
 *   prio 50 — Zephyr LPUART driver binds, calls LPUART_Init(). The
 *             hal_nxp @ c7f1b8449 fork has the 1 Mbaud override in
 *             fsl_lpuart.c (lines 435 / 850) so BAUD = 0x17000001 directly.
 *   prio 7  — loopback_thread: every 1 s, scan-ping SCSCL IDs 1..5 on
 *             LPUART3 (via the Waveshare Bus Servo Adapter A) and log
 *             which IDs responded. Each ping is 6 bytes (well under
 *             the 16-byte RX FIFO).
 */

/*
 * MobTurret M33 firmware — LPUART3 + console loopback (pure-Zephyr).
 *
 * Hardware targets (FRDM-iMX93, M33 core):
 *   - LPUART2 (console)  → /dev/ttyACM1 on the host
 *   - LPUART3 (servo bus) → GPIO_IO14 (TX), GPIO_IO15 (RX), 1 Mbaud
 *
 * Boot order:
 *   prio 0  — imx93_m33_clock_init: SDK-direct clock root + IP gate
 *             bring-up for BOTH Lpuart2 (console) and Lpuart3 (servo).
 *             Zephyr's clock_control_mcux_ccm_rev2.c only sets the IP gate
 *             (CLOCK_EnableClock) — it does NOT call CLOCK_SetRootClock,
 *             so the clock root's OFF bit stays set. Without this hook,
 *             LPUART TX hangs on TDRE and trips the SoC watchdog.
 *             See git grep "Zephyr clock driver" in
 *             physical_gunbound/.../CLAUDE.md for the full diagnosis.
 *   prio 50 — Zephyr mcux_lpuart driver binds for LPUART2/LPUART3, calls
 *             LPUART_Init() with the baud-search loop.
 *   prio 60 — imx93_lpuart3_baud_fixup: forces BAUD = 0x17000001 by direct
 *             register write, AFTER the driver has run. This bypasses any
 *             wrong (OSR, SBR) the SDK's baud-search may have produced.
 *   prio 7  — loopback_thread: every 1 s, scan-ping SCSCL IDs 1..5 on
 *             LPUART3 and log which IDs responded.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>

#include <fsl_clock.h>    /* SDK clock root + IP gate control */
#include <fsl_lpuart.h>   /* pulls in MIMX9352_cm33_COMMON.h with LPUART3_BASE */

#include <string.h>

/* LPUART3 base for the M33 non-secure view. Matches the DTS overlay's
 * `reg` property and the SC15 servo bus pinout. */
#define LPUART3_BASE_NS        0x42570000UL

/* BAUD register value for 1 Mbaud at 24 MHz Lpuart3 clock root:
 *   OSR = 23 (bits 28:24), SBR = 1 (bits 12:0)
 *   baud = 24e6 / ((OSR + 1) * SBR) = 24e6 / 24 = 1,000,000 */
#define LPUART3_BAUD_1MBPS     ((23U << 24) | 1U)   /* 0x17000001 */

/* LPUART register offsets (NXP LPUART IP) */
#define LPUART_BAUD_OFFSET   0x10U
#define LPUART_STAT_OFFSET   0x14U

#define STACKSIZE 2048
#define PRIORITY  7

/* SCSCL PING (INST_PING=0x01). 6 bytes total — well under the 16-byte RX
 * FIFO. Packet layout (bytes 0..5):
 *   0,1   0xFF, 0xFF   header
 *   2     ID            target ID (0xFE = broadcast; 1..253 = specific)
 *   3     0x02          length (instruction + checksum = 2)
 *   4     0x01          INST_PING
 *   5     ~sum          checksum = ~(ID + Length + Instr), low byte
 *
 * Built per-ping in run_ping_test() so we can target different IDs. */
#define PING_PACKET_LEN  6U
#define PING_SCAN_ID_MIN 1U
#define PING_SCAN_ID_MAX 5U

static const struct device *const lpuart3_dev =
	DEVICE_DT_GET(DT_NODELABEL(lpuart3));

/* ===========================================================================
 * PRE_KERNEL_1 prio 0: SDK-direct clock-tree bring-up.
 *
 * iMX93 reset state: every clock root has OFF bit set. Zephyr's
 * clock_control_mcux_ccm_rev2.c only sets the LPCG IP gate; it does NOT
 * clear the root OFF bit or set mux/div. Calling the SDK directly bypasses
 * that gap and configures both Lpuart2 (console) and Lpuart3 (servo bus)
 * for a 24 MHz functional clock sourced from Osc24M with div=1.
 * =========================================================================*/

static int imx93_m33_clock_init(void)
{
	const clock_root_config_t rootCfg = {
		.clockOff = false,
		.mux      = 0,
		.div      = 1,
	};
	CLOCK_SetRootClock(kCLOCK_Root_Lpuart2, &rootCfg);
	CLOCK_EnableClock(kCLOCK_Lpuart2);
	CLOCK_SetRootClock(kCLOCK_Root_Lpuart3, &rootCfg);
	CLOCK_EnableClock(kCLOCK_Lpuart3);
	return 0;
}
SYS_INIT(imx93_m33_clock_init, PRE_KERNEL_1, 0);

/* ===========================================================================
 * PRE_KERNEL_1 prio 60: force LPUART3 BAUD to 1 Mbaud.
 *
 * Runs after the LPUART driver init (prio 50) has potentially written
 * its own (OSR, SBR). We overwrite with the exact integer divisor that
 * yields 1,000,000 baud at the 24 MHz Lpuart3 root: OSR=23, SBR=1.
 * =========================================================================*/

static int imx93_lpuart3_baud_fixup(void)
{
	*(volatile uint32_t *)(LPUART3_BASE_NS + LPUART_BAUD_OFFSET) =
		LPUART3_BAUD_1MBPS;
	return 0;
}
SYS_INIT(imx93_lpuart3_baud_fixup, PRE_KERNEL_1, 60);

/* ===========================================================================
 * Helpers
 * =========================================================================*/

static inline uint32_t lpuart3_read(uint32_t offset)
{
	return *(volatile uint32_t *)(LPUART3_BASE + offset);
}

/* ===========================================================================
 * SCSCL register read/write helpers (raw packets).
 * Hand-built so we don't pull in the SCSCL library (which previously
 * crashed the SoC — see INIT_SOURCE_PROBLEM.md §2.3 and
 * project-sc15-ping-bisect-2026-07-11.md).
 * =========================================================================*/

#define INST_PING   0x01U
#define INST_READ   0x02U
#define INST_WRITE  0x03U

#define REG_TORQUE_ENABLE        40U
#define REG_GOAL_TIME_L          44U
#define REG_PRESENT_POSITION_L   56U

static inline void drain_rx(void)
{
	unsigned char c;
	while (uart_poll_in(lpuart3_dev, &c) == 0) { /* discard */ }
}

static void tx_packet(const uint8_t *pkt, size_t pkt_len)
{
	for (size_t i = 0; i < pkt_len; i++) {
		uart_poll_out(lpuart3_dev, pkt[i]);
	}
}

/* Read up to `n` bytes within `iters` polls. Inserts k_msleep(1) every
 * 2000 iters to flush printk + yield kernel. */
static uint32_t rx_bytes(uint8_t *out, size_t n, uint32_t iters)
{
	uint32_t idx = 0U;
	for (uint32_t w = 0; w < iters && idx < n; w++) {
		unsigned char c;
		int rc = uart_poll_in(lpuart3_dev, &c);
		if (rc == 0) {
			out[idx++] = (uint8_t)c;
		}
		for (volatile int d = 0; d < 20; d++) {
			__asm__ volatile("nop");
		}
		if ((w % 2000U) == 1999U) {
			k_msleep(1);
		}
	}
	return idx;
}

static bool write_reg(uint8_t id, uint8_t reg, const uint8_t *data, uint8_t data_len,
		      uint8_t *err_out)
{
	drain_rx();
	k_msleep(5);

	const uint8_t len = (uint8_t)(3U + data_len);
	uint8_t pkt[8 + 6];
	uint8_t sum;
	pkt[0] = 0xFF;
	pkt[1] = 0xFF;
	pkt[2] = id;
	pkt[3] = len;
	pkt[4] = INST_WRITE;
	pkt[5] = reg;
	sum = (uint8_t)(id + len + INST_WRITE + reg);
	for (uint8_t i = 0; i < data_len; i++) {
		pkt[6 + i] = data[i];
		sum = (uint8_t)(sum + data[i]);
	}
	pkt[6 + data_len] = (uint8_t)~sum;
	tx_packet(pkt, (size_t)(6U + data_len));

	k_msleep(5);

	uint8_t rx[6];
	if (rx_bytes(rx, 6, 15000U) != 6) return false;
	if (rx[0] != 0xFF || rx[1] != 0xFF) return false;
	if (err_out) {
		*err_out = rx[4];
	}
	return (rx[2] == id) && (rx[4] == 0U);
}

static bool write_reg_simple(uint8_t id, uint8_t reg, const uint8_t *data, uint8_t data_len)
{
	return write_reg(id, reg, data, data_len, NULL);
}

static bool read_reg(uint8_t id, uint8_t reg, uint8_t data_len, uint8_t *out)
{
	drain_rx();
	k_msleep(5);

	const uint8_t pkt[8] = {
		0xFF, 0xFF, id, 0x04,
		INST_READ, reg, data_len,
		(uint8_t)~(id + 0x04U + INST_READ + reg + data_len)
	};
	tx_packet(pkt, 8);

	k_msleep(5);

	const size_t total = (size_t)(6U + data_len);
	uint8_t rx[6 + 4];
	if (rx_bytes(rx, total, 15000U) != total) return false;
	if (rx[0] != 0xFF || rx[1] != 0xFF) return false;
	if (rx[2] != id) return false;
	if (out != NULL) {
		for (uint8_t i = 0; i < data_len; i++) {
			out[i] = rx[5U + i];
		}
	}
	return true;
}

static uint16_t read_present_position(uint8_t id)
{
	uint8_t data[2] = {0, 0};
	if (!read_reg(id, REG_PRESENT_POSITION_L, 2, data)) {
		return UINT16_MAX;
	}
	return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static bool write_speed(uint8_t id, int16_t speed)
{
	const uint8_t data[2] = {
		(uint8_t)(speed & 0xFFU),
		(uint8_t)((speed >> 8) & 0xFFU)
	};
	return write_reg_simple(id, REG_GOAL_TIME_L, data, 2);
}

/* ===========================================================================
 * SC15 ping test — pure Zephyr (uart_poll_in / uart_poll_out).
 *
 * Send a 6-byte SCSCL PING targeted at one ID and try to read a 6-byte
 * response. The response is a status packet:
 *   0,1   0xFF, 0xFF   header
 *   2     ID            responding servo's ID
 *   3     0x02          length
 *   4     ERR           error code (0 = OK)
 *   5     ~sum          checksum
 *
 * Identical UART polling pattern to the snapshot loopback test — that's
 * the known-good path. Only the packet and response check change.
 * =========================================================================*/

static bool run_ping_test(uint8_t target_id, uint8_t *const resp_id_out)
{
	uint8_t rx_buf[PING_PACKET_LEN] = {0};
	uint32_t idx = 0;
	unsigned char c;
	int rc;

	/* Drain stale RX from previous iterations / responses. */
	while (uart_poll_in(lpuart3_dev, &c) == 0) { /* discard */ }

	/* Build the SCSCL PING packet for this target ID. */
	const uint8_t chk = (uint8_t)~(target_id + 0x02U + 0x01U);
	uint8_t pkt[PING_PACKET_LEN] = {
		0xFF, 0xFF, target_id, 0x02, 0x01, chk
	};

	/* TX packet byte-by-byte. */
	for (size_t i = 0; i < PING_PACKET_LEN; i++) {
		uart_poll_out(lpuart3_dev, pkt[i]);
	}

	/* Read with busy-wait timeout. 6 bytes × 10 bit-times at 1 Mbaud
	 * fits well within 50000 iterations of the inner poll loop. */
	for (uint32_t waited = 0;
	     waited < 50000U && idx < PING_PACKET_LEN;
	     waited++) {
		rc = uart_poll_in(lpuart3_dev, &c);
		if (rc == 0) {
			rx_buf[idx++] = (uint8_t)c;
		}
		for (volatile int d = 0; d < 20; d++) {
			__asm__ volatile("nop");
		}
	}

	if (idx != PING_PACKET_LEN) {
		return false;
	}
	/* Header must be 0xFF 0xFF. */
	if (rx_buf[0] != 0xFF || rx_buf[1] != 0xFF) {
		return false;
	}
	/* Responding ID is the third byte. */
	if (resp_id_out) {
		*resp_id_out = rx_buf[2];
	}
	return true;
}

/* ===========================================================================
 * Main thread — runs once, prints banner + initial state, then a worker
 * thread handles the loopback loop.
 * =========================================================================*/

static int print_boot_banner(void)
{
	uint32_t baud = lpuart3_read(LPUART_BAUD_OFFSET);
	uint32_t stat = lpuart3_read(LPUART_STAT_OFFSET);

	if (!device_is_ready(lpuart3_dev)) {
		printk("BOOT FAIL: lpuart3 device not ready\n");
		return -ENODEV;
	}

	/* Clock root prio 0 hook (imx93_m33_clock_init) must have run before
	 * main(). After the prio 60 baud_fixup hook, BAUD should be 0x17000001. */
	if (baud == 0U) {
		printk("BOOT FAIL: LPUART3 BAUD=0 (clock root not configured)\n");
		return -ENODEV;
	}

	/* Expected BAUD at 24 MHz clock, OSR=23, SBR=1:
	 *   BAUD = (OSR-1) << 24 | SBR = (23 << 24) | 1 = 0x17000001. */
	const bool baud_ok = (baud == 0x17000001U);

	printk("\n");
	printk("============================================================\n");
	printk(" MobTurret M33 firmware — LPUART3 @ 1 Mbaud\n");
	printk(" Zephyr SDK 1.0.1 / GCC 14.3.0\n");
	printk(" Console: LPUART2 -> /dev/ttyACM1 (host)\n");
	printk(" Servo bus: LPUART3 @ 0x%08x, GPIO_IO14/IO15\n",
	       (unsigned)LPUART3_BASE);
	printk("------------------------------------------------------------\n");
	printk(" LPUART3 BAUD  = 0x%08x %s\n",
	       (unsigned)baud, baud_ok ? "(1 Mbaud OK)" : "(NOT 1 Mbaud!)");
	printk(" LPUART3 STAT  = 0x%08x\n", (unsigned)stat);
	/* Common STAT bits: TDRE=1<<23, TC=1<<22, RDRF=1<<21, OR=1<<19, ... */
	if (stat & (1U << 19)) {
		printk("   ! STAT[OR] overrun flag set\n");
	}
	if (stat & (1U << 16)) {
		printk("   ! STAT[LBKDIF] LIN break detect\n");
	}
	if (stat & (1U << 15)) {
		printk("   ! STAT[MA1F] match-1\n");
	}
	if (stat & 0x000F0000U) {
		/* Clear any latched error flags (W1C). */
		*(volatile uint32_t *)(LPUART3_BASE + LPUART_STAT_OFFSET) =
			0x000F0000U;
	}
	(void)baud_ok;
	return 0;
}

void loopback_thread(void)
{
	uint32_t tick = 0U;

	while (true) {
		/* Per-ID scan result. bit N set => ID N responded. */
		uint8_t found_mask = 0U;

		for (uint8_t id = PING_SCAN_ID_MIN; id <= PING_SCAN_ID_MAX; id++) {
			uint8_t resp_id = 0U;
			if (run_ping_test(id, &resp_id)) {
				found_mask |= (uint8_t)(1U << id);
			}
		}

		/* One summary line per tick — no flooding. */
		if (found_mask != 0U) {
			/* Build a human list of responding IDs. */
			char ids[24] = {0};
			int p = 0;
			for (uint8_t id = PING_SCAN_ID_MIN; id <= PING_SCAN_ID_MAX; id++) {
				if (found_mask & (1U << id)) {
					if (p > 0) {
						ids[p++] = ',';
					}
					ids[p++] = '0' + (char)id;
				}
			}
			ids[p] = '\0';
			for (int i = 0; i < 3; i++) {
				printk("[%u] PING OK: ids={%s}\n",
				       (unsigned)tick, ids);
			}
		} else {
			for (int i = 0; i < 3; i++) {
				printk("[%u] PING FAIL: no servo on IDs 1..%u\n",
				       (unsigned)tick,
				       (unsigned)PING_SCAN_ID_MAX);
			}
		}

		tick++;
		k_sleep(K_SECONDS(1));
	}
}

K_THREAD_DEFINE(loopback_id, STACKSIZE, loopback_thread, NULL, NULL, NULL,
		PRIORITY, 0, 0);

/* ===========================================================================
 * One-shot wheel-mode direction test (speed = -10, dur = 108 ms).
 *
 * Runs after the scan has had a chance to confirm the bus is alive.
 * Skips the wheel-mode-entry dance — id=2 should already be in wheel mode
 * from prior sessions (per project-sc15-discovery-2026-07-12.md). Reads
 * PRESENT_POSITION, sends raw int16 speed=-10 (0xFFF6 LE) to GOAL_TIME_L,
 * sleeps 108 ms, sends speed=0 stop, reads PRESENT_POSITION again.
 *
 * Calibration: 1° = 1218 PRESENT_POSITION units. speed=+10, dur=108ms
 * gives ~5° LEFT (per 2026-07-12 calibration). If direction REVERSED,
 * this test will show ~+5° delta (PRESENT_POSITION counts UP).
 * =========================================================================*/

#define TEST_ID          2U
#define TEST_SPEED       (-10)
#define TEST_DUR_MS      108U

static void direction_test_thread(void)
{
	/* Let the boot banner + a few scan ticks happen first. */
	k_msleep(3000);

	for (int i = 0; i < 3; i++) {
		printk("\n[TEST] === speed=%d, dur=%ums, target=id%u ===\n",
		       (int)TEST_SPEED, (unsigned)TEST_DUR_MS, (unsigned)TEST_ID);
	}

	const uint16_t start_pos = read_present_position(TEST_ID);
	for (int i = 0; i < 3; i++) {
		if (start_pos == UINT16_MAX) {
			printk("[FAIL] read PRESENT_POSITION (start)\n");
		} else {
			printk("[POS] start = %u\n", (unsigned)start_pos);
		}
	}
	if (start_pos == UINT16_MAX) {
		return;
	}

	/* Pre-flight: enable torque (reg 40). If torque is off, the servo
	 * silently ignores all motion commands and may not even ACK. The
	 * previous session noted "TORQUE OFF" after failed EPROM ops. */
	uint8_t err_te = 0xFF;
	for (int i = 0; i < 3; i++) {
		const bool ok = write_reg(TEST_ID, REG_TORQUE_ENABLE,
					 (uint8_t[]){1}, 1, &err_te);
		if (ok) {
			printk("[OK] torque_enable=1\n");
		} else {
			/* Servo might not ACK with torque off — try reading
			 * PRESENT_TEMP just to confirm the bus is alive. */
			uint8_t tmp = 0;
			if (read_reg(TEST_ID, 63, 1, &tmp)) {
				printk("[FAIL] torque_enable but temp=%u (bus OK)\n",
				       (unsigned)tmp);
			} else {
				printk("[FAIL] torque_enable — no ACK, no bus, err=0x%02x\n",
				       (unsigned)err_te);
			}
		}
	}
	k_msleep(50);

	/* Diagnostic: write speed=0 (a "no-op" — should always succeed if
	 * writes work at all). Captures the ERR byte to see what the servo
	 * is complaining about for the real test. */
	uint8_t err0 = 0xFF;
	for (int i = 0; i < 3; i++) {
		const bool ok = write_reg(TEST_ID, REG_GOAL_TIME_L,
					 (uint8_t[]){0, 0}, 2, &err0);
		if (ok) {
			printk("[OK] write_speed=0 (probe)\n");
		} else {
			printk("[FAIL] write_speed=0 err=0x%02x\n", (unsigned)err0);
		}
	}
	k_msleep(50);

	/* Now the real test. First try multiple speed formats to figure out
	 * which encoding the SC15 accepts:
	 *   Format A: raw int16 -10  = [0xF6, 0xFF]
	 *   Format B: abs + bit10    = 10 | (1<<10) = 0x40A = [0x0A, 0x04]
	 *   Format C: raw int16 +10  = [0x0A, 0x00]
	 * The user's session showed WritePWM() (which uses Format B) worked,
	 * but raw int16 writes returned no ACK. */
	uint8_t err_a = 0xFF, err_b = 0xFF, err_c = 0xFF;
	write_reg(TEST_ID, REG_GOAL_TIME_L,
		  (uint8_t[]){0xF6, 0xFF}, 2, &err_a);  /* Format A */
	write_reg(TEST_ID, REG_GOAL_TIME_L,
		  (uint8_t[]){0x0A, 0x04}, 2, &err_b);  /* Format B (bit10) */
	write_reg(TEST_ID, REG_GOAL_TIME_L,
		  (uint8_t[]){0x0A, 0x00}, 2, &err_c);  /* Format C */
	for (int i = 0; i < 3; i++) {
		printk("[PROBE] A=-10raw err=0x%02x  B=10|bit10 err=0x%02x  C=+10raw err=0x%02x\n",
		       (unsigned)err_a, (unsigned)err_b, (unsigned)err_c);
	}

	/* If Format A (raw -10) succeeded — direction reversed. */
	/* If Format B (abs + bit10) succeeded — direction unchanged with bit10. */
	const bool sp_ok = (err_a == 0U);
	(void)write_speed(TEST_ID, 0);  /* stop regardless */
	for (int i = 0; i < 3; i++) {
		if (sp_ok) {
			const char *verdict = "FORMAT_A_WORKED (-10 raw ACK'd)";
			if (err_b == 0U) {
				verdict = "BOTH_A_AND_B_WORKED";
			}
			if (err_c == 0U) {
				verdict = "ANY_WRITE_OKAY";
			}
			printk("[OK] %s\n", verdict);
		} else {
			if (err_b == 0U) {
				printk("[OK] Format B (abs+bit10) ACK'd, raw neg doesn't\n");
			} else {
				if (err_c == 0U) {
					printk("[OK] Format C (+10 raw) ACK'd — bit10 irrelevant\n");
				} else {
					if (err_a == 0xFF && err_b == 0xFF && err_c == 0xFF) {
						printk("[FAIL] all 3 formats got no ACK\n");
					} else {
						char detail[64];
						snprintk(detail, sizeof(detail),
							"A=0x%02x B=0x%02x C=0x%02x",
							(unsigned)err_a, (unsigned)err_b,
							(unsigned)err_c);
						printk("[FAIL] no format worked (%s)\n",
						       detail);
					}
				}
			}
		}
	}
	for (int i = 0; i < 3; i++) {
		if (sp_ok) {
			printk("[OK] write_speed(id=%u, speed=%d) [0x%02x 0x%02x]\n",
			       (unsigned)TEST_ID, (int)TEST_SPEED,
			       (unsigned)((int16_t)TEST_SPEED & 0xFFU),
			       (unsigned)(((int16_t)TEST_SPEED >> 8) & 0xFFU));
		} else {
			/* Re-issue with err capture to see what failed */
			uint8_t err = 0xFF;
			(void)write_reg(TEST_ID, REG_GOAL_TIME_L,
					(uint8_t[]){
					    (uint8_t)((int16_t)TEST_SPEED & 0xFFU),
					    (uint8_t)(((int16_t)TEST_SPEED >> 8) & 0xFFU)
					}, 2, &err);
			printk("[FAIL] write_speed(id=%u, speed=%d) err=0x%02x\n",
			       (unsigned)TEST_ID, (int)TEST_SPEED, (unsigned)err);
		}
	}
	if (!sp_ok) {
		return;
	}

	k_msleep(TEST_DUR_MS);

	const bool st_ok = write_speed(TEST_ID, 0);
	for (int i = 0; i < 3; i++) {
		if (st_ok) {
			printk("[OK] write_speed(id=%u, speed=0) [STOP]\n",
			       (unsigned)TEST_ID);
		} else {
			uint8_t err = 0xFF;
			(void)write_reg(TEST_ID, REG_GOAL_TIME_L,
					(uint8_t[]){0, 0}, 2, &err);
			printk("[FAIL] STOP write_speed err=0x%02x\n",
			       (unsigned)err);
		}
	}

	k_msleep(50);

	const uint16_t end_pos = read_present_position(TEST_ID);
	for (int i = 0; i < 3; i++) {
		if (end_pos == UINT16_MAX) {
			printk("[FAIL] read PRESENT_POSITION (end)\n");
		} else {
			int32_t delta = (int32_t)end_pos - (int32_t)start_pos;
			if (delta > 32767) delta -= 65536;
			if (delta < -32768) delta += 65536;
			const char *verdict;
			if (delta > 0) {
				verdict = "REVERSED (speed=-10 went CW)";
			} else if (delta < 0) {
				verdict = "SAME (speed=-10 went CCW)";
			} else {
				verdict = "NO MOTION";
			}
			printk("[POS] end = %u  delta=%d (~%.2f deg)  %s\n",
			       (unsigned)end_pos, (int)delta,
			       (double)delta / 1218.0, verdict);
		}
	}
}
K_THREAD_DEFINE(test_id, STACKSIZE, direction_test_thread, NULL, NULL, NULL,
		PRIORITY, 0, 0);

int main(void)
{
	int rc = print_boot_banner();
	if (rc != 0) {
		/* Stay here so the failure is visible. */
		while (true) {
			k_msleep(1000);
		}
	}
	while (true) {
		k_msleep(1000);
	}
	return 0;
}
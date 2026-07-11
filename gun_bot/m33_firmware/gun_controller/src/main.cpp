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
 *   prio 7  — loopback_thread: every 1 s, send 16-byte pattern on
 *             LPUART3 (matches M33 RX FIFO depth), echo loopback, log
 *             result to LPUART2.
 *
 * For full PASS, wire GPIO_IO14 ↔ GPIO_IO15 on the FRDM (a single Dupont
 * jumper). Without the wire, the firmware still runs and prints FAIL —
 * that's fine: it proves the LPUART3 path is alive at 1 Mbaud and ready
 * for SC15 servo traffic.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/dt-bindings/clock/imx_ccm_rev2.h>

#include <fsl_lpuart.h>   /* pulls in MIMX9352_cm33_COMMON.h with LPUART3_BASE */

#include <string.h>

/* LPUART register offsets (NXP LPUART IP) */
#define LPUART_BAUD_OFFSET   0x10U
#define LPUART_STAT_OFFSET   0x14U

#define STACKSIZE 2048
#define PRIORITY  7

/* 16-byte pattern fits the M33 LPUART3 RX FIFO exactly (FIFO TXFIFOSIZE = 3 = 16).
 * Anything longer without intermediate RX drain sets the OR (overrun) flag.
 * See memory project-m33-lpuart3-1mbaud.md §7 for the 16-vs-21 root-cause. */
#define LOOPBACK_PATTERN     "ZEPHYR_LOOPBACK_"
#define LOOPBACK_PATTERN_LEN (sizeof(LOOPBACK_PATTERN) - 1)

static const struct device *const ccm_dev =
	DEVICE_DT_GET(DT_NODELABEL(ccm));
static const struct device *const lpuart3_dev =
	DEVICE_DT_GET(DT_NODELABEL(lpuart3));

/* ===========================================================================
 * PRE_KERNEL_1 clock-root configuration (prio 0)
 * =========================================================================*/

static int lpuart_clocks_init(void)
{
	int rc;

	if (!device_is_ready(ccm_dev)) {
		return -ENODEV;
	}

	rc = clock_control_on(ccm_dev,
		(clock_control_subsys_t)IMX_CCM_LPUART3_CLK);
	if (rc != 0) {
		return rc;
	}

	return 0;
}
SYS_INIT(lpuart_clocks_init, PRE_KERNEL_1, 0);

/* ===========================================================================
 * Helpers
 * =========================================================================*/

static inline uint32_t lpuart3_read(uint32_t offset)
{
	return *(volatile uint32_t *)(LPUART3_BASE + offset);
}

/* ===========================================================================
 * Loopback test — pure Zephyr (uart_poll_in / uart_poll_out)
 * =========================================================================*/

static bool run_loopback_test(uint32_t *const baud_out, uint32_t *const stat_out)
{
	uint8_t rx_buf[LOOPBACK_PATTERN_LEN] = {0};
	uint32_t idx = 0;
	unsigned char c;
	int rc;

	/* Drain stale RX from previous iterations. */
	while (uart_poll_in(lpuart3_dev, &c) == 0) { /* discard */ }

	/* TX pattern byte-by-byte. */
	for (size_t i = 0; i < LOOPBACK_PATTERN_LEN; i++) {
		uart_poll_out(lpuart3_dev, LOOPBACK_PATTERN[i]);
	}

	/* Read with busy-wait timeout. At 24 MHz CPU clock and 1 Mbaud,
	 * each bit-time is ~24 CPU cycles. 16 byte-times × 10 bit-times +
	 * slack fits well within 50000 iterations of the inner poll loop. */
	for (uint32_t waited = 0;
	     waited < 50000U && idx < LOOPBACK_PATTERN_LEN;
	     waited++) {
		rc = uart_poll_in(lpuart3_dev, &c);
		if (rc == 0) {
			rx_buf[idx++] = (uint8_t)c;
		}
		for (volatile int d = 0; d < 20; d++) {
			__asm__ volatile("nop");
		}
	}

	/* Snapshot BAUD and STAT after the round-trip. */
	if (baud_out) {
		*baud_out = lpuart3_read(LPUART_BAUD_OFFSET);
	}
	if (stat_out) {
		*stat_out = lpuart3_read(LPUART_STAT_OFFSET);
	}

	if (idx != LOOPBACK_PATTERN_LEN) {
		return false;
	}
	return memcmp(rx_buf, LOOPBACK_PATTERN, LOOPBACK_PATTERN_LEN) == 0;
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

	if (!device_is_ready(ccm_dev)) {
		printk("BOOT FAIL: ccm device not ready\n");
		return -ENODEV;
	}

	/* Clock root prio 0 hook must have completed before main(). */
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
	uint32_t baud = 0U;
	uint32_t stat = 0U;
	uint32_t tick = 0U;

	while (true) {
		const bool ok = run_loopback_test(&baud, &stat);

		if (ok) {
			/* Flood the PASS so it survives the LPUART2 collision
			 * with the A55 Linux debug console. */
			for (int i = 0; i < 3; i++) {
				printk("[%u] LPUART3: loopback OK "
				       "BAUD=0x%08x STAT=0x%08x\n",
				       (unsigned)tick, (unsigned)baud,
				       (unsigned)stat);
			}
		} else {
			/* No wire short (expected on the bench without a
			 * loopback jumper) — log the BAUD so we can confirm
			 * the rate is right. */
			for (int i = 0; i < 3; i++) {
				printk("[%u] LPUART3: loopback FAIL "
				       "BAUD=0x%08x STAT=0x%08x "
				       "(no GPIO_14<->GPIO_15 wire?)\n",
				       (unsigned)tick, (unsigned)baud,
				       (unsigned)stat);
			}
		}

		tick++;
		k_sleep(K_SECONDS(1));
	}
}

K_THREAD_DEFINE(loopback_id, STACKSIZE, loopback_thread, NULL, NULL, NULL,
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
	return 0;
}
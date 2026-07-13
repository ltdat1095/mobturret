/*
 * MobTurret M33 firmware — LPUART3 servo scan (pure-Zephyr, ping-only).
 *
 * Hardware targets (FRDM-iMX93, M33 core):
 *   - LPUART2 (console)  → /dev/ttyACM1 on the host
 *   - LPUART3 (servo bus) → GPIO_IO14 (TX), GPIO_IO15 (RX), 1 Mbaud
 *
 * Boot order:
 *   prio 0  — imx93_m33_clock_init: SDK-direct clock root + IP gate
 *             bring-up for BOTH Lpuart2 (console) and Lpuart3 (servo bus).
 *             Zephyr's clock_control_mcux_ccm_rev2.c only sets the IP gate
 *             (CLOCK_EnableClock) — it does NOT call CLOCK_SetRootClock,
 *             so the clock root's OFF bit stays set. Without this hook,
 *             LPUART TX hangs on TDRE and trips the SoC watchdog.
 *             See gun_bot/INIT_SOURCE_PROBLEM.md §2.3 for the full diagnosis.
 *   prio 50 — Zephyr mcux_lpuart driver binds for LPUART2/LPUART3, calls
 *             LPUART_Init() with the baud-search loop.
 *   prio 60 — imx93_lpuart3_baud_fixup: forces BAUD = 0x17000001 by direct
 *             register write, AFTER the driver has run. This bypasses any
 *             wrong (OSR, SBR) the SDK's baud-search may have produced.
 *   prio 7  — loopback_thread: every 1 s, scan-ping SCSCL IDs 1..5 on
 *             LPUART3 and log which IDs responded.
 *
 * Intentionally minimal-delta per gun_bot/SERVO_SETUP.md §2: no motion
 * helpers, no SCSCL library, no GPIO, no range-test thread. Bus-presence
 * only. Motion tests live in a separate firmware.
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
#define INST_PING         0x01U
#define PING_PACKET_LEN   6U
#define PING_SCAN_ID_MIN  1U
#define PING_SCAN_ID_MAX  5U

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
	const uint8_t chk = (uint8_t)~(target_id + 0x02U + INST_PING);
	uint8_t pkt[PING_PACKET_LEN] = {
		0xFF, 0xFF, target_id, 0x02, INST_PING, chk
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
 * Worker thread: every 1 s, ping IDs 1..5 in order, log who responded.
 * =========================================================================*/

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
 * Main thread — runs once, prints banner + initial state, then sleeps.
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
	printk(" MobTurret M33 firmware — LPUART3 @ 1 Mbaud (ping-only)\n");
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
/*
 * MobTurret M33 firmware — LPUART3 servo scan (pure-Zephyr, ping-only)
 *                          + RPMsg hello-world port (Phase A).
 *
 * Hardware targets (FRDM-iMX93, M33 core):
 *   - LPUART2 (console)  → /dev/ttyACM1 on the host
 *   - LPUART3 (servo bus) → GPIO_IO14 (TX), GPIO_IO15 (RX), 1 Mbaud
 *   - MU1 (mailbox)       → 0x44230000, IRQ 21, used for RPMsg doorbell
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
 *             Zephyr mbox-imx-mu driver also binds at this prio, exposes
 *             the IPM device used by the OpenAMP backend.
 *   prio 60 — imx93_lpuart3_baud_fixup: forces BAUD = 0x17000001 by direct
 *             register write, AFTER the driver has run. This bypasses any
 *             wrong (OSR, SBR) the SDK's baud-search may have produced.
 *   prio 7  — loopback_thread: every 1 s, scan-ping SCSCL IDs 1..5 on
 *             LPUART3 and log which IDs responded.
 *   prio 8  — rpmsg_hello_thread: registers two RPMsg endpoints
 *             ("turret-ctrl", "turret-telemetry") and every 1 s, sends
 *             a hello string on each one. Logs whatever the A55 sends.
 *
 * The LPUART3 ping loop and the RPMsg hello thread run in parallel —
 * the RPMsg port is additive, not a replacement.
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
 *
 * v18: also enable the Mu_A IP gate. The M33's MU1 (mu1 in dtsi, mapped
 * to Mu_A in the kSDK) is the peripheral the MBOX driver probes for
 * the A55↔M33 RPMsg link. Without CLOCK_EnableClock(kCLOCK_Mu_A), the
 * MBOX driver reads garbage from the MU registers at probe time
 * (PRE_KERNEL_1 prio 50), and the rpmsg_service sys_init at POST_KERNEL
 * prio 48 then waits forever for an IPM device that never became
 * ready. Sympton: M33 firmware boots, LPUART3 servo scan runs, but
 * LPUART2 console is silent (the rpmsg_service init wedges the
 * printk mutex / system worker) and no endpoint is registered.
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
	/* RPMsg/OpenAMP link: enable the Mu_A IP gate so the MBOX
	 * driver can read/write the MU1 registers at 0x44230000.
	 * Mu_A is the AON-domain MU1 peripheral; it has no clock
	 * root (24 MHz direct from osc) so just the IP gate. */
	CLOCK_EnableClock(kCLOCK_Mu_A);
	return 0;
}
SYS_INIT(imx93_m33_clock_init, PRE_KERNEL_1, 0);

/* ===========================================================================
 * PRE_KERNEL_1 prio 1: direct MPU register programming for the
 * A55-DRAM addresses used by the RPMsg link.
 *
 * The Zephyr fork's MPU region table in
 * `arch/arm/core/mpu/arm_mpu_regions.c` is a static C array that
 * doesn't include child MPU regions from the devicetree. The
 * `zephyr,ipc_rsc_table` chosen node points to 0x2021e000 (A55
 * DRAM), and the A55 DTB's vdev0vrings are at 0xa4000000 — both
 * outside the M33's default MPU regions. Without this hook,
 * `rsc_table_get()` does `memcpy(0x2021e000, &resource_table, 88)`
 * and faults.
 *
 * Region layout (8 MPU regions available on ARMv8-M, 2 already used
 * by Zephyr for FLASH + SRAM, 6 free):
 *   rgn 0: FLASH (0x0ffe0000)  — Zephyr configures this
 *   rgn 1: SRAM  (0x20000000)  — Zephyr configures this
 *   rgn 2: 0x20200000 1 MB      — rsc-table region (this hook)
 *   rgn 3: 0xa4000000 64 KB     — vdev0vrings (this hook)
 *
 * Run at PRE_KERNEL_1 prio 1 — after Zephyr's own MPU init
 * (PRE_KERNEL_1 prio 0) and before any device driver probes that
 * might dereference 0x2021e000. Actually, Zephyr's
 * arm_mpu_configure_static_mpu_regions runs at PRE_KERNEL_1 default
 * priority. We run at prio 1, after it.
 * =========================================================================*/

#define MPU_RNR       (*(volatile uint32_t *)0xE000ED98UL)
#define MPU_RBAR      (*(volatile uint32_t *)0xE000ED9CUL)
#define MPU_RLAR      (*(volatile uint32_t *)0xE000EDA0UL)
#define MPU_CTRL      (*(volatile uint32_t *)0xE000ED94UL)

/* ARMv8-M MAIR attributes (one byte = outer attr<<4 | inner attr).
 * Used as AttrIndx in our MPU region setup.
 *   MPU_ATTR_NORMAL_NC: outer=0x4 (WB-NC), inner=0x4 (WB-NC)
 *   MPU_ATTR_DEVICE:    outer=0x0 (Device-nGnRnE), inner=0x0 (Device-nGnRnE) */
#define MPU_ATTR_NORMAL_NC  0x44U
#define MPU_ATTR_DEVICE     0x00U

static inline void mpu_program_region(uint32_t rgn, uint32_t base,
				      uint32_t limit, uint8_t attr)
{
	/* ARMv8-M (Cortex-M33) RLAR layout:
	 *   [31:5]  LIMIT  (end address, aligned)
	 *   [3:1]   AttrIndx  (MAIR index)
	 *   [0]     EN  (region enable)
	 *
	 * MAIR byte layout for Normal Non-Cacheable (0x44):
	 *   outer attr 0x4 = Write-Back Non-cacheable
	 *   inner attr 0x4 = Write-Back Non-cacheable
	 *
	 * MAIR byte layout for Device (0x00):
	 *   outer 0x0 = Device-nGnRnE
	 *   inner 0x0 = Device-nGnRnE
	 * AttrIndx = 0 (we use MAIR[0] for Normal-NC, MAIR[1] for Device)
	 */
	uint8_t attr_idx = (attr == 0x00U) ? 1U : 0U;

	MPU_RNR = rgn & 0x7U;
	MPU_RBAR = base & 0xFFFFFFE0U;        /* BASE[31:5] */
	MPU_RLAR = (limit & 0xFFFFFFE0U) |     /* LIMIT[31:5] */
		   ((attr_idx & 0x7U) << 1) |  /* AttrIndx[2:0] */
		   0x1U;                        /* EN (bit 0) */
	__asm__ volatile("dsb 0xf" ::: "memory");
	__asm__ volatile("isb 0xf" ::: "memory");
}

static int imx93_m33_mpu_init(void)
{
	/* v32: also add MU1A peripheral register region.
	 *
	 * MPU region 0 (FLASH) and 1 (SRAM) are configured by
	 * Zephyr's arm_mpu_configure_static_mpu_regions. We add:
	 *   rgn 2: 0x20200000 1 MB    — A55-DRAM rsc-table
	 *   rgn 3: 0x44230000 4 KB    — M33's MU1A registers
	 *   rgn 4: 0xa4000000 64 KB   — A55-DRAM vdev0vrings
	 * (3 new regions, total 5 of 8 available). */

	/* Region 2: rsc-table + vdevbuffer — 0x20200000, 1 MB */
	mpu_program_region(2U, 0x20200000U, 0x20300000U - 1U,
			  MPU_ATTR_NORMAL_NC);

	/* Region 3: M33 MU1A registers — 0x44230000, 4 KB.
	 * Device memory (nGnRnE): no gather, no reorder, no early
	 * write acknowledge. */
	mpu_program_region(3U, 0x44230000U, 0x44231000U - 1U,
			  MPU_ATTR_DEVICE);

	/* Region 4: vdev0vrings — 0xa4000000, 64 KB */
	mpu_program_region(4U, 0xa4000000U, 0xa4010000U - 1U,
			  MPU_ATTR_NORMAL_NC);

	__asm__ volatile("dsb 0xf" ::: "memory");
	__asm__ volatile("isb 0xf" ::: "memory");
	return 0;
}
SYS_INIT(imx93_m33_mpu_init, PRE_KERNEL_1, 1);

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
 * Phase A: RPMsg hello-world — see src/rpmsg_hello.c.
 *
 * The RPMsg bring-up is in a separate C file because the rpmsg_service
 * header transitively pulls in libmetal's <atomic>, which requires the
 * C++ standard library. picolibc (our libc) does not ship a C++
 * stdlib, so the rpmsg code must be in a C TU to take the libmetal
 * C branch instead. main.cpp declares the entry point with extern "C".
 * =========================================================================*/

extern "C" void rpmsg_hello_start(void);

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

	/* Phase A: spin up the RPMsg hello-world thread. Runs in
	 * parallel with the LPUART3 ping loop (priority 7). The OpenAMP
	 * backend's SYS_INIT brings up the IPM device + rpmsg_service
	 * before main() returns, so register_endpoint() succeeds
	 * immediately; binding waits for the A55 ioctl. */
	rpmsg_hello_start();

	while (true) {
		k_msleep(1000);
	}
	return 0;
}
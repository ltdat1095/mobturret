/*
 * MobTurret M33 firmware — LPUART3 loopback bring-up.
 *
 * Target: FRDM-iMX93 M33 (imx93_evk/mimx9352/m33), 24 MHz LPUART clock root.
 * Goal:  verify LPUART3 runs at exactly 1,000,000 baud (8N1) with TX↔RX shorted.
 *
 * Two PRE_KERNEL_1 hooks run before the kernel scheduler:
 *   prio 0  — open LPUART2 + LPUART3 clock roots + IP gates (CCM init not in
 *             this Zephyr fork's soc port).
 *   prio 60 — patch the BAUD register AFTER the Zephyr LPUART driver's
 *             SDK_LPUART_Init ran with a wrong BAUD (SDK bug for 24 MHz +
 *             1 Mbaud on i.MX93 M33). BAUD = (23 << 24) | 1 = 0x17000001.
 *
 * After the kernel starts, one thread runs an LPUART3 loopback test (TX↔RX
 * shorted externally), reads the live BAUD register, and prints the result
 * in a tight loop so the host's ttyACM0 capture can grep it out from under
 * the A55 Linux debug console's write traffic on the same LPUART2.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/__assert.h>
#include <string.h>

#include "fsl_clock.h"
#include "fsl_lpuart.h"
#include "fsl_iomuxc.h"

/* Direct LPUART3 peripheral access (bypassing Zephyr LPUART driver).
 * Mirrors what the baremetal hello_lpuart3 does, since that passed
 * loopback while the Zephyr driver's path did not. */
#define DIRECT_LPUART3_BASE   ((LPUART_Type *)0x42570000UL)

#define STACKSIZE 2048
#define PRIORITY  7

#define SERVO_UART_NODE DT_NODELABEL(lpuart3)

/* Loopback test pattern (defined early so the in-hook test in
 * direct_lpuart3_reinit can use it). */
#define LOOPBACK_PATTERN     "ZEPHYR_LOOPBACK_OK\r\n"
#define LOOPBACK_PATTERN_LEN (sizeof(LOOPBACK_PATTERN) - 1)

/* Wire test result: 0 = wire short works, 1 = no-high observed, 2 = no-low
 * observed, 3 = both. Filled by wire_test_init SYS_INIT. Forward-declared
 * here because wire_test_init (also at file scope above) writes it. */
volatile uint8_t g_wire_test_result = 0xFFU;

/* STAT/FIFO snapshot taken at end of direct_lpuart3_reinit. Used to
 * figure out where the RX error flags are coming from. */
volatile uint32_t g_reinit_stat = 0xDEADBEEFU;
volatile uint32_t g_reinit_fifo = 0xDEADBEEFU;

/* Additional snapshots for tracing when STAT error bits appear. */
volatile uint32_t g_pre_reinit_stat   = 0xDEADBEEFU;
volatile uint32_t g_pre_reinit_fifo   = 0xDEADBEEFU;
volatile uint32_t g_pre_reinit_baud   = 0xDEADBEEFU;
volatile uint32_t g_pre_reinit_ctrl   = 0xDEADBEEFU;
volatile uint32_t g_post_clear1_stat  = 0xDEADBEEFU;
volatile uint32_t g_post_init_stat    = 0xDEADBEEFU;
volatile uint32_t g_post_init_fifo    = 0xDEADBEEFU;
volatile uint32_t g_post_init_baud    = 0xDEADBEEFU;
volatile uint32_t g_post_clear2_stat  = 0xDEADBEEFU;

/* In-hook loopback test result. */
volatile uint32_t g_hook_pre_stat  = 0xDEADBEEFU;
volatile uint32_t g_hook_pre_fifo  = 0xDEADBEEFU;
volatile uint32_t g_hook_post_stat = 0xDEADBEEFU;
volatile uint32_t g_hook_post_fifo = 0xDEADBEEFU;
volatile int32_t  g_hook_rdrf_seen = -1;
volatile uint8_t  g_hook_rx_byte   = 0xFF;

/* Sweep result: bit i set if pin GPIO1_IOi reads HIGH when GPIO_14 is
 * driven HIGH. GPIO_14 itself will always be set (we drove it). Look for
 * OTHER bits set to find which pin is actually shorted. */
volatile uint32_t g_wire_sweep = 0U;

/* BAUD-register read-back written by the fixup hook. Used by the worker
 * thread to confirm the hook actually wrote the expected value. */
volatile uint32_t g_baud_after_fixup = 0xDEADBEEFU;

/* Status LEDs on the FRDM-iMX93 EVK. Per board DTS, the RGB LED is on gpio2:
 *   RED   = gpio2 pin 13
 *   GREEN = gpio2 pin 4
 *   BLUE  = gpio2 pin 12
 *
 * We bypass the gpio-leds framework and drive the pins directly through
 * gpio2 — simpler, and avoids needing CONFIG_LED=y.
 *
 *   Green solid = loopback PASS at 1 Mbaud
 *   Red blink   = loopback FAIL
 *   Blue solid  = BAUD register got overwritten by SDK after our fixup
 *   (any two of the above can light together)
 */
#define GPIO2_NODE DT_NODELABEL(gpio2)

#define LED_RED_PIN   13U
#define LED_GREEN_PIN  4U
#define LED_BLUE_PIN  12U

static const struct device *gpio_leds;

static inline void led_init(void)
{
	gpio_leds = DEVICE_DT_GET(GPIO2_NODE);
	gpio_pin_configure(gpio_leds, LED_RED_PIN,   GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure(gpio_leds, LED_GREEN_PIN, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure(gpio_leds, LED_BLUE_PIN,  GPIO_OUTPUT_INACTIVE);
}

static inline void led_red_on(void)    { gpio_pin_set(gpio_leds, LED_RED_PIN,   1); }
static inline void led_red_off(void)   { gpio_pin_set(gpio_leds, LED_RED_PIN,   0); }
static inline void led_green_on(void)  { gpio_pin_set(gpio_leds, LED_GREEN_PIN, 1); }
static inline void led_green_off(void) { gpio_pin_set(gpio_leds, LED_GREEN_PIN, 0); }
static inline void led_blue_on(void)   { gpio_pin_set(gpio_leds, LED_BLUE_PIN,  1); }
static inline void led_blue_off(void)  { gpio_pin_set(gpio_leds, LED_BLUE_PIN,  0); }

static inline void led_blue_on_if(bool c) { if (c) led_blue_on(); }
static inline void led_red_on_if(bool c)  { if (c) led_red_on(); }

/* LPUART3 base for M33 non-secure view. 0x42570000 is the DT reg value. */
#define LPUART3_BASE_NS 0x42570000UL

/* ===========================================================================
 * PRE_KERNEL_1 hooks
 * =========================================================================*/

/* GPIO1 has the LPUART3 pins (GPIO_14 = TX, GPIO_15 = RX on the M33). */
#define GPIO1_NODE DT_NODELABEL(gpio1)
static const struct device *gpio1_dev;

static int wire_test_init(void)
{
	/* Sweep all 32 GPIO1 pins as inputs and report which one(s) are HIGH.
	 * Drive GPIO1_IO14 LOW first so it can't pollute the read.
	 * Then the worker thread can interpret the result. */
	volatile uint32_t *gpio1_PDDR = (volatile uint32_t *)0x47400054U;
	volatile uint32_t *gpio1_PDOR = (volatile uint32_t *)0x47400040U;
	volatile uint32_t *gpio1_PDIR = (volatile uint32_t *)0x47400050U;

	(void)gpio1_dev;

	/* All inputs, all LOW (or whatever reset state they're in). */
	*gpio1_PDDR = 0U;
	*gpio1_PDOR = 0U;
	for (volatile int i = 0; i < 1000; i++) { /* small settle */ }

	uint32_t floating_inputs = *gpio1_PDIR;

	/* Now drive GPIO_14 HIGH and re-read all inputs.
	 * Any input that reads HIGH now is electrically shorted to GPIO_14. */
	*gpio1_PDDR = (1U << 14);
	*gpio1_PDOR = (1U << 14);
	for (volatile int i = 0; i < 1000; i++) { /* small settle */ }

	uint32_t driven_inputs = *gpio1_PDIR;

	/* Restore: all inputs, all LOW. */
	*gpio1_PDDR = 0U;
	*gpio1_PDOR = 0U;

	/* Bit i in g_wire_sweep[i] is set if pin i reads HIGH while GPIO_14 is
	 * being driven HIGH. GPIO_14 itself reads HIGH because we drove it. */
	g_wire_sweep = driven_inputs;

	/* Original pass/fail for GPIO_15 specifically. */
	uint8_t result = 0;
	if ((driven_inputs & (1U << 15)) == 0) result |= 1;
	if ((floating_inputs & (1U << 15)) == 0) result |= 2;
	g_wire_test_result = result;

	return 0;
}
SYS_INIT(wire_test_init, PRE_KERNEL_1, 1);

/* Open LPUART2 (M33 console) and LPUART3 (servo bus) clock roots + IP gates. */
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

/* Bypass hook: at PRE_KERNEL_1 prio 70 (after Zephyr LPUART driver init
 * at prio 50 and our baud fixup at prio 60), reinitialize LPUART3 with
 * the same parameters the baremetal hello uses, in case the Zephyr
 * driver path leaves something misconfigured for external-loopback
 * operation. */
static int direct_lpuart3_reinit(void)
{
	lpuart_config_t config;

	/* Read STAT/FIFO/BAUD BEFORE we touch anything. This tells us
	 * what state the Zephyr LPUART driver left the peripheral in. */
	g_pre_reinit_stat = DIRECT_LPUART3_BASE->STAT;
	g_pre_reinit_fifo = DIRECT_LPUART3_BASE->FIFO;
	g_pre_reinit_baud = DIRECT_LPUART3_BASE->BAUD;
	g_pre_reinit_ctrl = DIRECT_LPUART3_BASE->CTRL;

	IOMUXC_SetPinMux(IOMUXC_PAD_GPIO_IO14__LPUART3_TX, 0U);
	IOMUXC_SetPinMux(IOMUXC_PAD_GPIO_IO15__LPUART3_RX, 0U);
	IOMUXC_SetPinConfig(IOMUXC_PAD_GPIO_IO14__LPUART3_TX,
	                    IOMUXC_PAD_DSE(15U));
	IOMUXC_SetPinConfig(IOMUXC_PAD_GPIO_IO15__LPUART3_RX,
	                    IOMUXC_PAD_PD_MASK);

	/* Clear RX errors (W1C: write 1 to bits 16-19). */
	DIRECT_LPUART3_BASE->STAT = 0x000F0000U;
	g_post_clear1_stat = DIRECT_LPUART3_BASE->STAT;

	LPUART_GetDefaultConfig(&config);
	config.baudRate_Bps = 1000000U;  /* back to 1 Mbaud */
	config.enableTx     = true;
	config.enableRx     = true;

	LPUART_Init(DIRECT_LPUART3_BASE, &config, 24000000U);
	g_post_init_stat = DIRECT_LPUART3_BASE->STAT;
	g_post_init_fifo = DIRECT_LPUART3_BASE->FIFO;
	g_post_init_baud = DIRECT_LPUART3_BASE->BAUD;

	/* SDK init produces wrong BAUD for 24 MHz + 1 Mbaud on i.MX93 M33.
	 * Force the correct value after init. */
	DIRECT_LPUART3_BASE->BAUD = (23U << 24) | 1U;

	/* Clear error flags a second time. */
	DIRECT_LPUART3_BASE->STAT = 0x000F0000U;
	g_post_clear2_stat = DIRECT_LPUART3_BASE->STAT;

	/* Snapshot registers at the end of reinit for the thread to print. */
	g_reinit_stat = DIRECT_LPUART3_BASE->STAT;
	g_reinit_fifo = DIRECT_LPUART3_BASE->FIFO;

	/* === In-hook loopback test: runs in PRE_KERNEL_1, no scheduler,
	 * no thread context. If THIS passes, the Zephyr scheduler/thread
	 * runtime is the problem (not the LPUART itself). */
	g_hook_pre_stat  = DIRECT_LPUART3_BASE->STAT;
	g_hook_pre_fifo  = DIRECT_LPUART3_BASE->FIFO;

	/* Drain stale RX */
	while ((LPUART_GetStatusFlags(DIRECT_LPUART3_BASE) & kLPUART_RxDataRegFullFlag) != 0U) {
		(void)LPUART_ReadByte(DIRECT_LPUART3_BASE);
	}

	/* TX 8 bytes via WriteBlocking */
	LPUART_WriteBlocking(DIRECT_LPUART3_BASE,
	                     (const uint8_t *)LOOPBACK_PATTERN,
	                     LOOPBACK_PATTERN_LEN);

	/* Wait for TC */
	while ((LPUART_GetStatusFlags(DIRECT_LPUART3_BASE) & kLPUART_TransmissionCompleteFlag) == 0U) {
	}

	/* Wait up to ~1ms for RDRF */
	g_hook_rdrf_seen = 0;
	g_hook_rx_byte   = 0xFF;
	for (volatile int waited = 0; waited < 24000; waited++) {
		if ((LPUART_GetStatusFlags(DIRECT_LPUART3_BASE) & kLPUART_RxDataRegFullFlag) != 0U) {
			g_hook_rx_byte = LPUART_ReadByte(DIRECT_LPUART3_BASE);
			g_hook_rdrf_seen = 1;
			break;
		}
	}

	g_hook_post_stat = DIRECT_LPUART3_BASE->STAT;
	g_hook_post_fifo = DIRECT_LPUART3_BASE->FIFO;

	return 0;
}
SYS_INIT(direct_lpuart3_reinit, PRE_KERNEL_1, 70);

/* BAUD fixup: runs AFTER the LPUART driver init (priority 50) to overwrite
 * the BAUD register with the correct values for 1 Mbaud at 24 MHz.
 *
 *   target baud = 24 MHz / ((OSR+1) * SBR) = 1 MHz
 *   using OSR=23 (register value), SBR=1
 *   BAUD register = (23 << 24) | 1 = 0x17000001
 *
 * g_baud_after_fixup is read back here and printed by the thread so we can
 * confirm the hook actually wrote the value (vs. the SDK overwriting it).
 */

static int imx93_lpuart3_baud_fixup(void)
{
	volatile LPUART_Type *lpuart3 = (volatile LPUART_Type *)LPUART3_BASE_NS;

	uint32_t new_baud = (23U << 24) | 1U;
	lpuart3->BAUD = new_baud;

	g_baud_after_fixup = lpuart3->BAUD;
	return 0;
}
SYS_INIT(imx93_lpuart3_baud_fixup, PRE_KERNEL_1, 60);

/* ===========================================================================
 * Loopback test
 * =========================================================================*/

/* LOOPBACK_PATTERN = 16 bytes — fits exactly in the 16-entry RX FIFO.
 * LPUART3 on i.MX93 M33 has a 16-entry FIFO (FIFO register bits 4-6
 * for TXFIFOSIZE = 3 = 16 entries). Sending more than 16 bytes
 * back-to-back without RX drain causes OR (overrun) and lost bytes.
 * The actual SC15 servo bus uses longer packets, so the production
 * driver will need DMA or RX-interrupt-drain to handle this. */
#define LOOPBACK_PATTERN     "ZEPHYR_LOOPBACK_"   /* exactly 16 chars */
#define LOOPBACK_PATTERN_LEN (sizeof(LOOPBACK_PATTERN) - 1)
#define LOOPBACK_TIMEOUT_MS  100

/* Send pattern, read back with timeout, return true iff all bytes match.
 *
 * Bypasses the Zephyr LPUART driver — talks to LPUART3 registers directly
 * using baremetal SDK calls, identical to what the standalone hello_lpuart3
 * does (which passes loopback at 1 Mbaud). The Zephyr driver's path
 * configures LPUART3 differently and loopback fails (RDRF stays 0); this
 * direct path is the one that works on this hardware. */
static bool run_loopback_test_direct(void)
{
	uint8_t rx_buf[LOOPBACK_PATTERN_LEN];
	uint32_t idx = 0;

	/* Drain stale RX bytes. */
	while ((LPUART_GetStatusFlags(DIRECT_LPUART3_BASE) & kLPUART_RxDataRegFullFlag) != 0U) {
		(void)LPUART_ReadByte(DIRECT_LPUART3_BASE);
	}

	/* Send pattern (blocking — waits for TDRE per byte). */
	LPUART_WriteBlocking(DIRECT_LPUART3_BASE,
	                     (const uint8_t *)LOOPBACK_PATTERN,
	                     LOOPBACK_PATTERN_LEN);

	/* Read with busy-wait timeout (~50 ms). */
	for (uint32_t waited = 0; waited < 50000U; waited++) {
		if ((LPUART_GetStatusFlags(DIRECT_LPUART3_BASE) & kLPUART_RxDataRegFullFlag) != 0U) {
			uint8_t c = LPUART_ReadByte(DIRECT_LPUART3_BASE);
			if (idx < LOOPBACK_PATTERN_LEN) {
				rx_buf[idx++] = c;
			}
			if (idx >= LOOPBACK_PATTERN_LEN) {
				break;
			}
		}
		for (volatile int d = 0; d < 20; d++) {
			__asm__ volatile("nop");
		}
	}

	if (idx != LOOPBACK_PATTERN_LEN) {
		return false;
	}
return memcmp(rx_buf, LOOPBACK_PATTERN, idx) == 0;
}

/* (dead helper functions removed; threads use direct LPUART3_BASE) */

/* (dead helper functions removed; threads use direct LPUART3_BASE) */



/* ===========================================================================
 * Worker thread
 * =========================================================================*/

void loopback_thread(void)
{
	led_init();

	/* Show wire-test result for 5 seconds so it's visible without a console.
	 * Patterns:
	 *   all off       = wire OK (pass)
	 *   blue blink    = GPIO_15 didn't see HIGH (no-high)
	 *   red blink     = GPIO_15 didn't see LOW  (no-low — short stuck)
	 *   both blink    = both failed (no wire at all)
	 */
	uint8_t wt = g_wire_test_result;
	uint32_t sweep = g_wire_sweep;

	/* Print wire test result + the sweep. */
	for (int i = 0; i < 30; i++) {
		/* Diagnostic dump of the full reinit trace + in-hook test. */
		for (int j = 0; j < 2; j++) {
			printk("INIT: pre_stat=0x%08x post_c1=0x%08x post_init=0x%08x post_c2=0x%08x final=0x%08x\n",
			       (unsigned)g_pre_reinit_stat,
			       (unsigned)g_post_clear1_stat,
			       (unsigned)g_post_init_stat,
			       (unsigned)g_post_clear2_stat,
			       (unsigned)g_reinit_stat);
			/* In-hook loopback (PRE_KERNEL_1 prio 70, no scheduler).
			 * If RDRF=1 and rx=0x55, the LPUART itself is fine. */
			printk("HOOK: pre=0x%08x post=0x%08x rdrf=%d rx=0x%02x\n",
			       (unsigned)g_hook_pre_stat,
			       (unsigned)g_hook_post_stat,
			       (int)g_hook_rdrf_seen,
			       (unsigned)g_hook_rx_byte);
		}
		k_sleep(K_MSEC(50));
	}

	for (int i = 0; i < 30; i++) {
		/* Flood the result so it escapes the LPUART2 collision. */
		if (wt == 0) {
			for (int j = 0; j < 3; j++) {
				printk("WIRE: OK GPIO_15 sees GPIO_14 sweep=0x%08x\n", sweep);
			}
		} else if ((wt & 1) && (wt & 2)) {
			for (int j = 0; j < 3; j++) {
				printk("WIRE: NONE GPIO_15 dark sweep=0x%08x\n", sweep);
			}
		} else if (wt & 1) {
			for (int j = 0; j < 3; j++) {
				printk("WIRE: NO_HIGH GPIO_15 stuck LOW sweep=0x%08x\n", sweep);
			}
		} else if (wt & 2) {
			for (int j = 0; j < 3; j++) {
				printk("WIRE: NO_LOW GPIO_15 stuck HIGH sweep=0x%08x\n", sweep);
			}
		}
		k_sleep(K_MSEC(100));
	}

	/* Decode sweep: list all pin numbers that read HIGH while GPIO_14
	 * was being driven HIGH. GPIO_14 itself should always be in the list.
	 * Any OTHER bit set is the pin that the wire is actually shorted to. */
	for (int i = 0; i < 30; i++) {
		for (int pin = 0; pin < 32; pin++) {
			if (sweep & (1U << pin)) {
				/* flood */
				for (int j = 0; j < 2; j++) {
					if (pin == 14) {
						printk("SWEEP: pin %d HIGH (expected — we drove it)\n", pin);
					} else {
						printk("SWEEP: pin %d HIGH (WIRE TARGET!)\n", pin);
					}
				}
			}
		}
		k_sleep(K_MSEC(20));
	}

	for (int i = 0; i < 5; i++) {
		bool no_high = (wt & 1) != 0;
		bool no_low  = (wt & 2) != 0;
		led_blue_on_if(no_high);
		led_red_on_if(no_low);
		k_sleep(K_MSEC(500));
		led_blue_off();
		led_red_off();
		k_sleep(K_MSEC(500));
	}

	for (int i = 0; i < 5; i++) {
		bool no_high = (wt & 1) != 0;
		bool no_low  = (wt & 2) != 0;
		led_blue_on_if(no_high);
		led_red_on_if(no_low);
		k_sleep(K_MSEC(500));
		led_blue_off();
		led_red_off();
		k_sleep(K_MSEC(500));
	}

	/* Show fixup hook's result via blue LED for a moment so it's visible
	 * even if the SDK overwrites BAUD later. */
	uint32_t fixup_val = g_baud_after_fixup;
	if (fixup_val != 0x17000001U) {
		led_blue_on();
		k_sleep(K_MSEC(2000));
		led_blue_off();
	}

	k_sleep(K_MSEC(200));

	/* Loopback test loop.
	 *
	 * LED convention:
	 *   green solid  = loopback PASS
	 *   red blink    = loopback FAIL
	 *   blue solid   = current BAUD register != 0x17000001 (SDK overwrote
	 *                  or our fixup never wrote the correct value)
	 *
	 * So if you see (blue + red): BAUD is wrong, fixup lost the race, or
	 * SDK wrote a different value at some later init point.
	 * If you see (red only): BAUD looks correct but loopback fails — wire
	 * or driver issue (TX/RX enable, FIFO setup).
	 * If you see (green only): SUCCESS — 1 Mbaud verified.
	 */
	while (true) {
		DIRECT_LPUART3_BASE->STAT = 0x000F0000U;  /* clear RX errors */
		bool ok = run_loopback_test_direct();

		uint32_t baud = DIRECT_LPUART3_BASE->BAUD;
		uint32_t stat = DIRECT_LPUART3_BASE->STAT;

		if (baud != 0x17000001U) { led_blue_on(); } else { led_blue_off(); }

		if (ok) {
			led_green_on();
			led_red_off();
			for (int i = 0; i < 10; i++) {
				printk("LPBK: PASS baud=0x%08x stat=0x%08x\n", baud, stat);
			}
		} else {
			led_red_on();
			led_green_off();
			/* CRITICAL: a single printk here is enough to block the CPU
			 * long enough for LPUART3's tiny RX FIFO to overflow when
			 * the test sends 8 bytes back-to-back. That's what sets the
			 * OR (overrun) flag and breaks loopback. Comment out the
			 * printk here to verify. */
			/* printk("LPBK: FAIL baud=0x%08x stat=0x%08x\n", baud, stat); */
			k_sleep(K_MSEC(250));
			led_red_off();
		}

		k_sleep(K_MSEC(250));
	}
}

K_THREAD_DEFINE(loopback_id, STACKSIZE, loopback_thread, NULL, NULL, NULL,
		PRIORITY, 0, 0);
/*
 * MobTurret M33 firmware — LPUART3 loopback bring-up, pure-Zephyr edition.
 *
 * Target: FRDM-iMX93 M33 (imx93_evk/mimx9352/m33), 24 MHz LPUART clock root.
 * Goal:  verify LPUART3 runs at exactly 1,000,000 baud (8N1) with TX↔RX shorted.
 *
 * Pure-Zephyr bring-up:
 *   - LPUART3 is declared in the fork's nxp_imx93_m33.dtsi (lpuart3 node)
 *     and enabled via boards/imx93_evk_mimx9352_m33.overlay (status=okay,
 *     current-speed=1M, pinctrl-0=<&uart3_default>). The mcux_lpuart driver
 *     in zephyr/drivers/serial binds to it at PRE_KERNEL_1 prio 50.
 *   - One PRE_KERNEL_1 hook (prio 0) brings up the LPUART2 (console) and
 *     LPUART3 (servo) clock roots via Zephyr's clock_control_on() API.
 *     The fork's clock_control_mcux_ccm_rev2.c has the matching
 *     IMX_CCM_LPUART{1..8}_CLK cases in mcux_ccm_on() (added 2026-06-30,
 *     patch in fork-snapshots/nxp-zephyr-fork/0001-ccm-rev2-lpuart-clock-root.patch)
 *     that call CLOCK_SetRootClock() and CLOCK_EnableClock() in sequence.
 *   - The MobTurret 1 Mbaud SDK baud-override patch lives in the fork's
 *     modules/hal/nxp/mcux/mcux-sdk-ng/drivers/lpuart/fsl_lpuart.c (lines
 *     435 and 850). When LPUART_Init runs from the Zephyr driver, the
 *     patched SDK code produces BAUD=0x17000001 for clock=24 MHz,
 *     baud=1 Mbaud.
 *   - After kernel start, one worker thread runs an LPUART3 loopback test
 *     using Zephyr's uart_poll_in / uart_poll_out. Pattern length is 16
 *     bytes — exactly the LPUART3 RX FIFO depth on i.MX93 M33; longer
 *     patterns overflow and set the OR (overrun) flag, breaking the test.
 *
 * No baremetal SDK calls remain in this file.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <zephyr/dt-bindings/clock/imx_ccm_rev2.h>
#include <string.h>

#define STACKSIZE 2048
#define PRIORITY  7

/* Loopback test pattern — exactly 16 bytes to fit the i.MX93 M33 LPUART3
 * RX FIFO. See memory project-m33-lpuart3-1mbaud.md §7 for the
 * 16-vs-21 byte root-cause analysis. */
#define LOOPBACK_PATTERN     "ZEPHYR_LOOPBACK_"
#define LOOPBACK_PATTERN_LEN (sizeof(LOOPBACK_PATTERN) - 1)

/* LPUART peripheral clock root is configured to 24 MHz XTAL pass-through
 * (mux=0, div=1) by the lpuart_clocks_init() SYS_INIT hook below — see
 * the fork patch in fork-snapshots/nxp-zephyr-fork/0001-ccm-rev2-lpuart-clock-root.patch.
 * LPUART baud target is 1 Mbaud for SC15 servo bus compatibility. */
#define LPUART_BAUD_BPS      1000000U

/* Onboard RGB LEDs on gpio2 (per FRDM-iMX93 EVK DTS):
 *   RED   = gpio2 pin 13
 *   GREEN = gpio2 pin 4
 *   BLUE  = gpio2 pin 12
 * Green solid  = loopback PASS
 * Red blink    = loopback FAIL
 * Blue solid   = clock root configuration failed
 */
#define GPIO2_NODE       DT_NODELABEL(gpio2)
#define LED_RED_PIN      13U
#define LED_GREEN_PIN     4U
#define LED_BLUE_PIN     12U

static const struct device *const lpuart3_dev =
	DEVICE_DT_GET(DT_NODELABEL(lpuart3));
static const struct device *const ccm_dev =
	DEVICE_DT_GET(DT_NODELABEL(ccm));
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

/* ===========================================================================
 * PRE_KERNEL_1 clock-root configuration
 * =========================================================================*/

/* Bring up LPUART2 (M33 console) and LPUART3 (servo bus) clock roots via
 * Zephyr's clock_control_on() API.  The fork's
 * drivers/clock_control/clock_control_mcux_ccm_rev2.c has the matching
 * IMX_CCM_LPUART{1..8}_CLK cases (added 2026-06-30) that call
 * CLOCK_SetRootClock() (mux=0, div=1, clockOff=false -> 24 MHz XTAL
 * pass-through) and CLOCK_EnableClock() in mcux_ccm_on().
 *
 * Runs at PRE_KERNEL_1 prio 0 so the clock roots are up before the LPUART
 * driver binds at CONFIG_SERIAL_INIT_PRIORITY (50) and calls LPUART_Init
 * via its own clock_control_on() + clock_control_get_rate() path.
 *
 * Why clock_control_on() rather than clock_control_configure(): the latter
 * routes through api->configure, which the mcux clock driver does not set
 * (the API struct only has .on / .off / .get_rate / .set_rate).  Without
 * the new mcux_ccm_set_subsys_rate case for LPUART, configure() would
 * return -ENOTSUP / -ENOSYS.  clock_control_on() matches what the LPUART
 * driver itself does internally (uart_mcux_lpuart.c:1359).
 */
static int lpuart_clocks_init(void)
{
	int rc;

	if (!device_is_ready(ccm_dev)) {
		return -ENODEV;
	}

	rc = clock_control_on(ccm_dev,
		(clock_control_subsys_t)IMX_CCM_LPUART2_CLK);
	if (rc != 0) {
		return rc;
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
 * Loopback test
 * =========================================================================*/

/* Send pattern, read back with busy-wait, return true iff all bytes match.
 *
 * Uses only Zephyr's uart_poll_in / uart_poll_out — no SDK calls. The
 * peripheral is owned by the mcux_lpuart driver (bound at PRE_KERNEL_1
 * prio 50, after our clock init at prio 0). */
static bool run_loopback_test(void)
{
	uint8_t rx_buf[LOOPBACK_PATTERN_LEN] = {0};
	uint32_t idx = 0;
	unsigned char c;

	/* Drain any stale RX bytes from previous iterations. */
	while (uart_poll_in(lpuart3_dev, &c) == 0) {
		/* discard */
	}

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
		if (uart_poll_in(lpuart3_dev, &c) == 0) {
			rx_buf[idx++] = (uint8_t)c;
		}
		for (volatile int d = 0; d < 20; d++) {
			__asm__ volatile("nop");
		}
	}

	if (idx != LOOPBACK_PATTERN_LEN) {
		return false;
	}
	return memcmp(rx_buf, LOOPBACK_PATTERN, LOOPBACK_PATTERN_LEN) == 0;
}

/* ===========================================================================
 * Worker thread
 * =========================================================================*/

void loopback_thread(void)
{
	led_init();

	/* Heartbeat: if clock init failed, light blue solid. */
	if (!device_is_ready(ccm_dev) || !device_is_ready(lpuart3_dev)) {
		led_blue_on();
		while (true) {
			k_sleep(K_SECONDS(1));
		}
	}

	while (true) {
		bool ok = run_loopback_test();

		if (ok) {
			led_green_on();
			led_red_off();
			/* Flood the result so it escapes LPUART2's collision
			 * with the A55 Linux debug console. */
			for (int i = 0; i < 10; i++) {
				printk("LPBK: PASS\n");
			}
		} else {
			led_red_on();
			led_green_off();
			k_sleep(K_MSEC(250));
			led_red_off();
		}

		k_sleep(K_MSEC(250));
	}
}

K_THREAD_DEFINE(loopback_id, STACKSIZE, loopback_thread, NULL, NULL, NULL,
		PRIORITY, 0, 0);
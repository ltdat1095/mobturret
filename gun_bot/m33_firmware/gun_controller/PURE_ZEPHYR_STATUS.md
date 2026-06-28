# Pure Zephyr attempt — status and handoff

**Date:** 2026-06-28

**What was tried:** Move from the baremetal-SDK approach (working, 550+ loopback PASSes) to a fully Zephyr devicetree + `mcux_lpuart` driver approach (no baremetal SDK calls in `main.cpp`).

**Result: reverted.** The pure-Zephyr path needs fork work first; doing it in this session is not realistic because the Zephyr fork needs to be patched, built, and `west update`ed. The `nxp_zephyr` checkout that the in-tree build uses is missing the hal_nxp submodule and the lpuart3 dtsi node.

## What was done in this session (in `nxp_zephyr`)

1. **Added the `lpuart3` node to `nxp_zephyr/zephyr/dts/arm/nxp/imx/nxp_imx93_m33.dtsi`** (matching the fork branch):
   ```dts
   lpuart3: serial@42570000 {
       compatible = "nxp,imx-lpuart", "nxp,lpuart";
       reg = <0x42570000 DT_SIZE_K(64)>;
       interrupts = <68 3>;
       clocks = <&ccm IMX_CCM_LPUART3_CLK 0x6c 24>;
       status = "disabled";
   };
   ```

2. **Added the `uart3_default` pinctrl group to `nxp_zephyr/zephyr/boards/nxp/imx93_evk/imx93_evk-pinctrl.dtsi`** (referencing the existing `iomuxc1_gpio_io14_lpuart_tx_lpuart3_tx` and `iomuxc1_gpio_io15_lpuart_rx_lpuart3_rx` iomux entries from `hal_nxp`):
   ```dts
   uart3_default: uart3_default {
       group0 {
           pinmux = <&iomuxc1_gpio_io14_lpuart_tx_lpuart3_tx>,
                    <&iomuxc1_gpio_io15_lpuart_rx_lpuart3_rx>;
           bias-pull-up;
           slew-rate = "slightly_fast";
           drive-strength = "x5";
       };
   };
   ```

3. **Wrote a clean `main.cpp` that uses only Zephyr APIs** (`uart_poll_in` / `uart_poll_out`, no `#include "fsl_*.h"`):
   ```c
   static const struct device *const lpuart3 = DEVICE_DT_GET(DT_NODELABEL(lpuart3));
   ```

4. **Updated the in-tree overlay to enable lpuart3**:
   ```dts
   &lpuart3 {
       status = "okay";
       current-speed = <1000000>;
       pinctrl-0 = <&uart3_default>;
       pinctrl-names = "default";
   };
   ```

5. **Build configuration** worked, **link failed** with:
   ```
   undefined reference to `__device_dts_ord_46'
   undefined reference to `__device_dts_ord_45'
   ```
   The `lpuart3` device struct wasn't being instantiated properly — likely because the LPUART2 driver instance ordinals shifted and the mcux_lpuart driver binding didn't recognize the lpuart3 node in the older nxp_zephyr version (nxp-v4.3.0).

## What was reverted

- `src/main.cpp` — restored to the working baremetal version (`md5 1b9f1045a42620b7a3f45d279b8270b5`, saved in `fork-snapshots/working-binaries/main.cpp.working_v48`)
- `boards/imx93_evk_mimx9352_m33.overlay` — restored to the working comment-only version (intentionally empty overlay)

The working build (loopback passes 550+ times at 1 Mbaud with 16-byte pattern) is preserved in:
- `fork-snapshots/working-binaries/zephyr_v48_1mbaud_16byte_loopback.elf` (prebuilt)
- `fork-snapshots/working-binaries/main.cpp.working_v48` (the source that built it)

## What needs to happen for the pure-Zephyr path

**Step 1 — Patch the Zephyr fork first (the blocking dependency).**

In `~/mobturret-forks/zephyr` (a fresh clone of `zephyrproject-rtos/zephyr` main branch), apply these patches to the HAL NXP drivers:

a. **`drivers/clock_control/clock_control_mcux_ccm_rev2.c`** — add support for setting the clock root source/divider in `clock_control_configure()` for i.MX93 LPUART3. Currently the driver only enables the IP gate; the Lpuart3 root stays at reset (OFF), so `clock_control_get_rate()` returns 0 and the SDK's baud search produces garbage.

b. **`drivers/serial/uart_mcux_lpuart.c`** — after `LPUART_Init()`, if running on i.MX93 M33 with a 24 MHz clock and 1 Mbaud target, force `base->BAUD = (23 << 24) | 1`. The SDK's `LPUART_Init` doesn't handle this combination correctly (gives SBR=1028, OSR_field=0, which is ~820 kbaud).

c. **Upstream pinctrl entries** — the `iomuxc1_gpio_io14_lpuart_tx_lpuart3_tx` etc. entries exist in `hal_nxp` but need to be un-omitted in the imx93_evk board pinctrl file.

d. **Add `lpuart3` node to the M33 dtsi** — already done in step 1 of "What was done in this session" above (needs to be redone on the fork branch).

**Step 2 — Use the fork as `ZEPHYR_BASE` for the in-tree build.**

`mcux_include.json` (or the equivalent cmake env) should point to the fork after the patches. Then the in-tree `main.cpp` (the one I wrote, using only Zephyr APIs) should build and run with `lpuart3` fully Zephyr-driven.

**Step 3 — Re-run the loopback test.** With the patches in place, the test should pass with the same 16-byte pattern constraint as the baremetal approach.

## Why the pure-Zephyr path is harder than it looks

Three things need to align:

1. **LPUART2 is the Zephyr console** (`zephyr,console = &lpuart2`). The Zephyr driver instance ordinals are assigned by the DT in declaration order. Adding lpuart3 between lpuart2 and the other devices shifts the ordinals, and the driver instance macros need to find the right ordinal for the mcux_lpuart driver's matching compatible entry.

2. **The `hal_nxp` module** (where the LPUART driver, the clock driver, and the SDK live) is a west-managed submodule. `nxp_zephyr` (the in-tree checkout at `gun_bot/m33_firmware/nxp_zephyr`) doesn't have it unless `west update` was run. Without it, the LPUART driver can't be compiled.

3. **The mcux_lpuart driver** uses the SDK's `LPUART_Init` which has the baud bug. Even if everything else is set up correctly, the baud would still be wrong (~820 kbaud instead of 1 Mbaud) unless the driver is patched to override BAUD.

## Practical recommendation

For the next session, **start by applying the fork patches (Step 1) before touching the in-tree code**. With the fork properly set up, the in-tree `main.cpp` and overlay changes I made should work as-is.

In the meantime, the working baremetal path is preserved and can be deployed via:
```bash
scp gun_bot/m33_firmware/gun_controller/fork-snapshots/working-binaries/zephyr_v48_1mbaud_16byte_loopback.elf \
    root@192.168.1.94:/lib/firmware/zephyr.elf
# (force-stop M33, set firmware, start — see FORK.md)
```

The 550+ loopback PASSes are reproducible from this binary. For the actual servo bus (16-byte FIFO limit), see `ZEPHYR_M33_UART3_SC15SERVO.md` for the DMA / interrupt-driven drain plan.

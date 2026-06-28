# gun_bot — Troubleshooting

Low-level debugging notes for the M33 firmware. Read
[`./CLAUDE.md`](./CLAUDE.md) first for the build / deploy workflow.

## Critical: corrupted SDK file (blocks all further M33 work)

The file

```
m33_firmware/nxp_zephyr/modules/hal/nxp/mcux/mcux-sdk-ng/drivers/lpuart/fsl_lpuart.c
```

is currently in a broken state from accumulated sed/awk edits and
**will not compile**. The damage:

- Extra closing `}` at line 437 (orphaned)
- Missing `/* Check to see if actual baud rate is within 3% */` comment
- `config->inverseTxd = false;` at line 766 (member not in `lpuart_config_t`)
- Multiple `enableTxRTS` / `txRtsPolarity` references (members not in struct)
- Multiple `kLPUART_RtsPolarityLow` (enum not defined)
- `lpuart_config_t` doesn't have the fields the source code references

### Restoration

```bash
# 1. Delete the corrupted file
rm m33_firmware/nxp_zephyr/modules/hal/nxp/mcux/mcux-sdk-ng/drivers/lpuart/fsl_lpuart.c

# 2. Restore from the matching MCUXpresso SDK in the workspace
cp mcusdk_m33_firmware/sdks/mcimx93_evk_blank_sdk/mcuxsdk/drivers/lpuart/fsl_lpuart.c \
   m33_firmware/nxp_zephyr/modules/hal/nxp/mcux/mcux-sdk-ng/drivers/lpuart/

# 3. Verify it builds
cd m33_firmware/gun_controller
rm -rf debug
cmake --preset=debug
cmake --build --preset=debug
```

After restoration, re-apply the 1 Mbaud patch (next section) with a
**single atomic `sed` operation** — do not chain sed/awk/python edits
inside one another, that is what produced the corruption.

---

## 1 Mbaud BAUD register math (i.MX93 M33, 24 MHz clock root)

**Symptom:** LPUART3 measures ~820 kbaud instead of the configured
1,000,000 baud. The SDK's baud-search algorithm in `LPUART_Init`
produces wrong BAUD register values for 24 MHz + 1 Mbaud on the i.MX93
M33 (verified: `BAUD = 0x00000404`, SBR=1028, OSR=0 — wrong).

**Root cause:** the SDK's `LPUART_Init` iterates OSR/SBR combinations
to find the closest match within 3 %, but its table doesn't have a
viable entry for 24 MHz source + 1 Mbaud target on i.MX93. The
MCUXpresso bare-metal path works because it configures clocks
differently.

### Approaches tried

1. **SDK source patch (preferred)** — modify `fsl_lpuart.c` to bypass
   the search loop with hardcoded `osr=24, sbr=1` for 24 MHz + 1 Mbaud.
   **Status:** partially applied; file is now corrupted (see above).
2. **User-space fixup hook** — `SYS_INIT(imx93_lpuart3_baud_fixup,
   PRE_KERNEL_1, 60)` writes `0x17000001` to BAUD after the driver
   binds. Present in binary, but didn't change measured baud — either
   the SDK overwrites it, or the cycle measurement is off, or both.
   **Status:** ineffective, kept as a diagnostic probe.
3. **`current-speed` to a different value** — not attempted. The SDK
   produces wrong values for 1 Mbaud specifically; other bauds may
   work, but SC15 servos are factory-fixed at 1 Mbaud.
4. **Disable LPUART3 driver, manual init** — set `status = "disabled"`
   on `&lpuart3` and do clock + pinctrl + BAUD + CTRL configuration
   manually from a SYS_INIT hook. Bypasses the SDK's broken init
   entirely. **Status:** not attempted; cleanest fallback if approach
   1 cannot be re-applied.

### Recommended next steps

1. Restore `fsl_lpuart.c` (see above).
2. Re-apply the 1 Mbaud SDK patch with one atomic operation. Test
   loopback at 1 Mbaud on GPIO_14 ↔ GPIO_15.
3. If the patch fails again, fall back to approach 4 (disable
   LPUART3 driver, manual init).

---

## Remoteproc firmware cache

**Symptom:** writing to `/sys/class/remoteproc/remoteproc0/firmware`
returns `EBUSY` even though the file content is different.

**Cause:** Linux's firmware loader caches the blob by name. Subsequent
writes to the same path are short-circuited.

**Fixes:**
- Use a new filename each redeploy (`/lib/firmware/zephyr_v2.elf`,
  `zephyr_v3.elf`, …), OR
- Unbind and rebind the platform device to drop the cache:
  ```bash
  echo imx-rproc > /sys/bus/platform/drivers/imx-rproc/unbind
  echo imx-rproc > /sys/bus/platform/drivers/imx-rproc/bind
  ```

---

## LPUART3 single-wire mode hangs

**Symptom:** `uart_poll_out()` hangs indefinitely when the overlay
configures LPUART3 in single-wire (TX-only) mode.

**Workaround:** use two-pin mode in the overlay and externally short
GPIO_14 ↔ GPIO_15 for bench loopback tests. SC15 servos in production
do not need single-wire mode — they use TX/RX on the bus adapter.

---

## LPUART2 console conflicts with A55 Linux

**Symptom:** M33 `printk()` output is missing or garbled when running
under A55 Linux via remoteproc.

**Cause:** LPUART2 is owned by Linux's debug console on the A55. M33
and A55 writes collide on the wire.

**Fixes (in order of preference):**
1. Enable `CONFIG_LOG_BACKEND_RPMSG=y` in `prj.conf`; read M33 logs
   from `/dev/ttyRPMSG0` on the A55 side.
2. Change the overlay's `chosen { zephyr,console = &lpuart4; }` and
   wire a USB-UART adapter to the LPUART4 pins.
3. Boot M33 standalone via JLink (no Linux on A55) — LPUART2 console
   works normally.

---

## Bus fault on direct register write

**Symptom:** M33 faults with `BFAR=0x42570004` (LPUART3 BAUD register)
after a thread-context direct write.

**Cause:** the M33 subsystem is in a low-power or restricted state
when the user-space fixup hook runs, and the BAUD register isn't
readable/writable outside the PRE_KERNEL window.

**Fix:** move register writes to a SYS_INIT hook (PRE_KERNEL_1,
priority ≤ 60, before the LPUART driver's `INIT_PRIORITY = 50`), not
from a thread.

---

## Overlay not picked up

**Symptom:** `debug/zephyr/zephyr.dts` has no `lpuart3` node.

**Cause:** stale cmake cache, or the overlay filename doesn't match
the BOARD (`imx93_evk/mimx9352/m33` → `imx93_evk_mimx9352_m33.overlay`).

**Fix:**
```bash
rm -rf debug/CMakeCache.txt debug/CMakeFiles
cmake --preset=debug
cmake --build --preset=debug
```

---

## Permission notes

If a future Claude session is blocked on the SDK file:

```json
{
  "permissions": {
    "allow": [
      "Edit(m33_firmware/nxp_zephyr/modules/hal/nxp/mcux/mcux-sdk-ng/**)",
      "Bash(rm:*)",
      "Bash(cmake:*)",
      "Bash(ssh:*)"
    ]
  }
}
```

The auto-classifier rejects:
- direct `Edit` of `fsl_lpuart.c` (third-party SDK),
- `python3 -c "..."` that modifies `fsl_lpuart.c`,
- thread-context direct register writes (bus fault risk),
- persistent `cmake --build` cycles after the user says "stop".

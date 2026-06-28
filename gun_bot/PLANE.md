# Gun Controller - Project Checklist

## Stage 1: Hardware Bring-up (Current Stage)
- [x] Basic Zephyr RTOS boots on FRDM-iMX93
- [x] Console/logging via UART works (hello world)
- [x] LED threads running (led0/led1 toggling)
- [x] LPUART3 initialized correctly (Zephyr driver model via `boards/imx93_evk_mimx9352_m33.overlay`)
- [ ] SCSCL servo library integrated
- [ ] Servo ping/identification via LPUART3 (TX path proven on baremetal; Zephyr driver path pending hardware test)
- [ ] Verify servo communication at 115200 baud

### Notes
- Hardware is FRDM-iMX93, but the Zephyr fork does not ship a `frdm_imx93/mimx9352/m33` board port — only `frdm_imx93_mimx9352_a55` and `imx93_evk_mimx9352_m33`. The EVK M33 board port is reused for FRDM hardware (the M33 subsystem is identical), so `CMakePresets.json` targets `imx93_evk/mimx9352/m33` and the app overlay is named accordingly.
- The i.MX93 EVK pinctrl dtsi does not define a `uart3_default` group, so the app overlay defines both the `lpuart3` node and the `uart3_default` pinctrl group.
- `bare_lpuart3.h` was deleted; all UART access now flows through Zephyr's `nxp,imx-lpuart` driver via `uart_poll_out` / `uart_poll_in`.

## Stage 2: Basic Servo Control
- [ ] Single servo position write
- [ ] Single servo position read/feedback
- [ ] Basic servo movement test

## Stage 3: Mechanism Control
- [ ] Trigger servo control (fire mechanism)
- [ ] Dart pusher servo control (feed mechanism)
- [ ] Barrel lock servo control (rotation/selector)
- [ ] Synchronized firing sequence

## Stage 4: Calibration & Tuning
- [ ] Determine servo position ranges for each mechanism
- [ ] Calibrate trigger pull position
- [ ] Calibrate dart pusher stroke length
- [ ] Calibrate barrel lock positions
- [ ] Tune timing between servos

## Stage 5: Testing & Validation
- [ ] Single servo position control test
- [ ] Trigger mechanism bench test
- [ ] Dart pusher mechanism bench test
- [ ] Barrel lock mechanism bench test
- [ ] Full blaster integration test
- [ ] Load testing (multiple rapid shots)

## Documentation
- [x] README.rst - project description
- [x] sample.yaml - test configuration
- [x] CLAUDE.md - build/dev instructions
- [ ] Hardware wiring diagram
- [ ] Servo position calibration values

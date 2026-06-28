.. zephyr:code-sample:: gun_controller
   :name: SC15 Servo NERF Blaster Controller
   :relevant-api: gpio_interface uart_interface thread_apis

   Control a NERF-style blaster using SC15 serial servos via LPUART3.

Overview
********

This project controls a NERF-style blaster mechanism using SCSCL serial servos.
It demonstrates real-time servo control through Zephyr's multi-threading APIs
with precise timing and position control.

The system uses :c:func:`K_THREAD_DEFINE` to define threads at compile time:

- ``servo_ping_thread`` - Sends a sync byte then an SCSCL ping packet to servo ID 1
  over LPUART3, drains any response bytes, and sleeps until the next cycle.

The Zephyr ``nxp,imx-lpuart`` driver handles all UART bring-up: pinmux, IP clock
gating, baud rate, and IRQ wiring are all driven from the device tree.

Each servo is controlled via UART using the SCSCL protocol at 115200 baud.
The SC15 servos provide 12-bit angular resolution (0-4095) with feedback support.

Hardware
********

- **Board**: NXP FRDM-iMX93 (Cortex-M33 core, MIMX9352)
- 3.3V logic compatible (verify servo signal levels)
- SC15 serial servos (or compatible SCSCL protocol servos)
- Servo wiring:
  - Yellow/Orange: Signal (connect to LPUART3 TX on GPIO_IO14)
  - Red: 6-8.4V power (from battery)
  - Black/Brown: Ground

Note on build target
--------------------

This Zephyr fork does not currently ship a ``frdm_imx93/mimx9352/m33`` board
port — only ``frdm_imx93_mimx9352_a55`` and ``imx93_evk_mimx9352_m33``. The
i.MX93 M33 subsystem is identical across the FRDM and EVK boards, so the EVK
board port is reused for FRDM hardware until a dedicated FRDM M33 port is
added. ``BOARD`` in ``CMakePresets.json`` is therefore set to
``imx93_evk/mimx9352/m33``.

LPUART3 Connections
*******************

LPUART3 is mapped via the devicetree overlay at
``boards/imx93_evk_mimx9352_m33.overlay``:

- TX: ``GPIO_IO14`` (routed via ``iomuxc1_gpio_io14_lpuart_tx_lpuart3_tx``)
- RX: ``GPIO_IO15`` (routed via ``iomuxc1_gpio_io15_lpuart_rx_lpuart3_rx``)
- Base: ``0x42570000`` (M33 non-secure alias)
- IRQ: NVIC #68
- Clock root: ``Lpuart3``, sourced from Osc24M with div=1

Building
********

To build for the i.MX93 M33 core on FRDM-iMX93 hardware:

.. zephyr-app-commands::
   :zephyr-app: m33_firmware/gun_controller
   :board: imx93_evk/mimx9352/m33
   :goals: build flash
   :compact:

The build reads the application overlay from
``boards/imx93_evk_mimx9352_m33.overlay`` (Zephyr resolves it by matching the
board name with ``/`` replaced by ``_``).

Communication Protocol
*********************

The SCSCL protocol uses 8N1 UART at 115200 baud:

- Position write: 0xFF 0xFE [ID] [Cmd] [Addr] [Len] [Data] [Checksum]
- Position read: 0xFF 0xFE [ID] [Cmd] [Addr] [Len] [Checksum]
- Ping: 0xFF 0xFE [ID] 0x03 [Addr] [Len] [Checksum]

Position range: 0-4095 (approximately 0-240 degrees depending on servo model)

Project Layout
**************

::

  gun_controller/
  ├── boards/
  │   └── imx93_evk_mimx9352_m33.overlay   # LPUART3 DT bindings
  ├── src/
  │   ├── main.cpp                         # servo_ping_thread
  │   └── servo/                           # SCSCL protocol library
  │       ├── SCSerial.cpp / .h
  │       ├── SCSCL.cpp / .h
  │       ├── SCS.cpp / .h
  │       └── SMS_STS.cpp / .h
  ├── prj.conf                             # CONFIG_SERIAL, CONFIG_UART_MCUX_LPUART
  ├── CMakeLists.txt
  └── README.rst

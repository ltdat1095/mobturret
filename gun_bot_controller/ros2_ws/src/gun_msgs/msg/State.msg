# State — current operational state of the turret.
#
# Published at 1 Hz on /gun/state. Mirrors IPC.md MSG_TELEMETRY's
# non-servo fields plus a few local bookkeeping flags.

gun_msgs/Mode mode                  # current operating mode
bool   last_command_acked           # was the most recent command accepted?
uint8  ack_status                   # last MSG_ACK.status (0 = OK)
uint16 last_alert_code              # last MSG_ALERT.code, 0 if none
uint8  error_flags                  # bitfield (see below)

# error_flags bit assignments
uint8 ERROR_NONE             = 0
uint8 ERROR_ESTOP_ACTIVE     = 1   # bit 0 — emergency stop latched
uint8 ERROR_SAFE_POSE        = 2   # bit 1 — servos in safe pose due to fault
uint8 ERROR_RPMSG_DOWN       = 4   # bit 2 — RPMsg link to M33 is down
uint8 ERROR_WATCHDOG         = 8   # bit 3 — host heartbeat missed
uint8 ERROR_HARDWARE_FAULT   = 16  # bit 4 — generic hardware fault

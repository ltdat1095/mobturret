# Telemetry — full health snapshot published at 10 Hz on /gun/telemetry.
#
# Combines the IPC.md MSG_TELEMETRY payload fields with a per-servo
# diagnostics vector. cloud_bridge relays this to the server.

int32  yaw_pos_centideg     # current yaw position, centi-deg
int32  pitch_pos_centideg   # current pitch position, centi-deg
uint16 batt_mv              # battery voltage, mV (system-level, not servo bus)
int8   temp_c               # M33 die temperature, °C
gun_msgs/ServoDiag[] servos # per-servo diagnostics; empty if no servos detected

# ServoDiag — per-servo health snapshot.
#
# One instance per servo on the SC15 bus. gun_controller publishes a
# fresh vector on every /gun/telemetry message (10 Hz).

uint8  servo_id         # 1..254 (SCSCL ID)
uint16 position         # raw SCSCL units, 0–4095
uint16 load             # 0–1000 (SCSCL convention, ≈ percent × 10)
uint16 voltage_mv       # servo bus voltage, mV
int8   temp_c           # servo die temperature, °C

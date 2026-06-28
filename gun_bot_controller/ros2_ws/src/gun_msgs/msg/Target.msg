# Target — what we want the turret to aim at.
#
# Angles are in centi-degrees (1/100 of a degree) so a float64 keeps
# sub-degree precision without forcing float→int conversion at the
# gun_controller boundary. gun_controller converts to i16 centi-deg
# before sending IPC.md MSG_SET_TARGET.
#
# Range: ±32767 centi-deg = ±327.67°. SC15 servos are mechanically
# limited to ~240°, so practical targets are well inside this range.

float64 yaw_centideg      # target yaw, centi-deg
float64 pitch_centideg    # target pitch, centi-deg

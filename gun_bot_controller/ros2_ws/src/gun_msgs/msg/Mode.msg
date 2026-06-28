# Mode — operating mode for the turret.
#
# Matches IPC.md MSG_MODE payload byte. gun_controller passes the
# `mode` field through to MSG_MODE unchanged.

uint8 MODE_MANUAL=0
uint8 MODE_AUTO=1
uint8 MODE_SAFE=2

uint8 mode

# Fire — trigger one shot.
#
# Edge-triggered at the gun_controller layer: each incoming message
# fires exactly once. Re-publishing a message with the same content
# does NOT fire again unless a different Fire message arrives in
# between (gun_controller keeps a one-deep edge detector).
#
# Matches IPC.md MSG_FIRE payload. gun_controller enforces the 250 ms
# hardware cooldown regardless of cadence and replies with MSG_ACK
# { status = ACK_BUSY } if a second fire arrives too soon.

uint16 duration_ms    # how long to hold the trigger, ms (50–500 typical)
uint8  pressure       # 0–100, fed to the relay/MOSFET PWM duty cycle

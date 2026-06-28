#!/usr/bin/env python3
"""A55 serial helper for /a55-wifi skill.

Opens the FRDM-iMX93 USB serial console (/dev/ttyACM0 @ 115200),
optionally logs in as root, then runs commands read from stdin and
streams the device's output to stdout.

Used by SKILL.md to script a series of shell commands without
needing `expect` (which is not installed on this dev box).

Typical use:

    echo 'cat /root/network.sh' | sudo python3 serial_io.py --login

Or pipe a heredoc:

    sudo python3 serial_io.py --login <<'EOF'
    cat > /etc/systemd/system/wifi-connect.service <<'UNIT'
    [Unit]
    Description=Enable wifi on boot
    [Service]
    Type=oneshot
    ExecStart=/root/network.sh
    RemainAfterExit=yes
    [Install]
    WantedBy=multi-user.target
    UNIT
    systemctl daemon-reload
    systemctl enable wifi-connect.service
    systemctl start wifi-connect.service
    ip -br addr show wlan0
    EOF

The script:
  * waits for `login:` (when --login is set), sends `root\n`
  * then for each line of stdin, sends `<line>\n` to the device
  * after stdin EOF, drains the device for `--settle` seconds
  * returns the device's exit code (the script's, not ours)

Why `sudo`: the dev user is not in `dialout`, so /dev/ttyACM0
needs root. The user's global ~/.claude/settings.json allows
`Bash(*)` — sudo must be NOPASSWD for this to run unattended.
"""

from __future__ import annotations

import argparse
import re
import sys
import time

import serial

LOGIN_RE = re.compile(rb"[Ll]ogin:\s*$")
PROMPT_RE = re.compile(rb"(?:#|~\#|root@[^:]+:[^#]*#)\s*$")


def _drain(ser: serial.Serial, max_ms: int) -> bytes:
    """Read from `ser` until `max_ms` of quiet (no new bytes)."""
    buf = bytearray()
    deadline = time.monotonic() + max_ms / 1000
    while time.monotonic() < deadline:
        n = ser.in_waiting or 1
        try:
            chunk = ser.read(n)
        except serial.SerialException:
            break
        if chunk:
            buf.extend(chunk)
            deadline = time.monotonic() + max_ms / 1000
        else:
            time.sleep(0.05)
    return bytes(buf)


def _wait_for(ser: serial.Serial, pattern: re.Pattern, timeout: float) -> bytes:
    """Read until `pattern` matches somewhere in the accumulated buffer."""
    buf = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        n = ser.in_waiting or 64
        try:
            chunk = ser.read(n)
        except serial.SerialException as e:
            raise TimeoutError(f"serial read failed: {e}") from e
        if chunk:
            buf.extend(chunk)
            if pattern.search(buf):
                return bytes(buf)
        else:
            time.sleep(0.05)
    raise TimeoutError(
        f"timed out after {timeout:.1f}s waiting for {pattern.pattern!r}; "
        f"received: {bytes(buf)!r}"
    )


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    p.add_argument("--device", default="/dev/ttyACM0",
                   help="serial device (default: /dev/ttyACM0)")
    p.add_argument("--baud", type=int, default=115200,
                   help="baud rate (default: 115200)")
    p.add_argument("--login", action="store_true",
                   help="wait for 'login:' prompt, then send 'root'")
    p.add_argument("--login-timeout", type=float, default=15.0)
    p.add_argument("--settle", type=float, default=4.0,
                   help="seconds to keep reading after stdin EOF (default: 4)")
    p.add_argument("--read-only", action="store_true",
                   help="don't send stdin — just open, optionally log in, "
                        "drain, and print")
    args = p.parse_args()

    try:
        ser = serial.Serial(args.device, args.baud, timeout=0.5)
    except (serial.SerialException, FileNotFoundError) as e:
        print(f"[serial_io] cannot open {args.device}: {e}", file=sys.stderr)
        return 2

    try:
        # Discard any leftover output from a previous session.
        _drain(ser, max_ms=500)

        if args.login:
            try:
                _wait_for(ser, LOGIN_RE, timeout=args.login_timeout)
            except TimeoutError as e:
                print(f"[serial_io] {e}", file=sys.stderr)
                return 3
            ser.write(b"root\n")
            ser.flush()
            # Wait for the first shell prompt so we know we're logged in.
            try:
                _wait_for(ser, PROMPT_RE, timeout=args.login_timeout)
            except TimeoutError as e:
                print(f"[serial_io] {e}", file=sys.stderr)
                return 4
            # Print the boot/login banner so the operator can see what
            # state the device was in.
            sys.stdout.flush()

        if args.read_only:
            sys.stdout.buffer.write(_drain(ser, max_ms=2000))
            sys.stdout.flush()
            return 0

        # Stream stdin line-by-line to the device.
        for line in sys.stdin:
            # Strip the trailing newline; we'll add our own.
            ser.write(line.rstrip("\n").encode("utf-8", errors="replace") + b"\n")
            ser.flush()
            # Brief settle per command so the reply doesn't bleed into
            # the next command's output.
            sys.stdout.buffer.write(_drain(ser, max_ms=600))
            sys.stdout.flush()

        # Final drain — catch any output produced after the last command.
        sys.stdout.buffer.write(_drain(ser, max_ms=int(args.settle * 1000)))
        sys.stdout.flush()
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
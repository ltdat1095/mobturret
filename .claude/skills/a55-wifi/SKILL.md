---
name: a55-wifi
description: Bootstrap / restore wifi on the FRDM-iMX93 A55 Linux board. Connects over the USB serial console (/dev/ttyACM0 @ 115200), captures or re-applies /root/network.sh, and installs a systemd service (wifi-connect.service) so wifi comes up automatically on every boot. Use after a reflash when /root/network.sh and the service are both gone, or on first bring-up.
allowed-tools:
  - Bash
  - Read
  - Write
  - Edit
---

# /a55-wifi — A55 wifi bootstrap

The A55 Linux image on the FRDM-iMX93 starts with wifi down. The
existing bring-up ritual is to plug in USB, open `screen
/dev/ttyACM0 115200`, log in as root, and run `/root/network.sh`.
This skill automates that ritual and makes it survive reflash:

1. **Capture** `/root/network.sh` content via serial (first run)
2. **Save** it into Claude memory so it's not lost next reflash
3. **Install** a systemd `wifi-connect.service` so the script runs
   on every boot — no more manual intervention after reboot
4. **Verify** `wlan0` got an IP

The skill is **idempotent**: invoking it on a board that already has
the service deployed will just re-verify and report. Invoking it on
a fresh post-reflash board will read what's on disk, save it to
memory (if not already there), and re-deploy the service.

## Memory layout

Captured script lives at:

```
~/.claude/projects/-home-ltdat-Desktop-mobturret/memory/a55-network-sh.md
```

with frontmatter (`type: project`) and the script body in a fenced
code block. Index entry lives in the sibling `MEMORY.md`.

If that file is missing, the skill is in **capture mode**.
If it exists, the skill is in **reapply mode**.

## Steps

### 1. Decide mode

```bash
SKILL_DIR="$(git rev-parse --show-toplevel)/.claude/skills/a55-wifi"
MEMORY=~/.claude/projects/-home-ltdat-Desktop-mobturret/memory/a55-network-sh.md

test -f "$MEMORY" \
  && echo MEMORY_HIT_REAPPLY \
  || echo MEMORY_MISS_CAPTURE
```

- `MEMORY_HIT_REAPPLY` → skip to step 3
- `MEMORY_MISS_CAPTURE` → continue to step 2

### 2. Capture /root/network.sh (memory miss only)

```bash
sudo python3 "$SKILL_DIR/serial_io.py" --login <<'EOF'
cat /root/network.sh
echo __EOF_MARKER__
EOF
```

Look at the output. Strip the leading login banner, prompt, and
`cat` echo; everything between the `cat /root/network.sh` echo and
the next `#` prompt is the script body. The `__EOF_MARKER__` line
helps you delimit it precisely.

Save the script body to:

`~/.claude/projects/-home-ltdat-Desktop-mobturret/memory/a55-network-sh.md`

with this frontmatter:

```markdown
---
name: a55-network-sh
description: Contents of /root/network.sh on the FRDM-iMX93 A55 Linux image. Captured YYYY-MM-DD via /a55-wifi. Lost on every reflash; redeployed via the /a55-wifi skill.
metadata:
  type: project
---

## Why this exists

`/root/network.sh` lives on the FRDM-iMX93 A55 and is wiped on every
reflash. We back it up here so future sessions can redeploy it via
the `/a55-wifi` skill without needing a manual `screen` session.

## Script contents (captured YYYY-MM-DD)

\`\`\`sh
<paste verbatim here>
\`\`\`

## How to redeploy

Run `/a55-wifi`. It detects this memory file and skips the capture
phase, going straight to deploy.
```

Then add a one-line entry to `MEMORY.md`:

```
- [A55 network.sh contents](a55-network-sh.md) — captured script, lost on reflash; redeploy via /a55-wifi
```

### 3. Deploy (always run)

The `SKILL_DIR` variable is already set in step 1.

```bash
# 3a. Extract just the bash body from the memory file (between the
#     opening ```sh and closing ``` fences).
SCRIPT_BODY=$(awk '/^```sh$/{flag=1; next} /^```$/{flag=0} flag' "$MEMORY")

# 3b. Push to /root/network.sh via a heredoc.
sudo python3 "$SKILL_DIR/serial_io.py" --login <<EOF
cat > /root/network.sh <<'NETSH'
$SCRIPT_BODY
NETSH
chmod +x /root/network.sh
ls -l /root/network.sh
EOF

# 3c. Install + enable the systemd unit.
sudo python3 "$SKILL_DIR/serial_io.py" <<'EOF'
cat > /etc/systemd/system/wifi-connect.service <<'UNIT'
[Unit]
Description=Enable wifi on boot (runs /root/network.sh)
After=network-pre.target
Before=network.target

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
systemctl is-enabled wifi-connect.service
EOF

# 3d. Verify wifi came up.
sudo python3 "$SKILL_DIR/serial_io.py" <<'EOF'
sleep 2
ip -br addr show wlan0
EOF
```

### 4. Report

End the run with a short status block:

```
A55 wifi bootstrap: <MODE>
  /root/network.sh    : <size> bytes, mode 0755
  wifi-connect.service: <enabled | already-enabled>
  wlan0               : <ip-addr-or-empty>
  memory file         : <path>
```

Where `<MODE>` is `CAPTURE+DEPLOY` (memory was missing) or
`REDEPLOY` (memory existed).

## Notes / failure modes

- **`sudo` asks for a password.** The script uses `sudo` because the
  user is not in `dialout`. If sudo isn't NOPASSWD, set it up first:
  `echo "$(whoami) ALL=(ALL) NOPASSWD: /usr/bin/python3" | sudo tee /etc/sudoers.d/serial_io`
  (the global `~/.claude/settings.json` allows `Bash(*)`, so this
  one-time setup itself runs unattended).
- **`/dev/ttyACM0` doesn't appear.** USB enumeration sometimes takes
  a few seconds after the A55 comes up. `dmesg -w` on the dev box
  to watch for `cdc_acm`.
- **The A55 image uses a different `login:` banner.** If `--login`
  times out, look at the raw output with
  `sudo python3 "$SKILL_DIR/serial_io.py" --read-only` and adjust
  the `LOGIN_RE` regex in `serial_io.py`.
- **Heredoc inside heredoc.** The skill uses quoted heredocs
  (`<<'NETSH'`, `<<'UNIT'`) so the shell doesn't try to expand
  `$VARS` from the script body. Don't drop the quotes.
- **The script body may itself contain heredocs.** Same rule — the
  outer heredoc must be quoted (`<<'NETSH'`) so the inner `<<EOF`
  passes through verbatim.
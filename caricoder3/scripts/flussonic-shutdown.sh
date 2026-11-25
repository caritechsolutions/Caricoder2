#!/bin/bash
# Flussonic off for good, once its streams run on caricoder3; then checked,
# not assumed: nothing of it running, listening or receiving.
#   curl -fsSL .../scripts/flussonic-shutdown.sh | sudo bash
#   (... | sudo DRY=1 bash: only the checks, nothing stopped)
# Stops and disables the flussonic service (its files and config stay), then:
#   - no flussonic process left
#   - no socket of it, TCP or UDP (listening, connected, or a multicast input)
#   - caricoder3's channels, each running or not
# Undo: systemctl enable --now flussonic
set -u
DRY=${DRY:-0}
CFG=${CC3_CONFIG:-/etc/caricoder3/caricoder3.json}
say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
bad=0

MYPGID=$(ps -o pgid= -p $$ | tr -d ' ')
procs() {   # flussonic's processes: the unit's own, and anything named after it (not this script's own)
    local c p
    c=$({
        systemctl show flussonic -p MainPID --value 2>/dev/null | grep -E '^[1-9][0-9]*$'
        systemctl show flussonic -p ControlGroup --value 2>/dev/null | while read -r cg; do
            [ -n "$cg" ] && cat "/sys/fs/cgroup$cg/cgroup.procs" 2>/dev/null
        done
        pgrep -f '[f]lussonic' 2>/dev/null
    } | grep -E '^[0-9]+$' | sort -un)
    for p in $c; do
        [ -d "/proc/$p" ] || continue
        [ "$(ps -o pgid= -p "$p" 2>/dev/null | tr -d ' ')" = "$MYPGID" ] && continue
        echo "$p"
    done
}

sockets() {   # every TCP/UDP socket the given processes hold, read from /proc (no ss needed)
    python3 - "$@" <<'PY'
import os, socket, struct, sys
def addr(h, v6):
    ip, port = h.split(":")
    b = bytes.fromhex(ip)
    if v6:
        b = b"".join(struct.pack("<I", struct.unpack(">I", b[i:i + 4])[0]) for i in range(0, 16, 4))
        return f"[{socket.inet_ntop(socket.AF_INET6, b)}]:{int(port, 16)}"
    return f"{socket.inet_ntoa(struct.pack('<I', int(ip, 16)))}:{int(port, 16)}"
STATES = {"01": "connected", "0A": "listening"}
table = {}
for proto in ("tcp", "tcp6", "udp", "udp6"):
    try:
        for line in open(f"/proc/net/{proto}").readlines()[1:]:
            f = line.split()
            st = "open" if proto.startswith("udp") else STATES.get(f[3], "state " + f[3])
            table[f[9]] = f"{proto[:3]} {st} {addr(f[1], proto.endswith('6'))} -> {addr(f[2], proto.endswith('6'))}"
    except OSError:
        pass
for pid in sys.argv[1:]:
    try:
        fds = os.listdir(f"/proc/{pid}/fd")
    except OSError:
        continue
    for fd in fds:
        try:
            l = os.readlink(f"/proc/{pid}/fd/{fd}")
        except OSError:
            continue
        if l.startswith("socket:["):
            ino = l[8:-1]
            if ino in table:
                print(f"    pid {pid}: {table[ino]}")
PY
}

say "flussonic now: $(systemctl is-active flussonic 2>/dev/null || true), $(systemctl is-enabled flussonic 2>/dev/null || true)"
before=$(procs | tr '\n' ' ')
note "processes: ${before:-none}"

if [ "$DRY" = 0 ]; then
    say "stopping and disabling flussonic"
    systemctl stop flussonic 2>&1 | sed 's/^/    /'
    systemctl disable flussonic 2>&1 | sed 's/^/    /'
    for _ in $(seq 1 20); do
        [ -z "$(procs)" ] && break
        sleep 1
    done
else
    say "DRY RUN: nothing stopped"
fi

say "check: the service"
st=$(systemctl is-active flussonic 2>/dev/null || true)
en=$(systemctl is-enabled flussonic 2>/dev/null || true)
note "active: ${st:-unknown}, at boot: ${en:-unknown}"
if [ "$DRY" = 0 ] && { [ "$st" = active ] || [ "$en" = enabled ]; }; then
    note "NOT off"
    bad=1
fi

say "check: processes"
left=$(procs | tr '\n' ' ')
if [ -n "$left" ]; then
    note "still running:"
    for p in $left; do note "  $(ps -o pid=,args= -p "$p" 2>/dev/null | cut -c1-150)"; done
    [ "$DRY" = 0 ] && bad=1
else
    note "none"
fi

say "check: sockets (listening, connected, multicast inputs)"
socks=""
[ -n "$left" ] && socks=$(sockets $left)
if [ -n "$socks" ]; then
    echo "$socks" | head -40
    [ "$DRY" = 0 ] && bad=1
else
    note "none"
fi

say "check: caricoder3's channels"
python3 - "$CFG" <<'PY' || true
import base64, json, sys, urllib.request
cfg = json.load(open(sys.argv[1]))
host = cfg.get("listen", "0.0.0.0")
host = "127.0.0.1" if host in ("0.0.0.0", "", "::") else host
auth = {"Authorization": "Basic " + base64.b64encode(f"admin:{cfg.get('password', '')}".encode()).decode()}
try:
    with urllib.request.urlopen(urllib.request.Request(f"http://{host}:{cfg.get('port', 8550)}/api/channels", headers=auth), timeout=10) as f:
        chans = json.load(f)["channels"]
except Exception as e:
    print(f"    caricoder3 did not answer: {e}")
    sys.exit(0)
on = [c for c in chans if c.get("enabled")]
print(f"    {len(on)} of {len(chans)} switched on")
for c in sorted(chans, key=lambda c: c["name"]):
    print(f"    {c['name']:18} {'on ' if c.get('enabled') else 'off'} {c.get('state', '?')}")
PY

if [ "$DRY" != 0 ]; then
    say "dry run done: the above is how it is now; nothing was stopped"
elif [ "$bad" = 0 ]; then
    say "done: flussonic is off and nothing of it is left"
    note "to undo: systemctl enable --now flussonic"
else
    say "NOT clean: see above"
fi
exit $bad

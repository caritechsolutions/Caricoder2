#!/bin/bash
# Which PIDs of a UDP feed have continuity-counter trouble, and of what kind
# (read only: listens for a few seconds).
#   curl -fsSL .../scripts/input-cc.sh | sudo bash -s -- udp://224.2.2.2:3030 [SECONDS]
# For each PID: packets, repeats (the same counter again: "same" when the
# packet is byte for byte the one before, which is legal once; "changed" when
# it is not), and gaps (packets missing). The channel's own count ("CC errors")
# covers the streams it carries and the tables (PAT, PMT, SDT) and the PCR.
set -u
URL=${1:-}; SECS=${2:-10}
[ -n "$URL" ] || { echo "usage: ... | sudo bash -s -- udp://ADDRESS:PORT [SECONDS]" >&2; exit 2; }
exec python3 - "$URL" "$SECS" <<'PY'
import re, socket, struct, sys, time
m = re.match(r"(?:udp|rtp)://@?([\d.]+):(\d+)", sys.argv[1])
if not m:
    raise SystemExit("the address must be udp://A.B.C.D:PORT")
addr, port, secs = m.group(1), int(m.group(2)), float(sys.argv[2])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
s.bind((addr if int(addr.split(".")[0]) >= 224 else "", port))
if int(addr.split(".")[0]) >= 224:
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, struct.pack("4s4s", socket.inet_aton(addr), socket.inet_aton("0.0.0.0")))
s.settimeout(1.0)
st, senders, t_end = {}, {}, time.time() + secs
while time.time() < t_end:
    try:
        d, who = s.recvfrom(65536)
    except socket.timeout:
        continue
    senders[who[0]] = senders.get(who[0], 0) + 1
    if len(d) % 188 and len(d) > 12 and (len(d) - 12) % 188 == 0:
        d = d[12:]                                  # RTP
    for i in range(0, len(d) - 187, 188):
        p = d[i:i + 188]
        if p[0] != 0x47:
            continue
        pid = (p[1] & 0x1F) << 8 | p[2]
        if pid == 0x1FFF:
            continue
        x = st.setdefault(pid, {"n": 0, "same": 0, "same_run": 0, "changed": 0, "gaps": 0, "last": None, "run": 0})
        x["n"] += 1
        if not p[3] & 0x10:                         # no payload: the counter stands still
            continue
        cc, last = p[3] & 0x0F, x["last"]
        if last is not None:
            lcc = last[3] & 0x0F
            if cc == lcc:
                if p == last:
                    x["same"] += 1
                    x["run"] += 1
                    x["same_run"] = max(x["same_run"], x["run"])
                else:
                    x["changed"] += 1
                    x["run"] = 0
            else:
                x["run"] = 0
                if cc != (lcc + 1) & 0x0F:
                    x["gaps"] += 1
        x["last"] = p
names = {0: "PAT", 1: "CAT", 16: "NIT", 17: "SDT", 18: "EIT", 20: "TDT"}
print(f"{sys.argv[1]}, {secs:.0f} s; from {', '.join(f'{k} ({v} datagrams)' for k, v in senders.items()) or 'nobody'}")
print(f"{'PID':>6}  {'':4} {'packets':>8} {'repeats, same':>14} {'longest run':>12} {'repeats, changed':>17} {'gaps':>6}")
for pid in sorted(st):
    x = st[pid]
    print(f"{pid:>6}  {names.get(pid, ''):4} {x['n']:>8} {x['same']:>14} {x['same_run']:>12} {x['changed']:>17} {x['gaps']:>6}")
PY

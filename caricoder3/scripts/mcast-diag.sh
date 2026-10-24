#!/bin/bash
# Why channels do not receive their multicast input (read only).
#   curl -fsSL .../scripts/mcast-diag.sh | sudo bash
# Writes /tmp/caricoder3-mcast-diag.txt: how this boot went (network and
# caricoder3), which interface each group is joined on, the multicast
# routes, the sockets, what the channels report, and whether the network
# has an IGMP querier (listens 130 s for its queries).
OUT=/tmp/caricoder3-mcast-diag.txt
CFG=${CC3_CONFIG:-/etc/caricoder3/caricoder3.json}
exec > "$OUT" 2>&1
sec() { echo; echo "===== $* ====="; }

sec "now"; date; uptime
sec "this boot: network and caricoder3 (first 80 lines)"
journalctl -b -o short-precise --no-pager 2>/dev/null |
    grep -E "Link UP|link becomes ready|NIC Link is Up|carrier|Gained carrier|network-online|systemd-networkd-wait-online|caricoder3 supervisor|caricoder3\[.*(listening|started|serve)" | head -80
sec "caricoder3 service"; systemctl show caricoder3 -p ActiveEnterTimestamp -p NRestarts -p After 2>/dev/null
sec "wait-online"; systemctl status systemd-networkd-wait-online.service NetworkManager-wait-online.service --no-pager 2>/dev/null | head -20
sec "interfaces"; ip -br link; ip -br addr
sec "routes for multicast"; ip route show | grep -E "^(default|224|239|multicast)" ; for g in 224.100.1.5 224.100.1.6 224.2.2.2 224.2.2.3 224.5.5.5 224.10.10.10; do echo -n "$g -> "; ip route get "$g" 2>&1 | head -1; done
sec "groups joined, per interface (ip maddr)"; ip maddr show
sec "/proc/net/igmp"; cat /proc/net/igmp
sec "IGMP settings"; for f in /proc/sys/net/ipv4/conf/*/force_igmp_version; do echo "$f $(cat "$f")"; done; sysctl net.ipv4.igmp_max_memberships 2>/dev/null
sec "caricoder3 UDP sockets"; ss -uapn 2>/dev/null | grep -E "caricoder3" | head -60
sec "channel inputs (configs)"; grep -h '"input"' /etc/caricoder3/channels/*.json 2>/dev/null | sed 's/^ *//' | sort | uniq -c | head -60
sec "what the channels report"
python3 - "$CFG" <<'PY'
import base64, json, sys, urllib.request
c = json.load(open(sys.argv[1]))
host = c.get("listen", "0.0.0.0"); host = "127.0.0.1" if host in ("0.0.0.0", "", "::") else host
req = urllib.request.Request(f"http://{host}:{c.get('port', 8550)}/api/channels")
if c.get("password"):
    req.add_header("Authorization", "Basic " + base64.b64encode(f"admin:{c['password']}".encode()).decode())
try:
    j = json.load(urllib.request.urlopen(req, timeout=10))
except Exception as e:
    sys.exit(f"API: {e}")
for ch in j.get("channels", []):
    st = ch.get("stats") or {}
    i = st.get("input") or {}
    print(f"{ch.get('name'):22} {ch.get('state'):9} in {i.get('bitrate', '-')!s:>9} b/s  last packet {i.get('last_packet_age_ms', '-')!s:>8} ms ago"
          f"  cc {i.get('cc_errors', '-')}  {i.get('url') or ch.get('input', '')}")
PY
sec "IGMP queries on the network (130 s)"
if command -v tcpdump >/dev/null; then
    for i in $(ip -br link | awk '$1 != "lo" && $2 == "UP" {print $1}'); do
        echo "-- $i"; timeout 130 tcpdump -nni "$i" -c 20 igmp 2>&1 | grep -E "query|report|packets captured" &
    done; wait
else
    echo "tcpdump not installed"
fi
echo; echo "done: $OUT"

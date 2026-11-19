#!/bin/bash
# What Flussonic on this server is doing right now, before it is replaced
# (read only: nothing is started, stopped or changed).
#   curl -fsSL .../scripts/flussonic-audit.sh | sudo bash
# Writes /tmp/flussonic-audit.txt:
#   - the service, its version, its processes
#   - the ports it listens on, and every connection into them (who pulls
#     from it: their address and port), its UDP sockets (inputs it reads)
#   - its own view of each stream (alive, bit rate in, clients, pushes)
#     and of each client session, from its API on this machine
#   - the multicast groups it pushes to, and any that a caricoder3
#     channel also sends to
# The API login is read from Flussonic's config here and not written out.
OUT=/tmp/flussonic-audit.txt
FCONF=${FLUSSONIC_CONF:-/etc/flussonic/flussonic.conf}
exec 3>&2 > "$OUT" 2>&1
sec() { echo; echo "===== $* ====="; }

sec "now"; date; uptime
sec "service"
systemctl status flussonic --no-pager 2>/dev/null | head -12
dpkg -l 2>/dev/null | grep -i flussonic
MAIN=$(systemctl show flussonic -p MainPID --value 2>/dev/null)
PIDS=""
if [ -n "$MAIN" ] && [ "$MAIN" != "0" ]; then
    PIDS="$MAIN $(pgrep -P "$MAIN" | tr '\n' ' ')"
    # the Erlang VM's own children
    for p in $PIDS; do PIDS="$PIDS $(pgrep -P "$p" | tr '\n' ' ')"; done
fi
PIDS=$(echo "$PIDS" | tr ' ' '\n' | grep -E '^[0-9]+$' | sort -u | tr '\n' ' ')
echo "processes: ${PIDS:-none found}"
for p in $PIDS; do ps -o pid=,etime=,rss=,args= -p "$p" 2>/dev/null | cut -c1-200; done
PAT=$(for p in $PIDS; do printf 'pid=%s,|' "$p"; done | sed 's/|$//')

sec "TCP ports it listens on"
[ -n "$PAT" ] && ss -tlnp 2>/dev/null | grep -E "$PAT"
sec "TCP connections into it (who is connected; local -> peer)"
if [ -n "$PAT" ]; then
    ss -tnp state established 2>/dev/null | grep -E "$PAT" | awk '{print $3, "<->", $4}' | sort | uniq -c | sort -rn | head -80
fi
sec "UDP sockets (multicast inputs, SRT)"
[ -n "$PAT" ] && ss -uanp 2>/dev/null | grep -E "$PAT" | awk '{print $4, $5}' | sort | uniq -c | head -80
sec "multicast groups joined by anyone on this machine (ip maddr)"
ip maddr show 2>/dev/null | grep -E "^[0-9]|inet " | head -80

sec "Flussonic's own view: streams and sessions"
python3 - "$FCONF" <<'PY'
import base64, json, re, sys, urllib.request
conf = open(sys.argv[1]).read() if sys.argv[1] else ""
m = re.search(r"^\s*edit_auth\s+(\S+)\s+(\S+?);", conf, re.M)
port = re.search(r"^\s*http\s+(\d+)\s*;", conf, re.M)
port = port.group(1) if port else "80"
auth = {"Authorization": "Basic " + base64.b64encode(f"{m.group(1)}:{m.group(2)}".encode()).decode()} if m else {}
print(f"API on 127.0.0.1:{port}, login {'from the config' if m else 'not found in the config'}")

def get(path):
    try:
        r = urllib.request.Request(f"http://127.0.0.1:{port}{path}", headers=auth)
        with urllib.request.urlopen(r, timeout=10) as f:
            return json.load(f)
    except Exception as e:
        return {"__error": str(e)}

def dig(d, *keys, default=None):
    for k in keys:
        if not isinstance(d, dict) or k not in d:
            return default
        d = d[k]
    return d

streams, used = None, None
for path in ("/streamer/api/v3/streams?limit=500", "/flussonic/api/v3/streams?limit=500", "/flussonic/api/media"):
    j = get(path)
    if "__error" in j:
        print(f"{path}: {j['__error']}")
        continue
    streams, used = j, path
    break
if streams is None:
    sys.exit(0)
print(f"answered: {used}")
items = streams.get("streams") if isinstance(streams, dict) else streams
if not isinstance(items, list):
    print(json.dumps(streams)[:3000])
    sys.exit(0)
print(f"{len(items)} stream(s)\n")
print(f"{'stream':22} {'alive':5} {'in kb/s':>8} {'clients':>7}  pushes / disabled")
for s in items:
    name = s.get("name") or dig(s, "entry", "name") or "?"
    st = s.get("stats") or s.get("entry") or {}
    alive = st.get("alive", dig(st, "status") )
    kbps = dig(st, "input", "bitrate") or st.get("input_bitrate") or st.get("bitrate") or 0
    try:
        kbps = int(kbps)
    except (TypeError, ValueError):
        kbps = 0
    clients = st.get("online_clients", st.get("client_count", st.get("clients", "")))
    pushes = []
    for p in s.get("pushes") or dig(s, "stats", "push") or []:
        if isinstance(p, dict):
            pushes.append(f"{p.get('url', '?')}[{dig(p, 'stats', 'status') or p.get('status', '')}]")
    dis = s.get("disabled", dig(s, "config_on_disk", "disabled"))
    print(f"{name[:22]:22} {str(alive)[:5]:5} {kbps:8} {str(clients):>7}  {' '.join(pushes)[:160]} {'DISABLED' if dis else ''}")
print("\nfirst stream as the API gives it (to read the fields above right):")
print(json.dumps(items[0], indent=1)[:2500] if items else "-")

for path in ("/streamer/api/v3/sessions?limit=500", "/flussonic/api/v3/sessions?limit=500", "/flussonic/api/sessions"):
    j = get(path)
    if "__error" in j:
        print(f"\n{path}: {j['__error']}")
        continue
    ss = j.get("sessions") if isinstance(j, dict) else j
    print(f"\nclient sessions ({path}): {len(ss) if isinstance(ss, list) else '?'}")
    for x in (ss or [])[:200]:
        if isinstance(x, dict):
            print(f"  {str(x.get('name') or x.get('media') or x.get('stream') or '?'):22} {str(x.get('ip') or x.get('remote_ip') or '?'):16} "
                  f"{str(x.get('proto') or x.get('protocol') or x.get('type') or ''):8} since {x.get('opened_at') or x.get('created_at') or ''}")
    break
PY

sec "Flussonic's pushes (from its config) and caricoder3 outputs to the same address"
python3 - "$FCONF" <<'PY'
import glob, json, re, sys
conf = open(sys.argv[1]).read()
push = {}
for blk in re.finditer(r"stream\s+(\S+)\s*\{(.*?)\n\}", conf, re.S):
    name, body = blk.group(1), blk.group(2)
    off = re.search(r"^\s*disabled\s*(true)?\s*;", body, re.M) is not None
    for m in re.finditer(r"^\s*push\s+(\S+)", body, re.M):
        u = m.group(1).rstrip(";")
        hp = re.match(r"(?:udp|srt)://([^/?;]+)", u)
        push.setdefault(hp.group(1) if hp else u, []).append(name + (" (disabled)" if off else ""))
ours = {}
for f in glob.glob("/etc/caricoder3/channels/*.outputs.json"):
    ch = f.rsplit("/", 1)[1].replace(".outputs.json", "")
    try:
        j = json.load(open(f))
    except Exception:
        continue
    for o in (j.get("outputs") if isinstance(j, dict) else j) or []:
        u = o.get("url", "") if isinstance(o, dict) else str(o)
        hp = re.match(r"(?:udp|rtp|srt|rist)://@?([^/?]+)", u)
        if hp:
            ours.setdefault(hp.group(1), []).append(ch)
for addr in sorted(push):
    both = ours.get(addr)
    print(f"{addr:28} flussonic: {', '.join(push[addr])}" + (f"   <-- ALSO caricoder3: {', '.join(both)}" if both else ""))
PY
echo
echo "done: $OUT"
echo "written: $OUT (send it to Claude)" >&3

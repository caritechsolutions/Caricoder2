#!/bin/bash
# Why a channel's output does not play (read only).
#   curl -fsSL .../scripts/channel-check.sh | sudo bash -s -- NAME
# Prints: the channel's state and last log lines; what its output really
# carries, read here (programme, PIDs, codecs, picture, rates, errors);
# Flussonic's identifiers for the same stream if a copy was kept
# (flussonic-pids.sh); and which network port the output leaves by.
set -u
NAME=${1:-}
[ -n "$NAME" ] || { echo "usage: ... | sudo bash -s -- CHANNEL" >&2; exit 2; }
CFG=${CC3_CONFIG:-/etc/caricoder3/caricoder3.json}
BIN=${CC3_BIN:-/usr/local/bin/caricoder3}
STATE=${CC3_STATE:-/var/lib/caricoder3}
exec python3 - "$CFG" "$BIN" "$STATE" "$NAME" <<'PY'
import base64, json, os, re, subprocess, sys, urllib.request

cfg_path, binary, state, name = sys.argv[1:5]
cfg = json.load(open(cfg_path))
host = cfg.get("listen", "0.0.0.0")
host = "127.0.0.1" if host in ("0.0.0.0", "", "::") else host
base = f"http://{host}:{cfg.get('port', 8550)}"
auth = {"Authorization": "Basic " + base64.b64encode(f"admin:{cfg.get('password', '')}".encode()).decode()}

def api(path, raw=False):
    with urllib.request.urlopen(urllib.request.Request(base + path, headers=auth), timeout=20) as f:
        return f.read().decode(errors="replace") if raw else json.load(f)

def run(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True).stdout.strip()
    except FileNotFoundError:
        return f"({cmd[0]} is not on this machine)"

def sec(t):
    print(f"\n===== {t} =====", flush=True)

ch = api(f"/api/channels/{name}")
ch = ch.get("channel") or ch
conf = ch.get("config") or {}
sec("channel")
print("state:", ch.get("state"), "| enabled:", ch.get("enabled"), "| up", ch.get("uptime"), "s | restarts", ch.get("restarts"))
print("config:", json.dumps(conf))
st = ch.get("stats") or {}
for k in ("input", "output"):
    if isinstance(st.get(k), dict):
        print(f"{k}:", json.dumps({x: st[k][x] for x in list(st[k])[:14]}))
outs = st.get("outputs") or ch.get("outputs") or []
print("outputs:", json.dumps(outs)[:600])
sec("last log lines")
try:
    log = api(f"/api/channels/{name}/log", raw=True)
    try:
        j = json.loads(log)
        lines = j.get("log") or j.get("lines") or []
    except ValueError:
        lines = log.splitlines()
    print("\n".join(str(l) for l in lines[-40:]))
except Exception as e:
    print("log not available:", e)

urls = []
try:
    urls = [o["url"] for o in json.load(open(os.path.join(cfg.get("channels_dir", "/etc/caricoder3/channels"), name + ".outputs.json")))["outputs"]]
except Exception as e:
    print("outputs file:", e)
for u in urls:
    sec(f"what {u} carries, read on this server (5 s)")
    p = subprocess.run([binary, "probe", u, "--seconds", "5"], capture_output=True, text=True)
    try:
        j = json.loads(p.stdout)
    except ValueError:
        print(p.stdout[-800:], p.stderr[-400:])
        continue
    if not j.get("ok"):
        print("NOTHING:", j.get("error"))
        continue
    print(f"bit rate {j.get('bitrate', 0) / 1e6:.2f} Mb/s, TS id {j.get('ts_id')}, sync errors {j.get('sync_errors')}")
    for g in j.get("programs", []):
        print(f"program {g['number']}, PMT {g['pmt_pid']} (seen: {g.get('pmt_seen')}), PCR {g['pcr_pid']}, name {g.get('name')!r}, scrambled {g.get('scrambled')}")
        for s in g.get("streams", []):
            extra = {k: s[k] for k in ("width", "height", "interlaced", "fps", "profile", "sample_rate", "channels") if k in s}
            print(f"   {s['pid']:5} {s.get('type_name', '?'):22} {s.get('language', ''):4} {s.get('bitrate', 0) / 1000:7.0f} kb/s  cc errors {s.get('cc_errors')}  {extra}")
    print("PIDs outside any programme:", j.get("other_pids"))
    m = re.match(r"udp://@?([0-9.]+)", u)
    if m:
        sec(f"which port {m.group(1)} leaves by")
        print(run(["ip", "route", "get", m.group(1)]))

copy = os.path.join(state, "flussonic-outputs", name + ".http.json")
if os.path.exists(copy):
    sec("Flussonic's same stream (the copy kept by flussonic-pids.sh)")
    j = json.load(open(copy))
    print(f"bit rate {j.get('bitrate', 0) / 1e6:.2f} Mb/s, TS id {j.get('ts_id')}")
    for g in j.get("programs", []):
        print(f"program {g['number']}, PMT {g['pmt_pid']}, PCR {g['pcr_pid']}, name {g.get('name')!r}")
        for s in g.get("streams", []):
            extra = {k: s[k] for k in ("width", "height", "interlaced", "fps", "profile", "sample_rate", "channels") if k in s}
            print(f"   {s['pid']:5} {s.get('type_name', '?'):22} {s.get('language', ''):4} {s.get('bitrate', 0) / 1000:7.0f} kb/s  {extra}")
sec("interfaces")
print(run(["ip", "-br", "addr"]))
PY

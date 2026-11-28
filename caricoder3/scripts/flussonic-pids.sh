#!/bin/bash
# Gives each migrated channel the same output identifiers Flussonic's
# stream has: programme number, PMT PID, PCR PID, stream PIDs, the order
# of the streams in the PMT, the TS id and the service name. So whatever
# receives the multicast sees no change when caricoder3 takes over.
#   curl -fsSL .../scripts/flussonic-pids.sh | sudo bash
#   (... | sudo DRY=1 bash: show, change nothing)
#   ... | sudo bash -s -- NAME [NAME..] [--start]: only those channels;
#   --start also switches each matched channel on (refused while Flussonic
#   still sends that stream: switch it off there first)
#
# Flussonic's identifiers are kept in /var/lib/caricoder3/flussonic-outputs/
# NAME.http.json whenever they are read, so a stream already switched off
# in Flussonic (an SRT source takes one connection: the channel's source
# can be read only then) is matched against the copy.
#
# For each caricoder3 channel that Flussonic has a stream of the same name:
#   - Flussonic's output is read from its own HTTP port on this machine
#     (http://127.0.0.1:8080/NAME/mpegts): a few seconds, read only
#   - the channel's source is read a few seconds (caricoder3 probe)
#   - streams are matched kind by kind: video to video, audio by language
#     where that decides it, otherwise in order; subtitles, teletext and
#     data in order. A source stream Flussonic did not carry is left out.
#   - the channel's config gets "remap", "order", "drop", "service_id",
#     "pmt_pid", "pcr_pid", "ts_id", "service_name". A running channel
#     would restart; switched off, it just keeps them for when it starts.
# A channel whose streams do not match one for one is left as it was,
# with the reason. Flussonic is only read.
set -u
CFG=${CC3_CONFIG:-/etc/caricoder3/caricoder3.json}
BIN=${CC3_BIN:-/usr/local/bin/caricoder3}
FHTTP=${FLUSSONIC_HTTP:-http://127.0.0.1:8080}
PROBE_S=${PROBE_S:-4}
DRY=${DRY:-0}
[ -r "$CFG" ] || { echo "no caricoder3 config at $CFG" >&2; exit 1; }
STATE=${CC3_STATE:-/var/lib/caricoder3}
exec python3 - "$CFG" "$BIN" "$FHTTP" "$PROBE_S" "$DRY" "$STATE" "$@" <<'PY'
import base64, json, subprocess, sys, urllib.error, urllib.request

import os, time
cfg_path, binary, fhttp, probe_s, dry, state = sys.argv[1:7]
args = sys.argv[7:]
start = "--start" in args
only = [a for a in args if not a.startswith("--")]
cache = os.path.join(state, "flussonic-outputs")
os.makedirs(cache, exist_ok=True)
dry = dry not in ("", "0")
cfg = json.load(open(cfg_path))
host = cfg.get("listen", "0.0.0.0")
host = "127.0.0.1" if host in ("0.0.0.0", "", "::") else host
base = f"http://{host}:{cfg.get('port', 8550)}"
auth = {"Authorization": "Basic " + base64.b64encode(f"admin:{cfg.get('password', '')}".encode()).decode()}

def say(m):
    print(f"\033[1m==>\033[0m {m}", flush=True)

def note(m):
    print(f"    {m}", flush=True)

def api(path, method="GET", body=None):
    r = urllib.request.Request(base + path, method=method, headers=dict(auth, **{"Content-Type": "application/json"}),
                               data=json.dumps(body).encode() if body is not None else None)
    with urllib.request.urlopen(r, timeout=20) as f:
        return json.load(f)

def probe(url):
    p = subprocess.run([binary, "probe", url, "--seconds", str(probe_s)], capture_output=True, text=True, timeout=60)
    try:
        j = json.loads(p.stdout)
    except ValueError:
        return None, (p.stderr or p.stdout).strip()[-160:] or "no answer"
    progs = [g for g in j.get("programs", []) if g.get("streams")]
    if not j.get("ok") or not progs:
        return None, j.get("error") or "no programme with streams"
    return (j, progs), None

KINDS = ("video", "audio", "subtitles", "teletext", "data")

def match(src, out):
    """[(src_pid, out_pid)], [src pids left out], problem or None"""
    pairs, left = [], []
    for k in KINDS:
        s = [x for x in src if x.get("kind") == k]
        o = [x for x in out if x.get("kind") == k]
        if len(o) > len(s):
            return None, None, f"Flussonic has {len(o)} {k} stream(s), the source {len(s)}"
        if k == "audio":
            # by language first, where a language names exactly one stream on each side
            for x in list(o):
                lang = x.get("language") or ""
                cs = [y for y in s if (y.get("language") or "") == lang]
                co = [y for y in o if (y.get("language") or "") == lang]
                if lang and len(cs) == 1 and len(co) == 1:
                    pairs.append((cs[0]["pid"], x["pid"]))
                    s.remove(cs[0])
                    o.remove(x)
        for x in o:
            pairs.append((s.pop(0)["pid"], x["pid"]))
        left += [y["pid"] for y in s]
    return pairs, left, None

def http_answer(url):
    """what a web server says to a GET, in words"""
    try:
        with urllib.request.urlopen(urllib.request.Request(url), timeout=5) as f:
            return f"HTTP {f.status}, {f.headers.get('Content-Type', '?')}"
    except urllib.error.HTTPError as e:
        return f"HTTP {e.code} {e.reason}"
    except Exception as e:
        return f"no answer ({e})"

def clean(name):
    """a service name without the DVB character-table byte in front"""
    return (name or "").lstrip("".join(chr(c) for c in range(0x20))).strip()

chans = api("/api/channels")["channels"]
if only:
    chans = [c for c in chans if c["name"] in only]
    missing = set(only) - {c["name"] for c in chans}
    if missing:
        raise SystemExit(f"no such caricoder3 channel: {', '.join(sorted(missing))}")
say(f"{len(chans)} channel(s); Flussonic read at {fhttp}/NAME/mpegts" + ("  (DRY RUN: nothing changed)" if dry else "") +
    ("; matched ones are switched on" if start and not dry else ""))
for ch in chans:
    name = ch["name"]
    full = api(f"/api/channels/{name}")
    conf = (full.get("channel") or full).get("config") or {}
    say(name)
    # FLUSSONIC_REF_URL (one channel): what Flussonic sends is read there instead,
    # e.g. the multicast it pushes to. That is what the receivers get, and for a
    # stream Flussonic passes through untouched it is not what its HTTP port gives.
    ref_url = os.environ.get("FLUSSONIC_REF_URL") if len(chans) == 1 else None
    fr, err = probe(ref_url or f"{fhttp}/{name}/mpegts")
    if ref_url and fr:
        note(f"Flussonic's identifiers read from what it sends to {ref_url}")
    cpath = os.path.join(cache, name + ".http.json")
    flussonic_live = bool(fr)
    if fr:
        json.dump(fr[0], open(cpath, "w"), indent=1)
    elif os.path.exists(cpath):
        fr = (json.load(open(cpath)), None)
        fr = (fr[0], [g for g in fr[0].get("programs", []) if g.get("streams")])
        note("Flussonic no longer sends it: its identifiers from the copy taken " +
             time.strftime("%Y-%m-%d %H:%M", time.localtime(os.path.getmtime(cpath))))
    else:
        note(f"Flussonic's output could not be read ({err}) and there is no copy of it. Left as it was.")
        continue
    fj, fprogs = fr
    fp = fprogs[0]
    inp = conf.get("input", "")
    sr, err = probe(inp)
    if not sr:
        why = err
        if inp.startswith("http://"):
            why += f"; the server answers: {http_answer(inp)}"
        if inp.startswith("srt://") and flussonic_live:
            why += f". Flussonic holds this SRT source: switch {name} off in Flussonic, then run this for {name} again"
        note(f"the source could not be read ({inp}): {why}. Left as it was.")
        continue
    sj, sprogs = sr
    want = conf.get("program") or 0
    sp = next((g for g in sprogs if g.get("number") == want), sprogs[0])
    keep = set(conf.get("pids") or [])
    drop0 = set(conf.get("drop") or [])
    src = [x for x in sp["streams"] if (not keep or x["pid"] in keep) and x["pid"] not in drop0]
    fs = fp["streams"]
    note("Flussonic: program %s, PMT %s, PCR %s, TS id %s: %s" % (fp.get("number"), fp.get("pmt_pid"), fp.get("pcr_pid"), fj.get("ts_id"),
         ", ".join(f"{x['pid']} {x['kind']}{' ' + x['language'] if x.get('language') else ''}" for x in fs)))
    note("source:    program %s: %s" % (sp.get("number"), ", ".join(f"{x['pid']} {x['kind']}{' ' + x['language'] if x.get('language') else ''}" for x in src)))
    pairs, left, problem = match(src, fs)
    if problem:
        note(f"no one-for-one match ({problem}). Left as it was.")
        continue
    new = dict(conf)
    remap = {str(a): b for a, b in pairs if a != b}
    for k in ("remap", "order", "drop", "service_id", "pmt_pid", "pcr_pid", "ts_id", "service_name"):
        new.pop(k, None)
    if remap:
        new["remap"] = remap
    by_out = {b: a for a, b in pairs}
    new["order"] = [by_out[x["pid"]] for x in fs]
    if left:
        new["drop"] = sorted(drop0 | set(left))
    elif drop0:
        new["drop"] = sorted(drop0)
    new["service_id"] = fp.get("number")
    new["pmt_pid"] = fp.get("pmt_pid")
    if fp.get("pcr_pid") and fp["pcr_pid"] != 0x1FFF:
        new["pcr_pid"] = fp["pcr_pid"]
    if fj.get("ts_id"):
        new["ts_id"] = fj["ts_id"]
    # the name only when Flussonic's differs from the source's own (which goes out as it is)
    if clean(fp.get("name")) and clean(fp.get("name")) != clean(sp.get("name")):
        new["service_name"] = clean(fp["name"])
    note("set: " + ", ".join(f"{a}->{b}" for a, b in pairs) + (f"; left out {left}" if left else "") +
         f"; program {new['service_id']}, PMT {new['pmt_pid']}, PCR {new.get('pcr_pid', 'as is')}, TS id {new.get('ts_id', 'as is')}" +
         (f", name {new['service_name']!r}" if new.get("service_name") else ""))
    if dry:
        continue
    start_it = start and not flussonic_live
    if start_it:
        new["enabled"] = True
    api(f"/api/channels/{name}", "PUT", new)
    if start and flussonic_live:
        note(f"saved, NOT switched on: Flussonic still sends {name} (two senders on one address). Switch it off there first.")
    else:
        note("saved and switched on" if start_it else "saved")
say("done" + ("  (dry run)" if dry else ""))
PY

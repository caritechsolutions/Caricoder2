#!/bin/bash
# One Flussonic stream at a time, recreated as a caricoder3 channel on the
# same server: its settings are read from Flussonic's own config file, the
# channel is created switched OFF with the same source, program, treatment
# and outputs, and it is given the identifiers Flussonic's output has
# (scripts/flussonic-pids.sh). Flussonic is only read, never changed (except by --move).
#
#   curl -fsSL .../scripts/flussonic-channel.sh | sudo bash -s -- --list
#       every stream in Flussonic's config: can it be moved by this
#       script, and if not, why
#   curl -fsSL .../scripts/flussonic-channel.sh | sudo bash -s -- NAME [NAME..]
#       create each channel, switched off, identifiers copied
#   (then switch the stream off in Flussonic)
#   curl -fsSL .../scripts/flussonic-channel.sh | sudo bash -s -- NAME --start
#       switch the channel on (refused while Flussonic still sends it)
#   ... | sudo DRY=1 bash -s -- NAME        show, change nothing
#   curl -fsSL .../scripts/flussonic-channel.sh | sudo bash -s -- NAME [NAME..] --move
#       the whole move, one channel at a time: the channel is created (or
#       is there already), then the stream is switched off in Flussonic's
#       config file ("disabled;", Flussonic told to reload), the channel
#       switched on, and what it sends is listened to: the program number,
#       PMT PID, every PID, kind and codec must be what Flussonic sent,
#       with data on every video and audio stream and no CC errors. Only
#       then the next channel. One that fails is put back (channel off,
#       stream on again in Flussonic) and the run stops there. The config
#       file is copied first (the copy's name is printed).
#
# What it moves: a stream that takes its source from the network (udp://,
# mpts-udp://, rtp://, srt://, http(s):// TS), copies or re-encodes to at
# most one video rendition, and sends to udp:// addresses. Anything else
# (an input from another Flussonic stream, HLS or playlist inputs, a
# ladder of renditions, RTMP/SRT/HLS pushes, a stream another Flussonic
# stream reads) is refused with the reason and left for a later step.
#
# A channel that already exists in caricoder3 keeps its settings: only
# the identifiers are copied again (and --start applies).
set -u
CFG=${CC3_CONFIG:-/etc/caricoder3/caricoder3.json}
FCONF=${FLUSSONIC_CONF:-/etc/flussonic/flussonic.conf}
PIDS=${CC3_PIDS_SCRIPT:-/opt/caricoder3/scripts/flussonic-pids.sh}
DRY=${DRY:-0}
BIN=${CC3_BIN:-/usr/local/bin/caricoder3}
STATE=${CC3_STATE:-/var/lib/caricoder3}
RELOAD=${FLUSSONIC_RELOAD:-service flussonic reload}
[ -r "$CFG" ] || { echo "no caricoder3 config at $CFG" >&2; exit 1; }
[ -r "$FCONF" ] || { echo "no Flussonic config at $FCONF" >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 is needed" >&2; exit 1; }
exec python3 - "$CFG" "$FCONF" "$PIDS" "$DRY" "$BIN" "$STATE" "$RELOAD" "$@" <<'PY'
import base64, json, os, re, shutil, subprocess, sys, time, urllib.error, urllib.request

cfg_path, fconf, pids_script, dry, binary, state_dir, reload_cmd = sys.argv[1:8]
args = sys.argv[8:]
move = "--move" in args
start = "--start" in args
listing = "--list" in args
names = [a for a in args if not a.startswith("--")]
dry = dry not in ("", "0")
cfg = json.load(open(cfg_path))
chdir = cfg.get("channels_dir", "/etc/caricoder3/channels")
host = cfg.get("listen", "0.0.0.0")
host = "127.0.0.1" if host in ("0.0.0.0", "", "::") else host
base = f"http://{host}:{cfg.get('port', 8550)}"
auth = {"Authorization": "Basic " + base64.b64encode(f"admin:{cfg.get('password', '')}".encode()).decode()}

def say(m):
    print(f"\033[1m==>\033[0m {m}", flush=True)

def note(m):
    print(f"    {m}", flush=True)

def chan(name):
    """the channel as the supervisor has it: state, enabled, config, ..."""
    j = api(f"/api/channels/{name}")
    return j.get("channel") or j

def api(path, method="GET", body=None):
    r = urllib.request.Request(base + path, method=method, headers=dict(auth, **{"Content-Type": "application/json"}),
                               data=json.dumps(body).encode() if body is not None else None)
    try:
        with urllib.request.urlopen(r, timeout=20) as f:
            return json.load(f)
    except urllib.error.HTTPError as e:
        raise SystemExit(f"ERROR: {method} {path}: {e.code} {e.read().decode(errors='replace')[:200]}")
    except OSError as e:
        raise SystemExit(f"ERROR: caricoder3 does not answer at {base} ({e}). Is it running? systemctl status caricoder3")

# ---- Flussonic's config: "stream NAME { key rest; ... }", and "http PORT;" at the top ----
def read_conf(path):
    streams, http_port, cur = {}, None, None
    for raw in open(path, errors="replace"):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if cur is None:
            m = re.match(r"(stream|ondemand|live|template)\s+(\S+)\s*\{$", line)
            if m:
                cur = {"type": m.group(1), "name": m.group(2), "lines": [], "depth": 1}
                continue
            m = re.match(r"http\s+(?:\S+:)?(\d+)\s*;", line)
            if m and http_port is None:
                http_port = int(m.group(1))
            continue
        # nested blocks (a transcoder or dvr written with braces) are kept as one line each
        cur["depth"] += line.count("{") - line.count("}")
        if cur["depth"] <= 0:
            if cur["type"] == "stream":
                streams[cur["name"]] = cur["lines"]
            cur = None
            continue
        cur["lines"].append(line.rstrip(";").strip())
    return streams, http_port

NET = ("udp://", "mpts-udp://", "rtp://", "srt://", "http://", "https://")
LOCAL = re.compile(r"^(?:tshttp|http|https|m4f|m4s|hls|rtmp|rtsp)://(?:127\.0\.0\.1|localhost)(?::\d+)?/([^/\s?]+)")

def plan(name, lines, streams):
    """(channel config, outputs, notes) or (None, None, [why not])"""
    conf, outs, notes, why = {"enabled": False}, [], [], []
    inputs, tracks, glob, left = [], [], {}, []
    disabled = False
    for ln in lines:
        key, _, rest = ln.partition(" ")
        rest = rest.strip()
        if key == "disabled":
            disabled = rest != "false"
        elif key in ("input", "url"):
            inputs.append(rest.split())
        elif key == "push":
            outs.append(rest.split()[0])
        elif key == "transcoder":
            cur = None
            for tok in rest.split():
                k, _, v = tok.partition("=")
                if k == "vb":
                    cur = {"vb": v}
                    tracks.append(cur)
                elif cur is not None and k in ("vcodec", "size", "preset", "profile", "level", "fps", "bframes",
                                               "refs", "gop", "deinterlace", "sar", "logo", "crop", "rc_method",
                                               "interlace", "open_gop", "temporal_aq", "spatial_aq", "lookahead"):
                    cur[k] = v
                else:
                    glob[k] = v
        else:
            left.append(ln)
    if disabled:
        why.append("it is switched off in Flussonic")
    if not inputs:
        why.append("it has no input")
    urls = []
    for toks in inputs:
        url, opts = toks[0], dict(t.partition("=")[::2] for t in toks[1:])
        m = LOCAL.match(url)
        if m and m.group(1) in streams:
            why.append(f"its input is another Flussonic stream ({m.group(1)}): the two move together, as one channel")
            continue
        if not url.startswith(NET):
            why.append(f"an input of a kind this script does not move ({url.split('?')[0][:60]})")
            continue
        if url.endswith((".m3u8", ".mpd")) or "/index.m3u8" in url or "playlist" in url:
            why.append(f"an HLS or playlist input ({url.split('?')[0][:60]})")
            continue
        url = url.replace("mpts-udp://", "udp://", 1)
        if not urls:
            if opts.get("programs"):
                if "," in opts["programs"]:
                    why.append(f"several programs from one input ({opts['programs']})")
                else:
                    conf["program"] = int(opts["programs"])
            if opts.get("pids"):
                conf["pids"] = [int(p) for p in opts["pids"].split(",") if p.isdigit()]
            for k in opts:
                if k not in ("programs", "pids", "priority", "source_timeout"):
                    notes.append(f"input option not carried over: {k}={opts[k]}")
        urls.append(url)
    if urls:
        conf["input"] = urls[0]
        if urls[1:]:
            conf["backup_inputs"] = urls[1:]
    # who reads this stream inside Flussonic
    readers = sorted(n for n, ls in streams.items() if n != name and
                     not any(l.split()[:1] == ["disabled"] and l.strip() != "disabled false" for l in ls) and
                     any(l.startswith(("input ", "url ")) and (m := LOCAL.match(l.split()[1])) and m.group(1) == name for l in ls))
    if readers:
        why.append(f"Flussonic's {', '.join(readers)} read{'s' if len(readers) == 1 else ''} it from Flussonic: "
                   "switching it off there would stop " + ("it" if len(readers) == 1 else "them"))
    # outputs
    bad = [o for o in outs if not o.startswith(("udp://", "rtp://"))]
    if bad:
        why.append("it pushes to something other than UDP (" + ", ".join(o.split("://")[0] for o in bad) + ")")
    if not outs:
        why.append("it pushes nowhere: whatever uses it pulls it from Flussonic, which caricoder3 does not offer yet")
    # video
    real = [t for t in tracks if t["vb"] != "copy"]
    if len(real) > 1:
        why.append(f"it makes {len(real)} video renditions (a ladder)")
    elif len(real) == 1:
        t = real[0]
        codec = {"hevc": "hevc", "h265": "hevc", "h264": "h264", None: "h264"}.get(t.get("vcodec"))
        if not codec:
            why.append(f"a video codec this script does not know ({t.get('vcodec')})")
        else:
            # Flussonic's size: WxH[:fit|:scale|:crop][:#background]. With one side
            # -1 the picture keeps its shape anyway; a full box with "fit" is ours too.
            size = None
            if t.get("size"):
                parts = t["size"].split(":")
                size = parts[0]
                mode = [x for x in parts[1:] if not x.startswith("#")]
                if "fit" in mode and "-1" not in size.split("x"):
                    size += ":fit"
                for x in mode:
                    if x != "fit":
                        notes.append(f"size option not carried over: {x}")
            conf["video"] = [":".join(x for x in (size, codec, t["vb"]) if x)]
        if glob.get("hw") == "nvenc":
            conf["gpu"] = int(glob.get("deviceid", "0")) if glob.get("deviceid", "0").isdigit() else 0
        elif glob.get("hw") in (None, "cpu"):
            conf["encoder"] = "cpu"
            conf["decoder"] = "cpu"
        else:
            why.append(f"a kind of encoder this script does not know (hw={glob.get('hw')})")
        for k, v in t.items():
            if k not in ("vb", "vcodec", "size"):
                notes.append(f"video option not carried over: {k}={v}")
    # audio
    ab, ac = glob.get("ab"), glob.get("acodec")
    if ab and ab != "copy":
        a = {"mp2a": "mp2", "mp2": "mp2", "aac": "aac", "mp3": "mp3", None: "aac"}.get(ac)
        if not a:
            why.append(f"an audio codec this script does not know ({ac})")
        else:
            conf["audio"] = [f"{a}:{ab}"]
    for k, v in glob.items():
        if k not in ("ab", "acodec", "hw", "deviceid", "external"):
            notes.append(f"transcoder option not carried over: {k}={v}")
    for ln in left:
        notes.append(f"not carried over: {ln}")
    if why:
        return None, None, why
    return conf, [{"id": f"out{i + 1}", "url": u} for i, u in enumerate(outs)], notes

streams, http_port = read_conf(fconf)
if not streams:
    raise SystemExit(f"ERROR: no streams found in {fconf}")

if listing:
    ok = no = 0
    say(f"{len(streams)} stream(s) in {fconf}")
    for name in sorted(streams):
        conf, outs, notes = plan(name, streams[name], streams)
        if conf:
            ok += 1
            note(f"{name:24} can be moved: {conf['input']}" + (f" program {conf['program']}" if conf.get("program") else "") +
                 f" -> {', '.join(o['url'] for o in outs) or 'no output'}")
        else:
            no += 1
            note(f"{name:24} not by this script: {'; '.join(notes)}")
    say(f"{ok} can be moved by this script, {no} cannot (yet)")
    raise SystemExit(0)

if not names:
    raise SystemExit("usage: flussonic-channel.sh --list | NAME [NAME..] [--start]")

have = {c["name"] for c in api("/api/channels")["channels"]}
done = []
for name in names:
    say(name)
    if name not in streams:
        note(f"Flussonic's config has no stream of that name ({fconf}). Nothing done.")
        continue
    if name in have:
        # switched off: brought up to what this script would make now (its identifiers
        # kept); running: left alone
        full = chan(name)
        old = full.get("config") or {}
        conf, outs, notes = plan(name, streams[name], streams)
        changed = []
        if conf and not old.get("enabled", True):
            new = dict(old)
            for k in ("input", "backup_inputs", "program", "pids", "video", "audio", "gpu", "encoder", "decoder"):
                if conf.get(k) != old.get(k):
                    changed.append(f"{k} {old.get(k)!r} -> {conf.get(k)!r}")
                    if k in conf:
                        new[k] = conf[k]
                    else:
                        new.pop(k, None)
            if changed and not dry:
                api(f"/api/channels/{name}", "PUT", new)
        note("caricoder3 already has this channel" + (": settings corrected: " + "; ".join(changed) if changed
             else ": its settings are left as they are"))
        done.append(name)
        continue
    conf, outs, notes = plan(name, streams[name], streams)
    if not conf:
        note("NOT moved: " + "; ".join(notes))
        continue
    note(f"source   {conf['input']}" + (f", program {conf['program']}" if conf.get("program") else "") +
         (f", PIDs {conf['pids']}" if conf.get("pids") else "") +
         (f", then {', '.join(conf['backup_inputs'])}" if conf.get("backup_inputs") else ""))
    note(f"video    {conf['video'][0] if conf.get('video') else 'copied'}" +
         (f" on card {conf['gpu']}" if "gpu" in conf else " on the CPU" if conf.get("encoder") == "cpu" else ""))
    note(f"audio    {conf['audio'][0] if conf.get('audio') else 'copied'}")
    note(f"outputs  {', '.join(o['url'] for o in outs) or 'none'}")
    for n in notes:
        note(f"note     {n}")
    if dry:
        note("DRY RUN: not created")
        continue
    if outs:
        with open(os.path.join(chdir, name + ".outputs.json"), "w") as f:
            json.dump({"outputs": outs}, f, indent=2)
    api(f"/api/channels/{name}", "PUT", conf)
    note("created, switched off")
    done.append(name)

if not done:
    raise SystemExit(0 if dry else 1)

# ---- --move: one channel at a time, Flussonic's stream off, ours on, checked ----
def run_pids(which, extra=()):
    env = dict(os.environ, CC3_CONFIG=cfg_path, CC3_BIN=binary, CC3_STATE=state_dir, DRY="1" if dry else "0")
    env.setdefault("FLUSSONIC_HTTP", f"http://127.0.0.1:{http_port or 80}")
    sys.stdout.flush()
    return subprocess.run(["bash", pids_script, *which, *extra], env=env).returncode

def set_disabled(name, line):
    """Writes the stream's "disabled" line in Flussonic's config (None: no such line).
    Returns the line that was there (None: there was none)."""
    text = open(fconf, errors="surrogateescape").read().split("\n")
    at = next((i for i, l in enumerate(text) if re.match(r"\s*stream\s+" + re.escape(name) + r"\s*\{\s*$", l)), None)
    if at is None:
        raise SystemExit(f"ERROR: stream {name} is not in {fconf} any more")
    end = next(i for i in range(at + 1, len(text)) if text[i].strip() == "}")
    was = next((i for i in range(at + 1, end) if re.match(r"\s*disabled(\s+\w+)?\s*;\s*$", text[i])), None)
    old = text[was].strip() if was is not None else None
    if was is not None and line is None:
        del text[was]
    elif was is not None:
        text[was] = "  " + line
    elif line is not None:
        text.insert(at + 1, "  " + line)
    tmp = fconf + ".cc3-new"
    with open(tmp, "w", errors="surrogateescape") as f:
        f.write("\n".join(text))
    st = os.stat(fconf)
    os.chmod(tmp, st.st_mode & 0o7777)
    try:
        os.chown(tmp, st.st_uid, st.st_gid)
    except OSError:
        pass
    os.replace(tmp, fconf)
    return old

def reload_flussonic():
    p = subprocess.run(reload_cmd, shell=True, capture_output=True, text=True)
    if p.returncode != 0:
        note(f"'{reload_cmd}' answered {p.returncode}: {(p.stderr or p.stdout).strip()[-200:]}")
    return p.returncode == 0

def flussonic_sends(name):
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{http_port or 80}/{name}/mpegts", timeout=3) as f:
            return len(f.read(188 * 7)) > 0
    except Exception:
        return False

def probe(url, seconds=4):
    p = subprocess.run([binary, "probe", url, "--seconds", str(seconds)], capture_output=True, text=True, timeout=60)
    try:
        return json.loads(p.stdout)
    except ValueError:
        return {"ok": False, "error": (p.stderr or p.stdout).strip()[-160:] or "no answer"}

def check(name, conf, out_url):
    """None when what the channel sends is right, else what is wrong."""
    st = chan(name)
    if st.get("state") != "running":
        return f"the channel is {st.get('state')}" + (f" ({st['why']})" if st.get("why") else "")
    j = probe(out_url)
    progs = [g for g in j.get("programs", []) if g.get("streams")]
    if not j.get("ok") or not progs:
        return f"nothing usable at {out_url} ({j.get('error') or 'no programme'})"
    want_n = conf.get("service_id") or conf.get("program")
    pg = next((g for g in progs if g.get("number") == want_n), None)
    if not pg:
        return f"program {want_n} is not there (found {[g.get('number') for g in progs]})"
    if conf.get("pmt_pid") and pg.get("pmt_pid") != conf["pmt_pid"]:
        return f"PMT on PID {pg.get('pmt_pid')}, Flussonic had {conf['pmt_pid']}"
    try:
        ref = json.load(open(os.path.join(state_dir, "flussonic-outputs", name + ".http.json")))
        ref = [g for g in ref.get("programs", []) if g.get("streams")][0]["streams"]
    except (OSError, ValueError, IndexError):
        return "the copy of Flussonic's identifiers is missing"
    got = {x["pid"]: x for x in pg["streams"]}
    for r in ref:
        g = got.get(r["pid"])
        if not g:
            return f"PID {r['pid']} ({r.get('kind')}) is missing"
        if g.get("kind") != r.get("kind"):
            return f"PID {r['pid']} is {g.get('kind')}, Flussonic had {r.get('kind')}"
        if r.get("codec") and g.get("codec") and g["codec"] != r["codec"]:
            return f"PID {r['pid']} is {g['codec']}, Flussonic had {r['codec']}"
    extra = sorted(set(got) - {r["pid"] for r in ref})
    if extra:
        return f"PIDs Flussonic did not send: {extra}"
    for x in pg["streams"]:
        if x.get("kind") in ("video", "audio") and not x.get("bitrate"):
            return f"no data on PID {x['pid']} ({x['kind']})"
        if x.get("cc_errors"):
            return f"{x['cc_errors']} CC errors on PID {x['pid']} in {j.get('seconds')} s"
    check.last = "program %s, PMT %s: %s; %.2f Mb/s" % (
        pg.get("number"), pg.get("pmt_pid"),
        ", ".join(f"{x['pid']} {x.get('codec') or x.get('kind')} {x.get('bitrate', 0) / 1000:.0f} kb/s" for x in pg["streams"]),
        (pg.get("bitrate") or 0) / 1e6)
    return None

if move:
    if not os.path.exists(pids_script):
        raise SystemExit(f"ERROR: {pids_script} is not there. Run the caricoder3 installer, then this again.")
    backup = None
    moved, skipped = [], []
    for name in done:
        say(f"{name}: moving")
        full = chan(name)
        conf = full.get("config") or {}
        if not conf.get("pmt_pid"):
            run_pids([name])
            full = chan(name)
            conf = full.get("config") or {}
        if not conf.get("pmt_pid"):
            note("its identifiers could not be copied (the reason is above): NOT moved, Flussonic left as it is")
            skipped.append(name)
            continue
        try:
            out_url = json.load(open(os.path.join(chdir, name + ".outputs.json")))["outputs"][0]["url"]
        except (OSError, ValueError, LookupError):
            note("the channel has no output: NOT moved")
            skipped.append(name)
            continue
        if dry:
            note(f"DRY RUN: would switch {name} off in Flussonic, switch the channel on and listen to {out_url}")
            continue
        already = conf.get("enabled", True) and full.get("state") == "running"
        old = "unchanged"
        if already:
            note("the channel is running already: only checked")
        else:
            if flussonic_sends(name):
                if backup is None:
                    backup = fconf + time.strftime(".cc3-%Y%m%d-%H%M%S")
                    shutil.copy2(fconf, backup)
                    note(f"Flussonic's config copied to {backup}")
                old = set_disabled(name, "disabled;")
                reload_flussonic()
                t0 = time.time()
                while flussonic_sends(name) and time.time() - t0 < 20:
                    time.sleep(1)
                if flussonic_sends(name):
                    set_disabled(name, old)
                    reload_flussonic()
                    raise SystemExit(f"ERROR: Flussonic still sends {name} 20 s after its config was changed and reloaded: "
                                     "the line is put back, nothing was switched. Stopped here.")
                note(f"Flussonic's stream is off ({time.time() - t0:.0f} s)")
            else:
                note("Flussonic does not send it (already off there)")
            api(f"/api/channels/{name}", "PUT", dict(conf, enabled=True))
            note("channel switched on")
        # what it sends: right within 45 s, on two listens in a row
        bad, good, t0 = "not checked", 0, time.time()
        time.sleep(3)
        while time.time() - t0 < 45 and good < 2:
            bad = check(name, conf, out_url)
            good = 0 if bad else good + 1
            if bad:
                time.sleep(2)
        if good >= 2:
            note(f"checked at {out_url}: {check.last}")
            moved.append(name)
            continue
        note(f"WRONG: {bad}")
        if not already:
            api(f"/api/channels/{name}", "PUT", dict(conf, enabled=False))
            if old != "unchanged":
                set_disabled(name, old)
                reload_flussonic()
                note("put back: the channel is off, the stream is on again in Flussonic")
            else:
                note("the channel is switched off again (Flussonic's stream was off before this and still is)")
        say(f"stopped at {name}. Moved and checked: {', '.join(moved) or 'none'}" +
            (f"; not tried: {', '.join(done[done.index(name) + 1:])}" if done.index(name) + 1 < len(done) else ""))
        raise SystemExit(1)
    say(f"moved and checked: {', '.join(moved) or 'none'}" + (f"; NOT moved: {', '.join(skipped)}" if skipped else ""))
    if backup:
        say(f"Flussonic's config before this: {backup}")
    raise SystemExit(1 if skipped else 0)

if not os.path.exists(pids_script):
    raise SystemExit(f"ERROR: {pids_script} is not there: the identifiers were not copied. Run the caricoder3 installer, then this again.")
env = dict(os.environ, CC3_CONFIG=cfg_path, DRY="1" if dry else "0")
env.setdefault("FLUSSONIC_HTTP", f"http://127.0.0.1:{http_port or 80}")
sys.stdout.flush()
rc = subprocess.run(["bash", pids_script, *done, *(["--start"] if start else [])], env=env).returncode
if rc == 0 and not start and not dry:
    ready = []
    for n in done:
        full = api(f"/api/channels/{n}")
        if ((full.get("channel") or full).get("config") or {}).get("pmt_pid"):
            ready.append(n)
        else:
            say(f"{n}: its identifiers were NOT copied (the reason is above). Do not switch it over yet: run this for {n} again")
    if ready:
        say("next: switch " + ", ".join(ready) + " off in Flussonic, then run this again with: " + " ".join(ready) + " --start")
raise SystemExit(rc)
PY

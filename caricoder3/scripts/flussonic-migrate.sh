#!/bin/bash
# Flussonic's live H.265 streams on this server, recreated as caricoder3
# channels, all switched OFF. Every channel caricoder3 had before is
# removed (a backup of their files is kept). Flussonic is not touched.
#   curl -fsSL .../scripts/flussonic-migrate.sh | sudo bash
#
# Then, one stream at a time: switch the stream off in Flussonic, and in
# caricoder3 open the channel, Settings › Timing & service, Enabled: yes, Save. Each
# channel's output is already set to the address Flussonic sends to.
#
# Before anything is removed, what each Flussonic output carries now
# (programme number, PMT PID, stream PIDs) is written to
# /var/lib/caricoder3/flussonic-outputs/NAME.json, to compare with the
# caricoder3 channel once it runs.
#
# Undo: the backup's path is printed at the end; to put it back:
#   systemctl stop caricoder3; rm /etc/caricoder3/channels/*; tar -xzf BACKUP -C /; systemctl start caricoder3
set -u
CFG=${CC3_CONFIG:-/etc/caricoder3/caricoder3.json}
BIN=${CC3_BIN:-/usr/local/bin/caricoder3}
STATE=${CC3_STATE:-/var/lib/caricoder3}
PROBE_S=${PROBE_S:-3}
say() { printf '\033[1m==>\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

[ -r "$CFG" ] || die "no caricoder3 config at $CFG"
[ -x "$BIN" ] || die "no caricoder3 at $BIN"
command -v python3 >/dev/null || die "python3 is needed"

exec python3 - "$CFG" "$BIN" "$STATE" "$PROBE_S" <<'PY'
import base64, datetime, json, os, subprocess, sys, tarfile, urllib.error, urllib.request

cfg_path, binary, state, probe_s = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
cfg = json.load(open(cfg_path))
chdir = cfg.get("channels_dir", "/etc/caricoder3/channels")
host = cfg.get("listen", "0.0.0.0")
host = "127.0.0.1" if host in ("0.0.0.0", "", "::") else host
base = f"http://{host}:{cfg.get('port', 8550)}"
auth = {"Authorization": "Basic " + base64.b64encode(f"admin:{cfg.get('password', '')}".encode()).decode()}

def say(m):
    print(f"\033[1m==>\033[0m {m}", file=sys.stderr, flush=True)

def note(m):
    print(f"    {m}", file=sys.stderr, flush=True)

def api(path, method="GET", body=None):
    r = urllib.request.Request(base + path, method=method, headers=dict(auth, **{"Content-Type": "application/json"}),
                               data=json.dumps(body).encode() if body is not None else None)
    try:
        with urllib.request.urlopen(r, timeout=20) as f:
            return json.load(f)
    except urllib.error.HTTPError as e:
        raise SystemExit(f"ERROR: {method} {path}: {e.code} {e.read().decode(errors='replace')[:200]}")

SRT = "srt://104.143.4.161:{}?latency=1000"
# name: (input, backup inputs, video, output, extra)          -- from flussonic.conf (enabled streams)
CH = {
    "dischd_srt":     ("udp://224.10.10.10:10000", [], "-1x720:hevc:1800k", "udp://224.5.5.5:8002", {}),
    "discovhome_srt": ("udp://224.10.10.10:10001", [], "-1x480:hevc:1200k", "udp://224.5.5.5:8003", {"pids": [2110, 2104]}),
    "discovkids_srt": ("udp://224.10.10.10:10004", [], "-1x480:hevc:800k",  "udp://224.5.5.5:8004", {"pids": [2310, 2308]}),
    "hbo_srt":        (SRT.format(5030), [], "-1x720:hevc:1250k", "udp://224.5.5.5:8006", {}),
    "hbo2_srt":       (SRT.format(5029), [], "1280x720:fit:hevc:1850k", "udp://224.5.5.5:8007", {}),
    "hbof_srt":       ("http://192.168.110.50/hbof_srt/mpegts", [SRT.format(5028)], "-1x720:hevc:1250k", "udp://224.5.5.5:8008", {}),
    "hbop_srt":       (SRT.format(5027), [], "-1x720:hevc:1250k", "udp://224.5.5.5:8009", {}),
    # Flussonic read this from its own relay of port 5026 (hbos_input): read directly here
    "hbos_srt":       (SRT.format(5026), [], "1920x1080:fit:hevc:1200k", "udp://224.5.5.5:8010",
                       {"profile": "main10", "preset": "p7"}),
    "hbopop_srt":     (SRT.format(5025), [], "-1x720:hevc:1250k", "udp://224.5.5.5:8011", {}),
    "hbomundi_srt":   (SRT.format(5024), [], "-1x720:hevc:1250k", "udp://224.5.5.5:8012", {}),
    "hboex_srt":      (SRT.format(5023), [], "-1x720:hevc:1250k", "udp://224.5.5.5:8013", {}),
    "hgtv_srt":       ("http://192.168.110.50/hgtv_srt/mpegts", [SRT.format(5022)], "-1x720:hevc:2500k", "udp://224.5.5.5:8014", {}),
    "usa_srt":        (SRT.format(5020), [], "-1x720:hevc:2500k", "udp://224.5.5.5:8016", {}),
}

# 1. what Flussonic sends now (read only: listens to its multicast)
snap = os.path.join(state, "flussonic-outputs")
os.makedirs(snap, exist_ok=True)
say(f"what each Flussonic output carries now ({probe_s} s each) -> {snap}")
for name, (_, _, _, out, _) in CH.items():
    p = subprocess.run([binary, "probe", out, "--seconds", str(probe_s)], capture_output=True, text=True)
    try:
        j = json.loads(p.stdout)
    except ValueError:
        j = {"ok": False, "error": (p.stderr or p.stdout).strip()[-200:]}
    json.dump(j, open(os.path.join(snap, name + ".json"), "w"), indent=1)
    progs = j.get("programs") or []
    if j.get("ok") and progs:
        pg = progs[0]
        streams = ", ".join(f"{s.get('pid')} {s.get('codec') or s.get('type_name')}{' ' + s['language'] if s.get('language') else ''}"
                            for s in pg.get("streams", []))
        note(f"{name:15} {out[6:]:20} program {pg.get('number')}, PMT {pg.get('pmt_pid')}: {streams}")
    else:
        note(f"{name:15} {out[6:]:20} nothing received ({j.get('error', 'no programme')})")

# 2. backup of the channels as they are
stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
backup = os.path.join(state, f"channels-backup-{stamp}.tar.gz")
with tarfile.open(backup, "w:gz") as t:
    t.add(chdir, arcname=chdir.lstrip("/"))
say(f"backup of {chdir}: {backup}")

# 3. every channel there is now, removed (stopped first by the supervisor)
have = api("/api/channels")
names = [c["name"] for c in (have.get("channels") if isinstance(have, dict) else have)]
say(f"removing {len(names)} channel(s)")
for n in names:
    api(f"/api/channels/{n}", "DELETE")
    for ext in (".outputs.json", ".streams"):
        try:
            os.remove(os.path.join(chdir, n + ext))
        except FileNotFoundError:
            pass
note(", ".join(names) or "none")

# 4. the Flussonic streams as channels, switched off, outputs in place
say(f"creating {len(CH)} channel(s), switched off")
for name, (inp, backups, video, out, extra) in CH.items():
    with open(os.path.join(chdir, name + ".outputs.json"), "w") as f:
        json.dump({"outputs": [{"id": "out1", "url": out}]}, f, indent=2)
    c = {"input": inp, "video": [video], "audio": ["aac:128k"], "gpu": 1, "enabled": False}
    if backups:
        c["backup_inputs"] = backups
    c.update(extra)
    api(f"/api/channels/{name}", "PUT", c)
    note(f"{name:15} {inp} -> {out}  ({video}{', ' + ', '.join(f'{k} {v}' for k, v in extra.items()) if extra else ''})")
say("done: every channel is OFF. For each: switch it off in Flussonic, then Enabled: yes in caricoder3")
say(f"undo: systemctl stop caricoder3; rm {chdir}/*; tar -xzf {backup} -C /; systemctl start caricoder3")
PY

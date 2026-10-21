#!/bin/bash
#
# caricoder3 - two encoder presets on the same pictures at the same time
# Copyright (c) 2026 CariTech Solutions
#
#   sudo bash preset-compare.sh SOURCE PROGRAM [VIDEO] [PRESET_A] [PRESET_B]
#
#   sudo bash preset-compare.sh udp://224.100.1.5:49500 2010              # tntseries, h264:2500k, p2 vs p4
#   sudo bash preset-compare.sh udp://224.100.1.5:49500 1840 h264:2500k   # bet (1080 HEVC)
#
# Two temporary channels, cmp_a and cmp_b, read the same program with the
# same video setting (as in a channel's "video" list, default h264:2500k)
# and differ only in the preset. Both outputs are measured against the
# source over the same 20 s (tests/quality.py, fetched if not at
# /tmp/quality.py), so the pictures they are judged on are the same ones;
# one after the other, the content would differ more than the presets do.
# The channels are removed at the end, whatever happens. Their outputs go
# to 224.100.200.21:5000 and .22:5000, with godtv's output options.
#
set -euo pipefail

SRC="${1:?source, e.g. udp://224.100.1.5:49500}"
PROG="${2:?program number}"
VIDEO="${3:-h264:2500k}"
PA="${4:-p2}"
PB="${5:-p4}"
SECONDS_CAP=20
QUALITY=/tmp/quality.py
BRANCH_URL="https://raw.githubusercontent.com/caritechsolutions/Caricoder2/claude/caricoder3-stage2/caricoder3"

CFG="${CC3_CONFIG:+--config $CC3_CONFIG}"
PW="$(caricoder3 serve $CFG --show-password | awk '{print $3}')"
PORT="$(caricoder3 serve $CFG --show-port)"
API="http://127.0.0.1:$PORT/api"
call() { curl -fsS -u "admin:$PW" -H 'Content-Type: application/json' "$@"; }

[ -f "$QUALITY" ] || curl -fsSL "$BRANCH_URL/tests/quality.py?$(date +%s)" -o "$QUALITY"
command -v ffmpeg >/dev/null || { echo "ffmpeg is needed (apt install ffmpeg)"; exit 1; }

cleanup() {
    for n in cmp_a cmp_b; do
        call -X DELETE "$API/channels/$n" >/dev/null 2>&1 || true
    done
    rm -f /tmp/cmp_a.json /tmp/cmp_b.json
}
trap cleanup EXIT

OPTS="$(call "$API/channels/godtv" 2>/dev/null | python3 -c '
import json, sys
c = json.load(sys.stdin)["channel"]
outs = ((c.get("outputs") or c.get("saved_outputs") or {}).get("outputs") or [])
u = next((o["url"] for o in outs if o.get("url", "").startswith("udp://")), "")
print(u.split("?", 1)[1] if "?" in u else "")' 2>/dev/null || true)"

k=21
for n in cmp_a cmp_b; do
    preset="$PA"
    [ "$n" = cmp_b ] && preset="$PB"
    cfg="$(SRC="$SRC" PROG="$PROG" VIDEO="$VIDEO" P="$preset" python3 -c '
import json, os
print(json.dumps({"input": os.environ["SRC"], "program": int(os.environ["PROG"]), "video": [os.environ["VIDEO"]],
                  "preset": os.environ["P"], "enabled": True}))')"
    call -X PUT "$API/channels/$n" -d "$cfg" >/dev/null
    for _ in $(seq 1 50); do
        st="$(call "$API/channels/$n" | python3 -c 'import json, sys; print(json.load(sys.stdin)["channel"]["state"])')"
        [ "$st" = running ] && break
        sleep 0.2
    done
    url="udp://224.100.200.$k:5000${OPTS:+?$OPTS}"
    call -X POST "$API/channels/$n/outputs" -d "{\"url\": \"$url\", \"save\": false}" >/dev/null
    echo "$n: preset $preset, $VIDEO -> $url"
    k=$((k + 1))
done

# the outputs are read back on the interface they are sent on (godtv's iface=, if it has one)
CAP="$(OPTS="$OPTS" python3 -c '
import os
keep = [p for p in os.environ["OPTS"].split("&") if p.split("=")[0] in ("iface", "interface", "localaddr")]
print("&".join(keep))')"

echo "waiting 15 s for both encoders to settle ..."
sleep 15
echo "measuring both for $SECONDS_CAP s at once ..."
python3 "$QUALITY" --source "$SRC" --program "$PROG" --output "udp://224.100.200.21:5000${CAP:+?$CAP}" \
    --seconds "$SECONDS_CAP" --json > /tmp/cmp_a.json 2>/tmp/cmp_a.log &
a=$!
python3 "$QUALITY" --source "$SRC" --program "$PROG" --output "udp://224.100.200.22:5000${CAP:+?$CAP}" \
    --seconds "$SECONDS_CAP" --json > /tmp/cmp_b.json 2>/tmp/cmp_b.log &
b=$!
wait "$a" || true
wait "$b" || true

PA="$PA" PB="$PB" python3 -c '
import json, os
def load(p, log):
    try:
        return json.load(open(p))
    except Exception:
        print("no result for", p, "-", open(log).read()[-300:])
        return None
ra, rb = load("/tmp/cmp_a.json", "/tmp/cmp_a.log"), load("/tmp/cmp_b.json", "/tmp/cmp_b.log")
if ra and rb:
    s = ra["source"]
    na, nb = os.environ["PA"], os.environ["PB"]
    print()
    print("source:", s["codec"], s["size"])
    print("%-22s%12s%12s" % ("", na, nb))
    a, b = ra["renditions"][0], rb["renditions"][0]
    rows = (("PSNR mean (dB)", "psnr_mean_db", "%.2f"), ("PSNR worst (dB)", "psnr_min_db", "%.2f"),
            ("SSIM mean", "ssim_mean", "%.4f"), ("SSIM worst", "ssim_min", "%.4f"),
            ("output (kb/s)", "output_kbps", "%s"), ("frames compared", "frames", "%s"))
    for label, key, fmt in rows:
        print("%-22s%12s%12s" % (label, fmt % a[key], fmt % b[key]))
    print()
    print("per second (dB):")
    print(" ", na, a["psnr_per_second_db"])
    print(" ", nb, b["psnr_per_second_db"])
'
for f in /tmp/cmp_a.log /tmp/cmp_b.log; do grep -h "paired again" "$f" 2>/dev/null || true; done
echo
echo "temporary channels removed"

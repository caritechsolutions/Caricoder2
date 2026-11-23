#!/bin/bash
# A channel's picture made smaller looks wrong: the same done offline, by
# each route, from 20 s of the channel's real input (the live channel is
# not touched).
#   curl -fsSL .../scripts/scale-repro.sh | sudo bash -s -- NAME [WxH]
# (WxH: the size to try, default -1x720.)
# Writes, in /var/lib/caricoder3/scale-repro/NAME/:
#   source.ts         20 s of what the channel receives
#   0-as-is.jpg       the source size, card decode + card encode (control)
#   1-card.jpg        the card's decoder makes the size (as a channel alone does)
#   2-card-process.jpg  the same inside the running card process (as the channel does)
#   3-cpu.jpg         CPU decode and resize, card encode
#   4-off.jpg         the card's resize, deinterlace off
#   5-card-deint.jpg  the card deinterlaces, then resizes
# and, for the source and each result, the timing of the pictures (how
# many, the gaps between them, anything uneven: scripts/ts-cadence.py).
# and prints what each run said about the video. Each picture is taken
# from the encoded result, decoded on the CPU.
set -u
NAME=${1:-}
SIZE=${2:--1x720}
[ -n "$NAME" ] || { echo "usage: ... | sudo bash -s -- CHANNEL [WxH]" >&2; exit 2; }
CFG=${CC3_CONFIG:-/etc/caricoder3/caricoder3.json}
BIN=${CC3_BIN:-/usr/local/bin/caricoder3}
STATE=${CC3_STATE:-/var/lib/caricoder3}
RUN=${CC3_RUN:-$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('run_dir','/run/caricoder3'))" "$CFG")}
CHDIR=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('channels_dir','/etc/caricoder3/channels'))" "$CFG")
DIR="$STATE/scale-repro/$NAME"
say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }

CH="$CHDIR/$NAME.json"
[ -r "$CH" ] || { echo "no channel $NAME ($CH)" >&2; exit 1; }
read -r GPU VIDEO < <(python3 -c "
import json,sys; c=json.load(open(sys.argv[1]))
v=(c.get('video') or ['hevc:1250k'])[0].split(':')
codec=[x for x in v if x in ('hevc','h264','h265')]; rate=[x for x in v if x[-1:] in 'kKmM' and x[:-1].replace('.','').isdigit()]
print(c.get('gpu',0), (codec[0] if codec else 'hevc')+':'+(rate[0] if rate else '1250k'))" "$CH")
mkdir -p "$DIR"
rm -f "$DIR"/*.ts "$DIR"/*.jpg "$DIR"/*.log
cat > "$DIR/cadence.py" <<'CADENCE'
#!/usr/bin/env python3
"""The timing of the video pictures in a TS file: how many, the gaps between
their presentation times (sorted into display order), and anything uneven.
   ts-cadence.py FILE [FILE ...]
Read only. For comparing a channel's output made two ways (scale-repro.sh)."""
import collections, sys

def video_pts(path):
    data = open(path, "rb").read()
    pmt_pid, vpid, pts = None, None, []
    for off in range(0, len(data) - 187, 188):
        p = data[off:off + 188]
        if p[0] != 0x47:
            continue
        pid = ((p[1] & 0x1F) << 8) | p[2]
        pusi = p[1] & 0x40
        afc = (p[3] >> 4) & 3
        i = 4
        if afc & 2:
            i += 1 + p[4]
        if not (afc & 1) or i >= 188:
            continue
        pl = p[i:]
        if pid == 0 and pusi and pmt_pid is None:
            s = pl[1 + pl[0]:]
            n = ((s[1] & 0x0F) << 8) | s[2]
            for k in range(8, 3 + n - 4, 4):
                if (s[k] << 8) | s[k + 1]:
                    pmt_pid = ((s[k + 2] & 0x1F) << 8) | s[k + 3]
                    break
        elif pid == pmt_pid and pusi and vpid is None:
            s = pl[1 + pl[0]:]
            n = ((s[1] & 0x0F) << 8) | s[2]
            k = 12 + (((s[10] & 0x0F) << 8) | s[11])
            while k < 3 + n - 4:
                st, epid = s[k], ((s[k + 1] & 0x1F) << 8) | s[k + 2]
                if st in (0x01, 0x02, 0x1B, 0x24) and vpid is None:
                    vpid = epid
                k += 5 + (((s[k + 3] & 0x0F) << 8) | s[k + 4])
        elif pid == vpid and pusi and pl[:3] == b"\0\0\1" and pl[7] & 0x80:
            b = pl[9:14]
            pts.append(((b[0] >> 1) & 7) << 30 | b[1] << 22 | (b[2] >> 1) << 15 | b[3] << 7 | b[4] >> 1)
    return pts

for path in sys.argv[1:]:
    pts = sorted(video_pts(path))
    if len(pts) < 3:
        print(f"{path}: {len(pts)} pictures")
        continue
    gaps = [(b - a) / 90.0 for a, b in zip(pts, pts[1:])]
    span = (pts[-1] - pts[0]) / 90000.0
    hist = collections.Counter(round(g, 1) for g in gaps)
    top = ", ".join(f"{g} ms x{n}" for g, n in sorted(hist.items(), key=lambda x: -x[1])[:5])
    odd = sum(1 for g in gaps if g <= 1 or g > 60)
    print(f"{path.rsplit('/', 1)[-1]:22} {len(pts)} pictures in {span:.1f} s = {(len(pts) - 1) / span:.3f}/s; "
          f"gaps: {top}; odd (0 or over 60 ms): {odd}")
CADENCE
cadence() { python3 "$DIR/cadence.py" "$1" | sed 's/^/    timing: /'; }

say "$NAME: 20 s of its input (card $GPU, video $VIDEO, trying $SIZE)"
"$BIN" ctl "$RUN/$NAME.sock" keep-input "$DIR/source.ts" 20 0 >/dev/null || { echo "the channel did not save its input (is it running?)" >&2; exit 1; }
for _ in $(seq 1 30); do
    [ -s "$DIR/source.ts" ] && break
    sleep 1
done
[ -s "$DIR/source.ts" ] || { echo "no input file came" >&2; exit 1; }
sleep 2
note "$(du -h "$DIR/source.ts" | cut -f1) saved"
"$BIN" probe "$DIR/source.ts" | python3 -c "
import json,sys; j=json.load(sys.stdin)
for g in j.get('programs',[]):
    for s in g.get('streams',[]):
        if s.get('kind')=='video':
            print('    source video:', {k: s[k] for k in s if k in ('pid','type_name','width','height','interlaced','fps','profile','bitrate')})"
cadence "$DIR/source.ts"

run() {   # label, extra args...
    local lab=$1; shift
    "$BIN" -i "$DIR/source.ts" -o "$DIR/$lab.ts" --gpu "$GPU" "$@" > "$DIR/$lab.log" 2>&1
    grep -E "video .* -> |scal|WARN|ERR|cannot|refused" "$DIR/$lab.log" | grep -v "time base\|late\|clock" | head -6 | sed 's/^[0-9-]* [0-9:.]* /    /'
    "$BIN" -i "$DIR/$lab.ts" -o /dev/null --decoder cpu --thumb "$DIR/$lab.jpg" --thumb-interval 1 >/dev/null 2>&1
    if [ -s "$DIR/$lab.jpg" ]; then note "picture: $DIR/$lab.jpg"; else note "no picture came out"; fi
    [ -s "$DIR/$lab.ts" ] && cadence "$DIR/$lab.ts"
}
say "0: the source size, card decode + card encode (the control)"
run 0-as-is --video "$VIDEO"
say "1: the card's decoder makes $SIZE"
run 1-card --video "$SIZE:$VIDEO"
say "2: the same in the running card process (as the channel does)"
if [ -S "$RUN/card-$GPU.sock" ]; then
    run 2-card-process --card-process "$RUN" --video "$SIZE:$VIDEO"
else
    note "no card process for card $GPU ($RUN/card-$GPU.sock): skipped"
fi
say "3: CPU decode and resize, card encode"
run 3-cpu --decoder cpu --video "$SIZE:$VIDEO"
say "4: the card's resize to $SIZE, deinterlace off"
run 4-off --deinterlace off --video "$SIZE:$VIDEO"
say "5: the card deinterlaces, then resizes to $SIZE"
run 5-card-deint --deinterlace card --video "$SIZE:$VIDEO"
say "done: send the .jpg pictures above (and the lines) to Claude"

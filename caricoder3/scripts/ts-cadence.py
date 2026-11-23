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

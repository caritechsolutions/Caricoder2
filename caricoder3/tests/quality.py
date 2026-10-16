#!/usr/bin/env python3
"""
quality.py - how much does a transcode change the picture?

Compares the video of a transcoded stream with the source it was made
from, frame by frame, matched by PTS (caricoder3 passes PTS through
unchanged, so the match is exact), and reports PSNR and SSIM from
ffmpeg's own filters, per second and overall, plus the bit rates.

Two ways to use it:

  # files you already have (the source as it went in, the output as it came out)
  quality.py --source src.ts --output out.ts

  # capture both from the network for 20 seconds, then compare
  quality.py --source udp://224.100.1.5:49500 --program 1840 \
             --output udp://224.6.7.9:6789 --seconds 20

A rendition smaller than the source is compared against the source
scaled down to its size (bicubic), which is what a viewer of that
rendition would ideally see. Needs ffmpeg and ffprobe; the captures
use caricoder3 itself (copy mode, so the files are exactly what was
on the wire).

What the numbers mean, roughly, for broadcast content: PSNR above 40 dB
is hard to tell from the source, 35-40 good, 30-35 visible on detail,
below 30 clearly degraded. SSIM above 0.95 good, 0.90-0.95 fair.
"""
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def die(msg):
    print("error:", msg, file=sys.stderr)
    sys.exit(1)


def probe_streams(path):
    p = run(["ffprobe", "-v", "error", "-show_streams", "-show_programs", "-of", "json", path])
    if p.returncode:
        die(f"ffprobe {path}: {p.stderr.strip()[:200]}")
    return json.loads(p.stdout)


def video_streams(info):
    out = []
    for s in info.get("streams", []):
        if s.get("codec_type") == "video" and s.get("width"):
            out.append(s)
    return out


def frame_pts(path, stream_index):
    """
    PTS (seconds) of every decoded frame, in output order; frames before
    the first key frame are None (a capture starts mid-GOP, and what the
    decoder makes of those is not the picture).
    """
    p = run(["ffprobe", "-v", "error", "-select_streams", f"{stream_index}", "-show_frames",
             "-show_entries", "frame=pts_time,key_frame", "-of", "csv=p=0", path])
    if p.returncode:
        die(f"ffprobe frames {path}: {p.stderr.strip()[:200]}")
    pts = []
    seen_key = False
    for line in p.stdout.splitlines():
        if not line.strip():
            continue
        parts = line.split(",")          # ffprobe's order: key_frame, pts_time[, side data]
        if len(parts) < 2:
            continue
        key, t = parts[0].strip(), parts[1].strip()
        if key == "1":
            seen_key = True
        pts.append(None if t in ("N/A", "") or not seen_key else float(t))
    return pts


def decode_raw(path, stream_index, size, out_path, nframes):
    """Decodes the first `nframes` frames of one video stream to raw yuv420p at `size`."""
    p = run(["ffmpeg", "-v", "error", "-y", "-i", path, "-map", f"0:{stream_index}",
             "-vf", f"scale={size.replace('x', ':')}:flags=bicubic", "-fps_mode", "passthrough",
             "-frames:v", str(nframes), "-pix_fmt", "yuv420p", "-f", "rawvideo", out_path])
    if p.returncode:
        die(f"ffmpeg decode {path}: {p.stderr.strip()[:300]}")


def pick_frames(raw_in, raw_out, size, indices):
    """Copies the frames with these indices (in order) from one raw file to another."""
    w, h = (int(x) for x in size.split("x"))
    fsz = w * h * 3 // 2
    have = os.path.getsize(raw_in) // fsz
    n = 0
    with open(raw_in, "rb") as f, open(raw_out, "wb") as o:
        for i in indices:
            if i >= have:
                break
            f.seek(i * fsz)
            o.write(f.read(fsz))
            n += 1
    return n


def decode_all(src, si, out, oi, size, ns, no, work):
    """
    Both captures decoded once, whole, to raw files; returns (source
    file, output file). The frame count is checked against ffprobe's
    frame list, because the pairs index into these files by position.
    """
    w, h = (int(x) for x in size.split("x"))
    fsz = w * h * 3 // 2
    s_all = os.path.join(work, "s_all.yuv")
    o_all = os.path.join(work, "o_all.yuv")
    decode_raw(src, si, size, s_all, ns)
    decode_raw(out, oi, size, o_all, no)
    for name, path, n in (("source", s_all, ns), ("output", o_all, no)):
        have = os.path.getsize(path) // fsz
        if have != n:
            print(f"note: {name} decoded to {have} frames, ffprobe listed {n}", file=sys.stderr)
    return s_all, o_all


def pick_pairs(s_all, o_all, size, pairs, work, tag):
    """Raw files of the paired frames, aligned: (source file, output file, frames)."""
    s_raw = os.path.join(work, f"{tag}_s.yuv")
    o_raw = os.path.join(work, f"{tag}_o.yuv")
    n1 = pick_frames(s_all, s_raw, size, [i for i, _ in pairs])
    n2 = pick_frames(o_all, o_raw, size, [j for _, j in pairs])
    return s_raw, o_raw, min(n1, n2)


def measure(src_raw, out_raw, size, fps, log_dir, names=("psnr", "ssim")):
    """PSNR and SSIM per frame via ffmpeg's filters."""
    w, h = size.split("x")
    res = {}
    for name in names:
        logf = os.path.join(log_dir, name + ".log")
        p = run(["ffmpeg", "-v", "error", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", size, "-r", str(fps),
                 "-i", out_raw, "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", size, "-r", str(fps), "-i", src_raw,
                 "-lavfi", f"[0:v][1:v]{name}=stats_file={logf}", "-f", "null", "-"])
        if p.returncode:
            die(f"ffmpeg {name}: {p.stderr.strip()[:300]}")
        vals = []
        for line in open(logf):
            d = dict(kv.split(":", 1) for kv in line.split() if ":" in kv)
            if name == "psnr":
                v = d.get("psnr_avg", "inf")
            else:
                v = d.get("All", "1")
            try:
                vals.append(float(v))
            except ValueError:
                vals.append(float("inf") if name == "psnr" else 1.0)
        res[name] = vals
    return res


def pairs_at(spts, opts, offset, period, limit):
    """Frame pairs (source index, output index) with out_pts = src_pts + offset."""
    smap = {}
    for i, t in enumerate(spts):
        if t is not None:
            smap[int(round(t / period))] = i
    pairs = []
    for j, t in enumerate(opts):
        if t is None:
            continue
        i = smap.get(int(round((t - offset) / period)))
        if i is not None:
            pairs.append((i, j))
            if len(pairs) >= limit:
                break
    return pairs


def match_frames(s_all, o_all, spts, opts, size, fps, work, limit):
    """
    Which source frame is which output frame? caricoder3 keeps the PTS,
    but a channel that has absorbed a source restart carries a constant
    offset on its time line, so the offset is found first (the most
    common out - in difference), and then, because an offset of a whole
    number of frames would match every frame to a neighbour just as
    well, the shift that makes the pictures agree best is chosen by
    comparing a few frames at every shift that still overlaps: the
    histogram is only trusted for the sub-frame phase, since with a
    regular frame period every whole-frame shift is about as common.
    """
    period = 1.0 / fps
    # the common offset, to the millisecond
    # (every pair, not a sample: sampling both lists hides the one
    # offset at which the frames coincide)
    hist = {}
    s_list = [t for t in spts if t is not None][:1500]
    for t in [x for x in opts if x is not None][:1500]:
        for u in s_list:
            d = round(t - u, 3)
            hist[d] = hist.get(d, 0) + 1
    if not hist:
        return []
    base = max(hist, key=hist.get)
    reach = max(len(s_list), len(opts))
    scores = []
    for k in range(-reach, reach + 1):
        off = base + k * period
        pairs = pairs_at(spts, opts, off, period, 400)
        if len(pairs) < 10:
            continue
        sample = pairs[len(pairs) // 4::max(1, len(pairs) // 8)][:6]
        s_raw, o_raw, _ = pick_pairs(s_all, o_all, size, sample, work, "probe")
        m = measure(s_raw, o_raw, size, fps, work, ("psnr",))
        vals = [v for v in m["psnr"] if v != float("inf")] or [99.0]
        scores.append((sum(vals) / len(vals), k, off))
    if not scores:
        return []
    scores.sort(reverse=True)
    best_score, best_k, best = scores[0]
    print(f"time-line offset {best:+.3f} s (output - source): PTS offset {base:+.3f} s "
          f"{best_k:+d} frames, picked by content among {len(scores)} shifts; "
          "best shifts (frames: PSNR dB) " +
          ", ".join(f"{k:+d}: {sc:.1f}" for sc, k, _ in scores[:5]), file=sys.stderr)
    return pairs_at(spts, opts, best, period, limit)


def bitrate(path, stream_index):
    """Bit rate of one stream from its packets (bits per second of PTS)."""
    p = run(["ffprobe", "-v", "error", "-select_streams", f"{stream_index}", "-show_packets",
             "-show_entries", "packet=pts_time,size", "-of", "csv=p=0", path])
    pts, size = [], 0
    for line in p.stdout.splitlines():
        parts = line.split(",")
        if len(parts) >= 2 and parts[0] not in ("N/A", ""):
            pts.append(float(parts[0]))
            size += int(parts[1])
    if len(pts) < 2:
        return 0.0
    span = max(pts) - min(pts)
    return size * 8 / span if span > 0 else 0.0


def capture(url, program, seconds, out_path, binary):
    args = [binary, "-q", "-i", url, "-o", out_path, "--stats-interval", "0"]
    if program:
        args += ["-p", str(program)]
    proc = subprocess.Popen(args, stderr=subprocess.PIPE, text=True)
    time.sleep(seconds)
    proc.send_signal(signal.SIGTERM)
    try:
        _, log = proc.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        _, log = proc.communicate()
    if not os.path.exists(out_path) or os.path.getsize(out_path) < 188 * 100:
        die(f"capture of {url} got nothing: {log[-300:]}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", required=True, help="the source: a .ts file or udp://...")
    ap.add_argument("--output", required=True, help="the transcoded stream: a .ts file or udp://...")
    ap.add_argument("--program", type=int, default=0, help="program to take from the source (capture)")
    ap.add_argument("--output-program", type=int, default=0, help="program to take from the output (capture)")
    ap.add_argument("--seconds", type=int, default=20, help="capture length (default 20)")
    ap.add_argument("--max-frames", type=int, default=600, help="frames compared at most (default 600)")
    ap.add_argument("--keep", action="store_true", help="keep the captures and raw frames")
    ap.add_argument("--json", action="store_true", help="print the result as JSON")
    ap.add_argument("--pictures", metavar="DIR", help="write side-by-side pictures (source | output) of three matched frames here")
    a = ap.parse_args()

    for tool in ("ffmpeg", "ffprobe"):
        if not shutil.which(tool):
            die(f"{tool} is not installed")
    binary = shutil.which("caricoder3") or os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "caricoder3")

    work = tempfile.mkdtemp(prefix="quality.")
    try:
        src, out = a.source, a.output
        if "://" in src or "://" in out:
            print(f"capturing {a.seconds} s of both streams ...", file=sys.stderr)
            procs = []
            if "://" in src:
                src_file = os.path.join(work, "source.ts")
                procs.append((src, a.program, src_file))
                src = src_file
            if "://" in out:
                out_file = os.path.join(work, "output.ts")
                procs.append((out, a.output_program, out_file))
                out = out_file
            running = []
            for url, prog, path in procs:
                args = [binary, "-q", "-i", url, "-o", path, "--stats-interval", "0"]
                if prog:
                    args += ["-p", str(prog)]
                running.append(subprocess.Popen(args, stderr=subprocess.PIPE, text=True))
            time.sleep(a.seconds)
            for p in running:
                p.send_signal(signal.SIGTERM)
            for p, (url, _, path) in zip(running, procs):
                try:
                    _, log = p.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    p.kill()
                    _, log = p.communicate()
                if not os.path.exists(path) or os.path.getsize(path) < 188 * 100:
                    die(f"capture of {url} got nothing: {log[-300:]}")

        sinfo, oinfo = probe_streams(src), probe_streams(out)
        svs, ovs = video_streams(sinfo), video_streams(oinfo)
        if not svs:
            die("no video in the source")
        if not ovs:
            die("no video in the output")
        sv = svs[0]
        results = []
        for ov in ovs:                      # one result per rendition
            size = f"{ov['width']}x{ov['height']}"
            fps = eval(ov.get("avg_frame_rate", "25/1").replace("/", "/1.0/")) if "/" in ov.get("avg_frame_rate", "") else 25
            spts = frame_pts(src, sv["index"])
            opts = frame_pts(out, ov["index"])
            s_all, o_all = decode_all(src, sv["index"], out, ov["index"], size, len(spts), len(opts), work)
            pairs = match_frames(s_all, o_all, spts, opts, size, fps, work, a.max_frames)
            if len(pairs) < 10:
                die(f"only {len(pairs)} frames of the output could be matched to the source "
                    f"(source {len(spts)} frames, output {len(opts)}); are these the same content?")
            s_raw, o_raw, n = pick_pairs(s_all, o_all, size, pairs, work, "full")
            m = measure(s_raw, o_raw, size, fps, work)
            if a.pictures:
                os.makedirs(a.pictures, exist_ok=True)
                worst = min(range(n), key=lambda k: m["psnr"][k]) if n else 0
                for label, k in (("first", 0), ("middle", n // 2), ("worst", worst)):
                    png = os.path.join(a.pictures, f"{size}_{label}.png")
                    run(["ffmpeg", "-v", "error", "-y",
                         "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", size, "-i", s_raw,
                         "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", size, "-i", o_raw,
                         "-filter_complex", f"[0:v]select=eq(n\\,{k})[a];[1:v]select=eq(n\\,{k})[b];[a][b]hstack",
                         "-frames:v", "1", png])
                    print(f"picture: {png} (frame {k}, PSNR {m['psnr'][k]:.1f} dB)", file=sys.stderr)
            psnr = [v for v in m["psnr"][:n]]
            ssim = m["ssim"][:n]
            finite = [v for v in psnr if v != float("inf")]
            per_sec = []
            step = max(1, int(round(fps)))
            for k in range(0, len(psnr), step):
                chunk = [v for v in psnr[k:k + step] if v != float("inf")]
                per_sec.append(round(sum(chunk) / len(chunk), 2) if chunk else None)
            results.append({
                "size": size,
                "codec": ov.get("codec_name"),
                "frames": n,
                "psnr_mean_db": round(sum(finite) / len(finite), 2) if finite else None,
                "psnr_min_db": round(min(finite), 2) if finite else None,
                "psnr_per_second_db": per_sec,
                "ssim_mean": round(sum(ssim) / len(ssim), 4) if ssim else None,
                "ssim_min": round(min(ssim), 4) if ssim else None,
                "source_kbps": round(bitrate(src, sv["index"]) / 1000),
                "output_kbps": round(bitrate(out, ov["index"]) / 1000),
            })
        report = {"source": {"size": f"{sv['width']}x{sv['height']}", "codec": sv.get("codec_name"),
                             "frames": len(spts)}, "renditions": results}
        if a.json:
            print(json.dumps(report, indent=2))
        else:
            print(f"source: {report['source']['codec']} {report['source']['size']}, {report['source']['frames']} frames")
            for r in results:
                print(f"\n{r['codec']} {r['size']}: {r['frames']} frames compared, "
                      f"{r['source_kbps']} kb/s in -> {r['output_kbps']} kb/s out")
                print(f"  PSNR {r['psnr_mean_db']} dB mean, {r['psnr_min_db']} dB worst frame")
                print(f"  SSIM {r['ssim_mean']} mean, {r['ssim_min']} worst frame")
                print("  per second (PSNR dB): " + " ".join("--" if v is None else f"{v:.1f}" for v in r["psnr_per_second_db"]))
        if a.keep:
            print(f"\nkept: {work}", file=sys.stderr)
    finally:
        if not a.keep:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()

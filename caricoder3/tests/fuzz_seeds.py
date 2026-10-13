#!/usr/bin/env python3
"""
Writes starting inputs for the live-mode fuzzer (tests/fuzz_live.c).

  python3 tests/fuzz_seeds.py build/corpus-live

Each one is a short synthetic stream wrapped in the fuzzer's record format
and shaped like something a live input really does: runs clean, restarts on
a new clock, skips ahead, stalls, arrives late in a burst, loses its PCR,
carries a damaged PCR. The fuzzer mutates from there.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_ts                                                   # noqa: E402

TS = 188
HZ = 27_000_000
PACE, SLOW, FAST, BURST = 2, 3, 1, 0        # indices into gap_us[] in fuzz_live.c
GAP_250, GAP_800, GAP_3S = 5, 6, 7
ASLEEP = 0x40


def stream(seed, base, duration=2.4, pcr_ms=30):
    prog = gen_ts.default_program(video_rate=550_000)
    return gen_ts.build({"muxrate": 1_000_000, "duration": duration, "seed": seed,
                         "pcr_base": int(base * HZ), "programs": [prog],
                         "pcr_ms": pcr_ms})


def records(ts, gap=PACE, first=None, per=7):
    """Datagrams of `per` packets, each `gap` after the one before."""
    out = bytearray()
    step = per * TS
    for n, off in enumerate(range(0, len(ts), step)):
        chunk = ts[off:off + step]
        g = first if (n == 0 and first is not None) else gap
        out.append(g | ((len(chunk) // TS - 1) << 3))
        out += chunk
    return bytes(out)


def cut(ts, a, b):
    """The packets between fractions a and b of the stream, datagram-aligned."""
    n = len(ts) // TS
    return ts[int(n * a) // 7 * 7 * TS:int(n * b) // 7 * 7 * TS]


def without_pid(ts, pid, start):
    out = bytearray()
    for i in range(len(ts) // TS):
        p = ts[i * TS:(i + 1) * TS]
        if i >= start and ((p[1] & 0x1F) << 8 | p[2]) == pid:
            continue
        out += p
    return bytes(out)


def bad_pcr(ts, which, shift):
    data = bytearray(ts)
    seen = 0
    for i in range(len(data) // TS):
        p = i * TS
        if not (data[p + 3] & 0x20) or data[p + 4] < 7 or not (data[p + 5] & 0x10):
            continue
        if seen == which:
            base = (data[p + 6] << 25) | (data[p + 7] << 17) | (data[p + 8] << 9) | \
                   (data[p + 9] << 1) | (data[p + 10] >> 7)
            ext = ((data[p + 10] & 1) << 8) | data[p + 11]
            data[p + 6:p + 12] = gen_ts.pcr_bytes(base * 300 + ext + int(shift * HZ))
        seen += 1
    return bytes(data)


def xcode_seeds(out_dir):
    """Short real streams (made by ffmpeg) for tests/fuzz_xcode.c."""
    import shutil
    import subprocess
    if not shutil.which("ffmpeg"):
        print("ffmpeg is needed to make the transcoding seeds")
        return 1
    os.makedirs(out_dir, exist_ok=True)
    n = 0
    for name, v, a in (
            ("h264_mp2", ["-c:v", "libx264", "-preset", "veryfast", "-b:v", "300k", "-g", "12"],
             ["-c:a", "mp2", "-b:a", "96k"]),
            ("h264_aac", ["-c:v", "libx264", "-preset", "veryfast", "-b:v", "300k", "-g", "12",
                          "-bf", "2"], ["-c:a", "aac", "-b:a", "64k"]),
            ("hevc_ac3", ["-c:v", "libx265", "-preset", "ultrafast", "-b:v", "300k",
                          "-x265-params", "log-level=error:keyint=12"],
             ["-c:a", "ac3", "-b:a", "96k"]),
            ("h264_mp3", ["-c:v", "libx264", "-preset", "veryfast", "-b:v", "300k", "-g", "12"],
             ["-c:a", "libmp3lame", "-b:a", "64k"])):
        p = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
                            "testsrc2=size=160x120:rate=25:duration=3", "-f", "lavfi", "-i",
                            "sine=frequency=700:sample_rate=48000:duration=3",
                            "-pix_fmt", "yuv420p", *v, *a, "-f", "mpegts", "-muxrate", "700k",
                            "-"], capture_output=True)
        if p.returncode or not p.stdout:
            continue
        for mode in (0, 1, 2, 3, 8, 18):
            with open(os.path.join(out_dir, f"seed_{name}_{mode}"), "wb") as f:
                f.write(bytes([mode]) + p.stdout[:399_000])
            n += 1
    print(f"{n} seeds written to {out_dir}")
    return 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--xcode":
        return xcode_seeds(sys.argv[2] if len(sys.argv) > 2 else "build/corpus-xcode")
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "build/corpus-live"
    os.makedirs(out_dir, exist_ok=True)
    a = stream(1, 10.0)
    b = stream(2, 7000.0, duration=1.2)
    sparse = stream(3, 10.0, pcr_ms=240)
    n = len(a) // TS

    seeds = {
        "clean":        records(a),
        "restart":      records(cut(a, 0, 0.5)) + records(b),
        "skip_ahead":   records(cut(a, 0, 0.3)) + records(cut(a, 0.6, 1)),
        "skip_fast":    records(cut(a, 0, 0.3)) + records(cut(a, 0.5, 0.7), gap=FAST)
                        + records(cut(a, 0.7, 1)),
        "skip_small":   records(cut(a, 0, 0.4)) + records(cut(a, 0.6, 1)),
        "skip_back":    records(cut(a, 0, 0.7)) + records(cut(a, 0.3, 0.8)),
        "stall_late":   records(cut(a, 0, 0.4)) + records(cut(a, 0.4, 1), first=GAP_800),
        "late_burst":   records(cut(a, 0, 0.4)) + records(cut(a, 0.4, 0.7), gap=BURST,
                                                          first=GAP_800)
                        + records(cut(a, 0.7, 1)),
        "slow_drain":   records(cut(a, 0, 0.3)) + records(cut(a, 0.3, 0.6), gap=FAST,
                                                          first=GAP_250)
                        + records(cut(a, 0.6, 1)),
        "outage":       records(cut(a, 0, 0.5)) + records(b, first=GAP_3S),
        "asleep":       records(cut(a, 0, 0.5)) + records(cut(a, 0.5, 1),
                                                          first=GAP_800 | ASLEEP),
        "pcr_gone":     records(without_pid(a, 1651, n // 3)),
        "pcr_pause":    records(without_pid(cut(a, 0, 0.7), 1651, n // 4))
                        + records(cut(a, 0.7, 1)),
        "bad_pcr":      records(bad_pcr(bad_pcr(bad_pcr(a, 20, 4.0), 40, -4.0), 60, 0.6)),
        "back_pcr_gone": records(cut(a, 0, 0.5))
                         + records(without_pid(cut(a, 0.1, 0.7), 1651, n // 20)),
        "fwd_pcr_gone":  records(cut(a, 0, 0.3))
                         + records(without_pid(cut(a, 0.6, 1), 1651, n // 20)),
        "two_bad_pcrs":  records(bad_pcr(bad_pcr(a, 30, 4.0), 31, 4.0)),
        "sparse":       records(sparse),
        "sparse_skip":  records(cut(sparse, 0, 0.3)) + records(cut(sparse, 0.6, 1)),
        "ends_in_doubt": records(cut(a, 0, 0.6)) + records(cut(b, 0, 0.5)),
        "slow_source":  records(a, gap=SLOW)[:200_000],
    }
    for mode, tag in ((0x00, "cbr"), (0x08, "vbr"), (0x02, "lowdelay")):
        for name, body in seeds.items():
            data = bytes([mode]) + body
            if len(data) > 400_000:
                data = data[:400_000]
            with open(os.path.join(out_dir, f"seed_{name}_{tag}"), "wb") as f:
                f.write(data)
    print(f"{3 * len(seeds)} seeds written to {out_dir}")


if __name__ == "__main__":
    sys.exit(main() or 0)

#!/usr/bin/env python3
"""
Measures audio/video sync through a transcode, with ffmpeg as the reference
receiver: both the original and the result are decoded by it, and the two
decoded signals are slid against each other until they match.

    python3 tests/av_sync.py original.ts result.ts [number of audio streams]

The figure printed is where the result's audio sits in time relative to
the original's, both read off their own time stamps. Zero means a viewer
hears each sound at the moment the original would have played it.
"""
import subprocess, sys, json
import numpy as np

def start_pts(path, idx):
    out = subprocess.run(["ffprobe","-v","error","-select_streams",f"a:{idx}","-show_entries","packet=pts_time","-read_intervals","%+#1","-of","csv=p=0",path],capture_output=True,text=True).stdout.split()
    return float(out[0].strip(','))

def decode(path, idx, rate=48000):
    raw = subprocess.run(["ffmpeg","-v","error","-i",path,"-map",f"0:a:{idx}","-ac","1","-ar",str(rate),"-f","s16le","-"],capture_output=True).stdout
    return np.frombuffer(raw, dtype=np.int16).astype(np.float64)

def offset_ms(src, si, out, oi, rate=48000):
    a = decode(src, si, rate); b = decode(out, oi, rate)
    ta = start_pts(src, si); tb = start_pts(out, oi)
    n = 1 << int(np.ceil(np.log2(len(a) + len(b))))
    fa = np.fft.rfft(a, n); fb = np.fft.rfft(b, n)
    cc = np.fft.irfft(fb * np.conj(fa), n)
    lag = int(np.argmax(cc))
    if lag > n // 2: lag -= n
    # b[i + lag] ~ a[i]  -> content in out appears lag samples later in its own array
    return ((tb + lag / rate) - ta) * 1000.0, len(a), len(b)

if __name__ == "__main__":
    src, out = sys.argv[1], sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    for i in range(n):
        ms, la, lb = offset_ms(src, i, out, i)
        print(f"audio {i}: content is {ms:+.2f} ms off  (decoded {la} vs {lb} samples)")

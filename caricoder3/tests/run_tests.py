#!/usr/bin/env python3
"""
caricoder3 test suite.

    python3 tests/run_tests.py            everything
    python3 tests/run_tests.py file       only tests whose name contains "file"
    python3 tests/run_tests.py --list

File tests run the AddressSanitizer build (so a memory error fails the
test even if the output happens to look right) and are deterministic.
Live tests run the normal build over UDP on the loopback interface in
real time. Nothing outside Python's standard library is needed; ffmpeg
and TSDuck are used for extra cross-checks when they are installed.
"""
import hashlib
import json
import os
import random
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import gen_ts                                           # noqa: E402
from tslib import Analysis, TS                          # noqa: E402
from ts_check import check, compare                     # noqa: E402
from udptools import Player, Capture                    # noqa: E402

BIN = os.path.join(ROOT, "caricoder3")
ASAN = os.path.join(ROOT, "caricoder3-asan")
# A sanitizer build older than the plain one is stale (sources changed since):
# it is not used, so the tests never run old code without saying so.
if os.path.exists(ASAN) and os.path.exists(os.path.join(ROOT, "caricoder3")) and \
        os.path.getmtime(ASAN) < os.path.getmtime(os.path.join(ROOT, "caricoder3")):
    print(f"note: {ASAN} is older than the plain binary; testing the plain one (make debug to refresh)")
    ASAN = ASAN + ".stale"
WORK = os.path.join(ROOT, "build", "test-work")
HZ = 27_000_000
TESTS = []
# Leaks that belong to a codec library, not to us, are listed in lsan.supp.
SAN_ENV = dict(os.environ, LSAN_OPTIONS="suppressions=" + os.path.join(HERE, "lsan.supp")
               + ":print_suppressions=0")


def test(fn):
    TESTS.append(fn)
    return fn


class Fail(Exception):
    pass


def need(cond, msg):
    if not cond:
        raise Fail(msg)


def w(name):
    return os.path.join(WORK, name)


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def write(path, data):
    with open(path, "wb") as f:
        f.write(data)
    return path


def read(path):
    with open(path, "rb") as f:
        return f.read()


def run_file(src, dst, *args, expect=0, binary=None):
    """Runs a file-to-file pass; returns the statistics JSON (or None)."""
    binary = binary or (ASAN if os.path.exists(ASAN) else BIN)
    sf = dst + ".json"
    if os.path.exists(sf):
        os.remove(sf)
    p = subprocess.run([binary, "-q", "-i", src, "-o", dst, "--stats-file", sf,
                        *args], capture_output=True, text=True, timeout=300, env=SAN_ENV)
    if "AddressSanitizer" in p.stderr or "runtime error" in p.stderr:
        raise Fail("sanitizer error:\n" + p.stderr[-2000:])
    if p.returncode != expect:
        raise Fail(f"exit status {p.returncode}, expected {expect}\n{p.stderr[-800:]}")
    return json.load(open(sf)) if os.path.exists(sf) else None


def must_pass(path, cbr=False, program=0, limits=None, allow_late=False):
    rep, fails = check(path, program, cbr, limits)
    if allow_late:
        fails = [f for f in fails if "after their DTS" not in f]
    need(not fails, f"{os.path.basename(path)}: " + "; ".join(fails))
    return rep


def must_match(orig, out, **kw):
    rep, fails = compare(orig, out, **kw)
    need(not fails, "content: " + "; ".join(fails))
    return rep


_cache = {}


def synth(name, duration=6.0, muxrate=4_000_000, pcr_base=10.0, mpts=False,
          separate_pcr=False, seed=1, programs=None, **extra):
    """Generates (once) a synthetic stream and returns its path."""
    path = w(name + ".ts")
    if name in _cache:
        return path
    if programs is None:
        programs = [gen_ts.default_program(separate_pcr=separate_pcr)]
        if mpts:
            programs.insert(0, gen_ts.default_program(
                2000, video_rate=1_500_000, sections=False, name="Other"))
    cfg = {"muxrate": muxrate, "duration": duration, "seed": seed,
           "pcr_base": int(pcr_base * HZ), "programs": programs, **extra}
    write(path, gen_ts.build(cfg))
    _cache[name] = True
    return path


# ---------------------------------------------------------------- file tests

@test
def file_cbr_exact():
    """CBR remux: conformant, PCR exact, every PES and section identical."""
    src = synth("syn")
    st = run_file(src, w("cbr.ts"), "-r", "4M")
    rep = must_pass(w("cbr.ts"), cbr=True)
    must_match(src, w("cbr.ts"))
    need(rep["bitrate"] == 4_000_000, f"bitrate {rep['bitrate']}")
    need(st["output"]["dropped_units"] == 0 and st["output"]["late_units"] == 0,
         "units dropped or late")
    need(st["input"]["cc_errors"] == 0, "false continuity errors on a clean input")
    return f"PCR accuracy {rep['pcr_accuracy_ns']} ns, PCR interval " \
           f"{rep['pcr_interval_max_ms']} ms, {sum(rep['pes'].values())} PES"


@test
def file_drop_streams():
    """--drop leaves the named streams out: gone from the PMT and from the packets; the rest exact."""
    src = synth("syn")
    out = w("drop.ts")
    st = run_file(src, out, "-r", "4M", "--drop", "1653,0x1FF0")
    rep = must_pass(out, cbr=True)
    a = Analysis(out)
    pids = {e["pid"] for e in a.pmt["streams"]}
    need(pids == {1651, 1652}, f"PMT lists {sorted(pids)}")
    need(1653 not in a.pid_packets and 0x1FF0 not in a.pid_packets, "dropped PIDs still have packets")
    must_match(src, out, only={1651, 1652})
    need(st["service"]["programs"] == 1 and len(st["service"]["streams"]) == 2, f"stats: {st['service']}")
    return "AC-3 and the section PID left out; video and AAC exact"


@test
def file_follows_pid_change():
    """The source moves its streams and PMT to other PIDs mid-run: settings still apply, the output keeps its PIDs; remembered for the next run."""
    a = synth("follow_a", duration=4.0)
    pb = gen_ts.default_program(base_pid=2400)
    b = synth("follow_b", duration=4.0, pcr_base=14.0, programs=[pb])
    both = write(w("follow_ab.ts"), read(a) + read(b))
    sfile = w("follow.streams")
    if os.path.exists(sfile):
        os.remove(sfile)
    out = w("follow_out.ts")
    # settings by the first PIDs: AC-3 (1653) left out, the AAC (1652) sent as 500
    st = run_file(both, out, "-r", "4M", "--streams-file", sfile, "--drop", "1653", "--remap", "1652=500")
    must_pass(out, cbr=True)
    o, ia, ib = Analysis(out), Analysis(a), Analysis(b)
    pids = sorted(o.pid_packets)
    need(not ({2401, 2402, 2403} & set(o.pid_packets)) and 1653 not in o.pid_packets,
         f"the source's new PIDs or the dropped stream reached the output: {pids}")
    need(o.pmt_pid == 1650, f"output PMT PID {o.pmt_pid}")
    want_v = len(ia.pes.get(1651, [])) + len(ib.pes.get(2401, []))
    want_a = len(ia.pes.get(1652, [])) + len(ib.pes.get(2402, []))
    got_v, got_a = len(o.pes.get(1651, [])), len(o.pes.get(500, []))
    need(got_v >= want_v - 2 and got_a >= want_a - 2, f"video {got_v}/{want_v}, audio {got_a}/{want_a} PES")
    kept = read(sfile).decode()
    need("0x0674 0x0962" in kept and "pmt 0x0672 0x0960" in kept, f"streams file:\n{kept}")
    srcs = {x["pid"]: x.get("src_pid") for x in st["service"]["streams"]}
    need(srcs.get(500) == 2402, f"stats do not say where the source has it now: {st['service']['streams']}")
    # the next run starts on the new PIDs only: the file says what they were
    out2 = w("follow_out2.ts")
    run_file(b, out2, "-r", "4M", "--streams-file", sfile, "--drop", "1653", "--remap", "1652=500")
    o2 = Analysis(out2)
    need(sorted(o2.pid_packets) == pids, f"restart: {sorted(o2.pid_packets)}, before {pids}")
    # and without the file it follows the source as before
    out3 = w("follow_out3.ts")
    run_file(b, out3, "-r", "4M")
    need(2402 in Analysis(out3).pid_packets, "without a streams file the source's PIDs should pass")
    return f"video {got_v}/{want_v}, audio {got_a}/{want_a} PES across the change; output PIDs {pids} in both runs"


@test
def file_output_ids():
    """--remap, --order, --pmt-pid, --pcr-pid, --service-id, --ts-id, --onid: the output's identifiers as asked, content unchanged."""
    src = synth("syn")
    out = w("ids.ts")
    st = run_file(src, out, "-r", "4M", "--remap", "1651=481,1652=0x1E2", "--order", "1653,1651",
                  "--pmt-pid", "4000", "--pcr-pid", "600", "--service-id", "1840", "--ts-id", "77", "--onid", "99")
    must_pass(out, cbr=True)
    a = Analysis(out)
    b = Analysis(src)
    order = [e["pid"] for e in a.pmt["streams"]]
    need(order == [1653, 481, 482, 0x1FF0], f"PMT order {order}")
    need(a.pmt_pid == 4000 and a.pmt["pcr_pid"] == 600 and a.pmt["program"] == 1840, f"PMT PID {a.pmt_pid}, PMT {a.pmt}")
    need(a.pat["ts_id"] == 77 and a.pat["programs"] == {1840: 4000}, f"PAT {a.pat}")
    need(a.pid_packets.get(600, 0) > 100 and len(a.pcrs) > 100, f"PCR-only PID: {a.pid_packets.get(600)} packets")
    need(1651 not in a.pid_packets and 1652 not in a.pid_packets, "the old PIDs still have packets")
    for old, new in ((1651, 481), (1652, 482), (1653, 1653)):
        got = [p.get("sha_nots") for p in a.pes.get(new, [])]
        want = [p.get("sha_nots") for p in b.pes.get(old, [])]
        need(got and got == want, f"PID {old} -> {new}: content differs ({len(got)} vs {len(want)} PES)")
    sv = st["service"]
    need((sv["number"], sv["ts_id"], sv["pmt_pid"], sv["pcr_pid"]) == (1840, 77, 4000, 600) and
         [x["pid"] for x in sv["streams"]] == [1653, 481, 482, 0x1FF0] and
         [x["in_pid"] for x in sv["streams"]] == [1653, 1651, 1652, 0x1FF0], f"stats: {sv}")
    # PCR on the video, and a PID asked for that an untouched stream had: that one moves
    out2 = w("ids2.ts")
    run_file(src, out2, "-r", "4M", "--remap", "1651=1652", "--pcr-pid", "video")
    a2 = Analysis(out2)
    pids = [e["pid"] for e in a2.pmt["streams"]]
    need(pids[0] == 1652 and a2.pmt["pcr_pid"] == 1652 and len(set(pids)) == len(pids), f"collision: {pids}, PCR {a2.pmt['pcr_pid']}")
    return "PIDs, order, PMT PID, PCR-only PID, service/TS/network ids as asked; a clashing PID moved"


@test
def probe_file_and_live():
    """caricoder3 probe: programs, streams, formats and rates, from a file and from UDP."""
    src = synth("syn_mpts", mpts=True)
    p = subprocess.run([BIN, "probe", src], capture_output=True, text=True)
    need(p.returncode == 0, f"probe exit {p.returncode}: {p.stdout[-300:]}")
    rep = json.loads(p.stdout)
    need(rep["ok"] and len(rep["programs"]) == 2, f"programs: {rep.get('programs')}")
    prog = {q["number"]: q for q in rep["programs"]}
    need(prog[1650]["name"] == "Vacation" and prog[2000]["name"] == "Other", f"names: {prog}")
    st = {x["pid"]: x for x in prog[1650]["streams"]}
    need(st[1651]["kind"] == "video" and st[1652]["kind"] == "audio" and st[1652]["language"] == "eng"
         and st[1653]["codec"] == "ac3" and st[0x1FF0]["kind"] == "data", f"streams: {st}")
    need(1_500_000 < st[1651]["bitrate"] < 2_600_000, f"video bit rate {st[1651]['bitrate']}")
    need(3_800_000 < rep["bitrate"] < 4_200_000, f"mux bit rate {rep['bitrate']}")
    # a real stream: the parsers say what the pictures and the sound are
    real = _real_h264_aac(8)
    if real:
        p = subprocess.run([BIN, "probe", real], capture_output=True, text=True)
        rep2 = json.loads(p.stdout)
        v = [x for x in rep2["programs"][0]["streams"] if x["kind"] == "video"][0]
        a = [x for x in rep2["programs"][0]["streams"] if x["kind"] == "audio"][0]
        need((v["width"], v["height"], v["interlaced"]) == (640, 360, False) and abs(v["fps"] - 25) < 0.1,
             f"video format: {v}")
        need(a["sample_rate"] == 48000 and a["channels"] == 1, f"audio format: {a}")
    # live, over UDP, for 2 seconds
    pin = _port[0]
    _port[0] += 2
    pl = Player("127.0.0.1", pin)
    import threading
    t = threading.Thread(target=lambda: pl.play(src), daemon=True)
    t.start()
    time.sleep(0.3)
    p = subprocess.run([BIN, "probe", f"udp://127.0.0.1:{pin}", "--seconds", "2", "--program", "1650"],
                       capture_output=True, text=True, timeout=20)
    t.join()
    pl.close()
    need(p.returncode == 0, f"live probe exit {p.returncode}: {p.stdout[-300:]}")
    rep3 = json.loads(p.stdout)
    need(len(rep3["programs"]) == 1 and rep3["programs"][0]["number"] == 1650 and
         3_500_000 < rep3["bitrate"] < 4_500_000, f"live: {rep3['bitrate']} b/s, {len(rep3['programs'])} programs")
    # a wrong address says so, without hanging
    p = subprocess.run([BIN, "probe", "udp://127.0.0.1:1", "--seconds", "1"], capture_output=True, text=True, timeout=30)
    need(p.returncode == 1 and not json.loads(p.stdout)["ok"], "a silent address must fail")
    return f"2 programs, formats read, live {rep3['bitrate'] / 1e6:.2f} Mb/s over UDP"


@test
def file_vbr_exact():
    """VBR remux: no null packets, content identical."""
    src = synth("syn")
    run_file(src, w("vbr.ts"))
    must_pass(w("vbr.ts"))
    must_match(src, w("vbr.ts"))
    a = Analysis(w("vbr.ts"))
    need(a.pid_packets.get(0x1FFF, 0) == 0, "null packets in a VBR output")
    saved = 100 - 100 * os.path.getsize(w("vbr.ts")) / os.path.getsize(src)
    return f"{saved:.0f}% smaller than the CBR input"


@test
def file_deterministic():
    """The same input always gives byte-identical output."""
    src = synth("syn")
    run_file(src, w("det1.ts"), "-r", "4M")
    run_file(src, w("det2.ts"), "-r", "4M", binary=BIN)
    need(sha(w("det1.ts")) == sha(w("det2.ts")), "two runs differ")
    return "sanitizer and release builds agree bit for bit"


@test
def file_mpts_select():
    """Picks one service out of a multi-program stream, by number and by default."""
    src = synth("mpts", mpts=True, separate_pcr=True, muxrate=7_000_000)
    run_file(src, w("mpts1650.ts"), "-r", "4M", "-p", "1650")
    must_pass(w("mpts1650.ts"), cbr=True)
    must_match(src, w("mpts1650.ts"), program=1650)
    a = Analysis(w("mpts1650.ts"))
    need(list(a.pat["programs"]) == [1650], "PAT lists more than the chosen service")
    need(set(a.sdt["services"]) == {1650}, "SDT lists more than the chosen service")
    need(a.pmt["pcr_pid"] == 1658, "separate PCR PID not preserved")

    run_file(src, w("mptsdef.ts"), "-r", "3M")           # default = first = 2000
    must_pass(w("mptsdef.ts"), cbr=True)
    must_match(src, w("mptsdef.ts"), program=2000)
    return "service 1650 (PCR on its own PID) and default service 2000"


@test
def file_timestamp_wrap():
    """33-bit PCR/PTS rollover in the middle of the stream."""
    wrap_s = (1 << 33) / 90000
    src = synth("wrap", pcr_base=wrap_s - 3.0)
    raw = [p for _, p in Analysis(src).pcrs]
    need(any(b < a for a, b in zip(raw, raw[1:])), "test stream does not wrap")
    st = run_file(src, w("wrap_out.ts"), "-r", "4M")
    must_pass(w("wrap_out.ts"), cbr=True)
    must_match(src, w("wrap_out.ts"))
    need(st["clock"]["discontinuities"] == 0, "wrap was mistaken for a discontinuity")
    return "rollover crossed with no discontinuity, content identical"


def _restart(name, base_a, base_b):
    a = synth(name + "_a", duration=4.0, pcr_base=base_a, seed=11)
    b = synth(name + "_b", duration=4.0, pcr_base=base_b, seed=12)
    src = write(w(name + ".ts"), read(a) + read(b))
    st = run_file(src, w(name + "_out.ts"), "-r", "4M")
    rep = must_pass(w(name + "_out.ts"), cbr=True, allow_late=True)
    must_match(src, w(name + "_out.ts"), subset=True, shifted=True)
    need(st["clock"]["discontinuities"] == 1,
         f"{st['clock']['discontinuities']} discontinuities, expected 1")
    out = Analysis(w(name + "_out.ts"))
    orig = Analysis(src)
    lost = sum(len(v) for v in orig.pes.values()) - sum(len(v) for v in out.pes.values())
    need(lost <= 8, f"{lost} PES packets lost around the restart")
    return f"offset {st['clock']['ts_offset_ms'] / 1000:+.1f} s applied, " \
           f"PCR interval max {rep['pcr_interval_max_ms']} ms, {lost} PES lost at the join"


@test
def file_source_restart_forward():
    """Source restarts on a later clock: output clock runs on unbroken."""
    return _restart("rst_fwd", 10.0, 5000.0)


@test
def file_source_restart_backward():
    """Source restarts on an earlier clock: output clock runs on unbroken."""
    return _restart("rst_back", 5000.0, 10.0)


@test
def file_pmt_change():
    """Stream line-up changes mid-stream: new PMT version, new stream carried."""
    pa = gen_ts.default_program(sections=False)
    pb = gen_ts.default_program(sections=False)
    pb["streams"] = [pb["streams"][0], pb["streams"][2],
                     {"pid": 0x690, "type": 0x03, "kind": "audio", "bitrate": 96_000}]
    a = synth("pmtchg_a", duration=4.0, pcr_base=10.0, programs=[pa], seed=21)
    b = synth("pmtchg_b", duration=4.0, pcr_base=14.0, programs=[pb], seed=22)
    src = write(w("pmtchg.ts"), read(a) + read(b))
    st = run_file(src, w("pmtchg_out.ts"), "-r", "4M")
    must_pass(w("pmtchg_out.ts"), cbr=True, allow_late=True)
    out = Analysis(w("pmtchg_out.ts"))
    need(len(set(out.pmt_versions)) == 2, f"PMT versions seen: {set(out.pmt_versions)}")
    need([s["pid"] for s in out.pmt["streams"]] == [1651, 1653, 0x690],
         "final PMT does not list the new line-up")
    need(len(out.pes.get(0x690, [])) > 50, "new stream not carried")
    need(st["service"]["changes"] >= 2, "service change not counted")
    v = sorted(set(out.pmt_versions))
    return f"PMT version {v[0]} -> {v[1]}, new PID 0x0690 carried"


@test
def file_pid_changes_hands():
    """PMT moves onto the old video PID and video onto the old PMT PID: the output keeps its layout, video carried throughout."""
    pa = gen_ts.default_program(sections=False)
    pb = gen_ts.default_program(sections=False)
    pb["pmt_pid"], pb["streams"][0]["pid"] = pa["streams"][0]["pid"], pa["pmt_pid"]
    pb["pcr_pid"] = pb["streams"][0]["pid"]
    a = synth("swap_a", duration=3.0, pcr_base=10.0, programs=[pa], seed=41)
    b = synth("swap_b", duration=3.0, pcr_base=13.0, programs=[pb], seed=42)
    src = write(w("swap.ts"), read(a) + read(b))
    run_file(src, w("swap_out.ts"), "-r", "4M")
    rep = must_pass(w("swap_out.ts"), cbr=True, allow_late=True)
    out = Analysis(w("swap_out.ts"))
    need(out.pmt_pid == 1650 and out.pmt["pcr_pid"] == 1651, f"layout changed: PMT {out.pmt_pid}, PCR {out.pmt['pcr_pid']}")
    va, vb = len(Analysis(a).pes.get(1651, [])), len(Analysis(b).pes.get(1650, []))
    need(len(out.pes.get(1651, [])) >= va + vb - 2 and 1650 not in out.pes, f"video {len(out.pes.get(1651, []))} of {va + vb} PES on 1651")
    return f"layout kept, all video on its PID, {rep['cc_errors']} CC errors"


@test
def file_vbr_input():
    """Null-stripped input with huge key frames sent in bursts (ffmpeg-style VBR)."""
    prog = gen_ts.default_program(sections=False)
    prog["streams"][0].update(bitrate=1_500_000, i_scale=12.0, burst=True, vbv=0.6)
    cbr = synth("vbrin_cbr", duration=8.0, muxrate=20_000_000, programs=[prog], seed=51)
    data = read(cbr)
    stripped = b"".join(data[i:i + TS] for i in range(0, len(data), TS)
                        if data[i + 1:i + 3] != b"\x1f\xff")
    src = write(w("vbrin.ts"), stripped)
    a = Analysis(src)
    p = a.unwrapped_pcrs()
    gaps = [b[0] - a_[0] for a_, b in zip(p, p[1:])]
    secs = [(b[1] - a_[1]) / HZ for a_, b in zip(p, p[1:])]
    # The stream must contain what once went wrong: a PCR interval holding
    # more packets than half a second at the rate of the interval before.
    need(any(g > max(100, 0.5 * gp / sp)
             for gp, sp, g in zip(gaps, secs, gaps[1:])),
         "test stream is not bursty enough to mean anything")
    need(a.late_pes() == 0, "test stream itself is late")
    st = run_file(src, w("vbrin_out.ts"))
    must_pass(w("vbrin_out.ts"))
    must_match(src, w("vbrin_out.ts"))
    need(st["input"]["pcr_missing"] == 0,
         f"{st['input']['pcr_missing']} units timed as if the PCR had stopped")
    need(st["output"]["late_units"] == 0 and st["output"]["dropped_units"] == 0,
         "units late or dropped")
    return f"packets between PCRs range {min(gaps)}..{max(gaps)}; nothing late, " \
           f"nothing mistimed, content identical"


@test
def file_bad_pmt_ignored():
    """A PMT that passes its CRC but is malformed inside leaves the service alone."""
    src = synth("syn")
    data = bytearray(read(src))
    good = Analysis(src)
    pmt_pkts = [i for i in range(len(data) // TS)
                if ((data[i * TS + 1] & 0x1F) << 8 | data[i * TS + 2]) == good.pmt_pid]
    victim = pmt_pkts[len(pmt_pkts) // 2]
    # version 1, first stream fine, second claims 200 descriptor bytes that are not there
    body = bytes([0x06, 0x72, 0xC3, 0, 0, 0xE6, 0x73, 0xF0, 0,
                  0x1B, 0xE6, 0x73, 0xF0, 0,
                  0x0F, 0xE6, 0x74, 0xF0, 200, 1, 2, 3, 4, 5, 6])
    bad = gen_ts.section(0x02, body)
    hdr = bytes(data[victim * TS:victim * TS + 4])
    data[victim * TS:(victim + 1) * TS] = hdr + b"\x00" + bad + b"\xFF" * (TS - 5 - len(bad))
    write(w("badpmt.ts"), bytes(data))
    st = run_file(w("badpmt.ts"), w("badpmt_out.ts"), "-r", "4M")
    must_pass(w("badpmt_out.ts"), cbr=True)
    must_match(src, w("badpmt_out.ts"))
    out = Analysis(w("badpmt_out.ts"))
    need(len(set(out.pmt_versions)) == 1, "the bad table reached the output")
    need(st["input"]["psi_errors"] >= 1, "bad table not counted")
    need(st["service"]["changes"] <= 2, "service was re-announced")
    return "bad table counted and ignored; all four streams carried to the end"


def _corrupt_pcrs(src, dst, edits):
    """Rewrites the PCR of the n-th PCR packet (video PID) by `shift` seconds."""
    data = bytearray(read(src))
    seen = 0
    for i in range(len(data) // TS):
        p = i * TS
        pid = (data[p + 1] & 0x1F) << 8 | data[p + 2]
        if pid != 1651 or not (data[p + 3] & 0x20) or data[p + 4] < 7 or not (data[p + 5] & 0x10):
            continue
        if seen in edits:
            base = (data[p + 6] << 25) | (data[p + 7] << 17) | (data[p + 8] << 9) | \
                   (data[p + 9] << 1) | (data[p + 10] >> 7)
            ext = ((data[p + 10] & 1) << 8) | data[p + 11]
            v = base * 300 + ext + int(edits[seen] * HZ)
            data[p + 6:p + 12] = gen_ts.pcr_bytes(v)
        seen += 1
    return write(dst, bytes(data))


@test
def file_corrupt_pcr():
    """One PCR with a wrong value, from 4 s behind to 4 s ahead, is never
    believed: no break, no move of the time line, nothing lost."""
    src = synth("syn")
    shifts = (-4.0, -0.9, -0.6, -0.3, -0.05, 0.12, 0.15, 0.18, 0.6, 4.0)
    for shift in shifts:
        bad = _corrupt_pcrs(src, w("badpcr.ts"), {60: shift})
        st = run_file(bad, w("badpcr_out.ts"), "-r", "4M")
        what = f"PCR off by {shift:+.2f} s: "
        # A value a little ahead is only recognised by the PCR after it,
        # so the packets just before it leave up to that much late.
        must_pass(w("badpcr_out.ts"), cbr=True, allow_late=0 < shift < 0.2)
        must_match(src, w("badpcr_out.ts"))
        need(st["input"]["pcr_glitches"] == 1, what + f"{st['input']['pcr_glitches']} glitches counted")
        need(st["input"]["pcr_jumps"] == 0, what + "taken for a break in the clock")
        need(st["clock"]["discontinuities"] == 0 and st["clock"]["ts_offset_ms"] == 0,
             what + "time line moved")
        need(st["input"]["pes_dropped"] == 0 and st["output"]["dropped_units"] == 0,
             what + "content dropped")
        late = Analysis(w("badpcr_out.ts")).lateness_ms()
        need(len(late) <= 6 and max(late, default=0) < 200,
             what + f"{len(late)} PES late, worst {max(late, default=0):.0f} ms")
    # Several in one stream.
    bad = _corrupt_pcrs(src, w("badpcr.ts"), {60: 4.0, 110: -4.0, 150: 0.6})
    st = run_file(bad, w("badpcr_out.ts"), "-r", "4M")
    must_pass(w("badpcr_out.ts"), cbr=True)
    must_match(src, w("badpcr_out.ts"))
    need(st["input"]["pcr_glitches"] == 3 and st["input"]["pcr_jumps"] == 0,
         f"{st['input']['pcr_glitches']} glitches, {st['input']['pcr_jumps']} jumps")
    return f"{len(shifts)} wrong values, each set aside and counted; content identical, " \
           f"time line untouched"


@test
def file_pcr_on_pmt_pid():
    """PCR carried on the PMT's own PID."""
    prog = gen_ts.default_program(sections=False)
    prog["pcr_pid"] = prog["pmt_pid"]
    src = synth("pcrpmt", programs=[prog], seed=52)
    need(Analysis(src).total_cc_errors() == 0, "test stream itself is broken")
    run_file(src, w("pcrpmt_out.ts"), "-r", "4M")
    rep = must_pass(w("pcrpmt_out.ts"), cbr=True)
    must_match(src, w("pcrpmt_out.ts"))
    need(Analysis(w("pcrpmt_out.ts")).pmt["pcr_pid"] == prog["pmt_pid"], "PCR PID moved")
    return f"continuity intact on the shared PID, PCR accuracy {rep['pcr_accuracy_ns']} ns"


def _pcr_stop_stream():
    """14 s stream whose video (the PCR PID) turns into null packets after 3 s."""
    src = synth("pcrstop_src", duration=14.0)
    path = w("pcrstop.ts")
    if not os.path.exists(path):
        data = read(src)
        cut = len(data) // TS * 3 // 14
        out = bytearray()
        for i in range(len(data) // TS):
            pid = (data[i * TS + 1] & 0x1F) << 8 | data[i * TS + 2]
            out += gen_ts.NULL if (i > cut and pid == 1651) else data[i * TS:(i + 1) * TS]
        write(path, bytes(out))
    return src, path


@test
def file_pcr_stops():
    """The PCR (and video) stop while audio carries on: audio still gets out."""
    src, cut_src = _pcr_stop_stream()
    st = run_file(cut_src, w("pcrstop_out.ts"), "-r", "4M")
    must_pass(w("pcrstop_out.ts"), cbr=True, allow_late=True)
    must_match(src, w("pcrstop_out.ts"), subset=True)
    a, b = Analysis(src), Analysis(w("pcrstop_out.ts"))
    need(len(b.pes[1652]) >= len(a.pes[1652]) - 3,
         f"audio: {len(b.pes[1652])} of {len(a.pes[1652])} PES")
    # the one video PES that was cut off mid-way is rightly discarded
    need(st["output"]["dropped_units"] <= 1, f"{st['output']['dropped_units']} units dropped")
    return f"audio {len(b.pes[1652])}/{len(a.pes[1652])} PES over 11 s with no PCR " \
           f"from the source; output PCR kept running"


@test
def file_backdated_start():
    """Data far ahead of the first PCR cannot stretch the output."""
    pat = gen_ts.build_pat(1, [{"number": 100, "pmt_pid": 0x20}])
    pmt = gen_ts.build_pmt({"number": 100, "pcr_pid": 0x100, "streams":
                            [{"pid": 0x100, "type": 0x1B}]})
    pkts = []
    for pid, sec in ((0, pat), (0x20, pmt)):
        pkts.append(gen_ts.ts_packet(pid, 0, True, b"\x00" + sec, pad_payload=True)[0])
    pcr0 = 36000 * HZ
    pes = gen_ts.pes(0xE0, b"\xAA" * 100, pcr0 // 300 + 9000, bounded=False)
    pkts.append(gen_ts.ts_packet(0x100, 0, True, pes)[0])
    pkts += [gen_ts.NULL] * 3000
    pkts.append(gen_ts.pcr_only(0x100, 0, pcr0))
    pkts.append(gen_ts.pcr_only(0x100, 0, pcr0 + HZ))    # one packet = "1 second"
    pkts.append(gen_ts.ts_packet(0x100, 1, True, pes)[0])
    pkts += [gen_ts.NULL] * 10
    write(w("backdate.ts"), b"".join(pkts))
    run_file(w("backdate.ts"), w("backdate_out.ts"), "-r", "1M")
    secs = os.path.getsize(w("backdate_out.ts")) * 8 / 1e6
    need(secs < 5.0, f"a {len(pkts) * TS // 1000} kB input produced {secs:.0f} s of output")
    return f"{len(pkts) * TS // 1000} kB in, {secs:.1f} s of stream out"


@test
def file_service_switch():
    """Default service changes: the old service's name does not follow it."""
    p2000 = gen_ts.default_program(2000, video_rate=1_500_000, sections=False, name="Other")
    p1650 = gen_ts.default_program(sections=False, name="Vacation")
    def mk(name, order, base, seed):
        cfg = {"muxrate": 7_000_000, "duration": 3.0, "seed": seed,
               "pcr_base": int(base * HZ), "programs": order, "sdt_services": [2000]}
        return write(w(name), gen_ts.build(cfg))
    a = mk("sw_a.ts", [dict(p2000), dict(p1650)], 10.0, 61)
    b = mk("sw_b.ts", [dict(p1650), dict(p2000)], 13.0, 62)
    src = write(w("switch.ts"), read(a) + read(b))
    run_file(src, w("switch_out.ts"), "-r", "4M")
    must_pass(w("switch_out.ts"), cbr=True, allow_late=True,
              limits={"sdt_interval_ms": 1e9})
    out = Analysis(w("switch_out.ts"))
    need(out.program == 2000 and 1650 in out.pat["programs"],
         "output did not follow the change of service")
    need((1650, "Other") not in out.sdt_seen,
         "SDT announced the new service under the old one's name")
    return "service 2000 then 1650; SDT never mislabels the new one"


@test
def file_sdt_version_moves():
    """Different SDT content never goes out under the same version number."""
    p2000 = gen_ts.default_program(2000, video_rate=1_500_000, sections=False, name="Other")
    p1650 = gen_ts.default_program(sections=False, name="Vacation")
    def mk(name, order, base, seed):
        cfg = {"muxrate": 7_000_000, "duration": 3.0, "seed": seed, "sdt_version": 5,
               "pcr_base": int(base * HZ), "programs": order}
        return write(w(name), gen_ts.build(cfg))
    a = mk("sv_a.ts", [dict(p2000), dict(p1650)], 10.0, 71)
    b = mk("sv_b.ts", [dict(p1650), dict(p2000)], 13.0, 72)
    src = write(w("sdtver.ts"), read(a) + read(b))
    run_file(src, w("sdtver_out.ts"), "-r", "4M")
    out = Analysis(w("sdtver_out.ts"))
    need(out.total_cc_errors() == 0 and out.psi_crc_errors == 0, "output not clean")
    names = {n for _, n in out.sdt_seen}
    need(names == {"Other", "Vacation"}, f"SDT names seen: {names}")
    clash = {v: len(s) for v, s in out.sdt_by_version.items() if len(s) > 1}
    need(not clash, f"version(s) reused for different content: {clash}")
    return f"versions {sorted(out.sdt_by_version)}: one content each"


@test
def file_long_service_text():
    """Renaming a service whose provider text is very long keeps the SDT well formed."""
    prog = gen_ts.default_program(sections=False)
    prog["provider"] = "P" * 200
    src = synth("longsdt", duration=3.0, programs=[prog], seed=53)
    run_file(src, w("longsdt_out.ts"), "-r", "4M", "--service-name", "N" * 100)
    must_pass(w("longsdt_out.ts"), cbr=True)
    svc = Analysis(w("longsdt_out.ts")).sdt["services"][1650]
    need(svc["well_formed"], "SDT descriptor loop is malformed")
    need(svc["name"] == "N" * 100, "name not applied")
    return f"descriptor well formed (provider kept to {len(svc['provider'])} bytes)"


@test
def file_big_tables_low_rate():
    """Tables too big for the repetition rate asked for are never sent cut short."""
    prog = gen_ts.default_program(sections=False)
    for s in prog["streams"]:
        s["desc"] = bytes([0x80, 250]) + bytes(250)
    prog["info"] = bytes([0x80, 200]) + bytes(200)
    src = synth("bigpmt", duration=4.0, programs=[prog], seed=54)
    need(len(gen_ts.build_pmt(prog)) > 900, "PMT not big enough for this test")
    run_file(src, w("bigpmt_out.ts"), "-r", "150k", "--psi-interval", "20")
    out = Analysis(w("bigpmt_out.ts"))
    need(out.psi_crc_errors == 0 and out.total_cc_errors() == 0 and out.sync_errors == 0,
         f"crc {out.psi_crc_errors}, cc {out.cc_errors}")
    need(len(out.pmt_idx) > 10, "PMT hardly ever sent")
    return f"{len(out.pmt_idx)} complete PMTs of {len(gen_ts.build_pmt(prog))} bytes, none truncated"


@test
def file_service_rename():
    """--service-name / --service-provider rewrite the SDT."""
    src = synth("syn")
    run_file(src, w("rename.ts"), "-r", "4M", "--service-name", "Coop One",
             "--service-provider", "Coop Cable")
    must_pass(w("rename.ts"), cbr=True)
    must_match(src, w("rename.ts"))
    svc = Analysis(w("rename.ts")).sdt["services"][1650]
    need(svc["name"] == "Coop One" and svc["provider"] == "Coop Cable",
         f"SDT says {svc['name']!r} / {svc['provider']!r}")
    return "SDT carries the new name and provider"


def _drop(src, dst, rate, seed):
    data = read(src)
    rng = random.Random(seed)
    out = bytearray()
    i, n, dropped = 0, len(data) // TS, 0
    while i < n:
        if i > 200 and rng.random() < rate:
            # Bursts stay under 15 packets: a burst that removes an exact
            # multiple of 16 packets of one PID is invisible to the 4-bit
            # continuity counter, for any demultiplexer.
            burst = rng.choice([1, 1, 1, 2, 3, 5, 9])
            dropped += burst
            i += burst
            continue
        out += data[i * TS:(i + 1) * TS]
        i += 1
    write(dst, bytes(out))
    return dropped


@test
def file_packet_loss():
    """Lossy input: losses are counted, output stays a clean stream."""
    src = synth("syn")
    dropped = _drop(src, w("lossy.ts"), 0.003, 5)
    st = run_file(w("lossy.ts"), w("lossy_out.ts"), "-r", "4M")
    rep = must_pass(w("lossy_out.ts"), cbr=True, allow_late=True)
    must_match(src, w("lossy_out.ts"), subset=True)
    need(st["input"]["cc_errors"] > 0, "losses not detected")
    need(st["input"]["pes_dropped"] > 0, "damaged PES packets not discarded")
    return f"{dropped} packets removed, {st['input']['cc_errors']} CC errors seen, " \
           f"{st['input']['pes_dropped']} damaged PES discarded, output CC errors " \
           f"{rep['cc_errors']}, PCR accuracy {rep['pcr_accuracy_ns']} ns"


@test
def file_corruption():
    """Random bit errors anywhere in the input never break the output."""
    src = synth("syn")
    data = bytearray(read(src))
    rng = random.Random(9)
    for _ in range(400):
        pos = rng.randrange(TS * 300, len(data))
        data[pos] ^= 1 << rng.randrange(8)
    write(w("corrupt.ts"), bytes(data))
    run_file(w("corrupt.ts"), w("corrupt_out.ts"), "-r", "4M")
    rep = must_pass(w("corrupt_out.ts"), cbr=True, allow_late=True)
    return f"400 bit errors in, output: 0 sync / {rep['cc_errors']} CC / " \
           f"{rep['psi_crc_errors']} CRC errors"


@test
def file_resync():
    """Junk bytes in the stream (lost alignment): the demux finds sync again."""
    src = synth("syn")
    data = read(src)
    rng = random.Random(3)
    need(len(data) > TS * 14000, "test stream too short")
    cut = [TS * 5000 + 77, TS * 9000 + 3, TS * 13000]
    out = rng.randbytes(1234) + data[:cut[0]] + rng.randbytes(999) + \
        data[cut[0]:cut[1]] + data[cut[1] + 55:cut[2]] + b"\x47" * 500 + data[cut[2]:]
    write(w("junk.ts"), out)
    st = run_file(w("junk.ts"), w("junk_out.ts"), "-r", "4M")
    must_pass(w("junk_out.ts"), cbr=True, allow_late=True)
    must_match(src, w("junk_out.ts"), subset=True)
    orig = sum(len(v) for v in Analysis(src).pes.values())
    got = sum(len(v) for v in Analysis(w("junk_out.ts")).pes.values())
    need(got > orig * 0.97, f"only {got} of {orig} PES recovered")
    need(st["input"]["sync_errors"] >= 3, "sync losses not counted")
    return f"{st['input']['sync_errors']} sync losses, {got}/{orig} PES recovered"


@test
def file_not_a_stream():
    """Empty, tiny and random inputs: clean exit, no output, no crash."""
    rng = random.Random(1)
    cases = {"empty": b"", "tiny": b"\x47" * 100, "random": rng.randbytes(300000),
             "nulls": (b"\x47\x1f\xff\x10" + b"\xff" * 184) * 2000}
    for name, data in cases.items():
        write(w(f"bad_{name}.ts"), data)
        run_file(w(f"bad_{name}.ts"), w(f"bad_{name}_out.ts"), "-r", "4M", expect=2)
        need(os.path.getsize(w(f"bad_{name}_out.ts")) == 0, f"{name}: output not empty")
    return "4 non-streams rejected with exit status 2"


@test
def file_muxrate_too_low():
    """Output rate below the content rate: units are shed, stream stays valid."""
    src = synth("syn")
    st = run_file(src, w("low.ts"), "-r", "1500k")
    rep = must_pass(w("low.ts"), cbr=True, allow_late=True)
    must_match(src, w("low.ts"), subset=True)
    need(st["output"]["dropped_units"] > 0, "overflow not reported")
    return f"{st['output']['dropped_units']} units shed and counted, " \
           f"PCR accuracy still {rep['pcr_accuracy_ns']} ns"


@test
def file_low_delay():
    """A 60 ms buffer still gives a complete, conformant output."""
    src = synth("syn")
    st = run_file(src, w("lowdelay.ts"), "-r", "4M", "-d", "60")
    must_pass(w("lowdelay.ts"), cbr=True)
    must_match(src, w("lowdelay.ts"))
    return f"content identical, worst lateness {st['output']['max_late_ms']} ms"


@test
def file_fuzz_mutations():
    """300 randomly mangled inputs: never a crash, never an invalid output."""
    src = synth("fuzzseed", duration=1.5, mpts=True, muxrate=7_000_000, seed=4)
    seed_data = read(src)
    rng = random.Random(2026)
    for i in range(300):
        d = bytearray(seed_data)
        for _ in range(rng.randrange(1, 6)):
            op = rng.randrange(6)
            pos = rng.randrange(len(d))
            if op == 0:                                 # bit flips in a burst
                for k in range(rng.randrange(1, 200)):
                    d[rng.randrange(len(d))] ^= 1 << rng.randrange(8)
            elif op == 1:                               # truncate
                del d[pos:]
            elif op == 2:                               # delete a span
                del d[pos:pos + rng.randrange(1, 5000)]
            elif op == 3:                               # insert junk
                d[pos:pos] = rng.randbytes(rng.randrange(1, 3000))
            elif op == 4:                               # repeat a span
                span = bytes(d[pos:pos + rng.randrange(1, 20000)])
                d[pos:pos] = span
            else:                                       # scribble on headers
                for k in range(pos - pos % TS, min(len(d), pos + TS * 50), TS):
                    d[k + 1 + rng.randrange(3)] = rng.randrange(256)
            if not d:
                d = bytearray(b"\x47")
        write(w("fuzz_in.ts"), bytes(d))
        binary = ASAN if os.path.exists(ASAN) else BIN
        args = ["-r", "5M"] if i % 2 else []
        if i % 3 == 0:
            args += ["-p", "1650"]
        p = subprocess.run([binary, "-q", "-i", w("fuzz_in.ts"), "-o", w("fuzz_out.ts"),
                            *args], capture_output=True, text=True, timeout=120)
        if "Sanitizer" in p.stderr or "runtime error" in p.stderr or \
           p.returncode not in (0, 2):
            shutil.copy(w("fuzz_in.ts"), w(f"fuzz_crash_{i}.ts"))
            raise Fail(f"iteration {i}: exit {p.returncode}\n{p.stderr[-1500:]}")
        if os.path.getsize(w("fuzz_out.ts")):
            a = Analysis(w("fuzz_out.ts"))
            bad = a.sync_errors + a.total_cc_errors() + a.psi_crc_errors
            if bad or not a.pcr_monotonic():
                shutil.copy(w("fuzz_in.ts"), w(f"fuzz_bad_{i}.ts"))
                raise Fail(f"iteration {i}: invalid output (cc {a.cc_errors}, "
                           f"sync {a.sync_errors}, crc {a.psi_crc_errors}, "
                           f"monotonic {a.pcr_monotonic()})")
    return "300 mangled inputs, 0 crashes, 0 invalid outputs"


@test
def file_real_codecs():
    """Real H.264/HEVC/MPEG-2 + AAC/AC-3/MP2 streams (needs ffmpeg)."""
    if not shutil.which("ffmpeg"):
        return "SKIPPED: ffmpeg not installed"
    enc = [
        ("h264_aac", ["-c:v", "libx264", "-preset", "veryfast", "-b:v", "2500k",
                      "-maxrate", "2500k", "-bufsize", "2500k", "-bf", "2",
                      "-c:a", "aac", "-b:a", "128k"], "3500k"),
        ("hevc_ac3", ["-c:v", "libx265", "-preset", "ultrafast", "-b:v", "1500k",
                      "-x265-params", "log-level=error",
                      "-c:a", "ac3", "-b:a", "192k"], "2500k"),
        ("mpeg2_mp2", ["-c:v", "mpeg2video", "-b:v", "3M", "-maxrate", "3M",
                       "-bufsize", "1835k", "-bf", "2",
                       "-c:a", "mp2", "-b:a", "192k"], "4M"),
    ]
    done = []
    for name, codec, rate in enc:
        src = w(f"real_{name}.ts")
        if not os.path.exists(src):
            p = subprocess.run(
                ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                 "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25",
                 "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000",
                 "-t", "8", "-pix_fmt", "yuv420p", "-g", "25", *codec,
                 "-f", "mpegts", "-muxrate", rate, src],
                capture_output=True, text=True)
            if p.returncode:
                continue                                # encoder not in this ffmpeg
        out = w(f"real_{name}_out.ts")
        run_file(src, out, "-r", rate)
        must_pass(out, cbr=True)
        must_match(src, out)
        p = subprocess.run(["ffmpeg", "-v", "error", "-xerror", "-i", out,
                            "-f", "null", "-"], capture_output=True, text=True)
        need(p.returncode == 0 and not p.stderr.strip(),
             f"{name}: ffmpeg reports errors decoding the output: {p.stderr[:300]}")
        if shutil.which("tsp"):
            p = subprocess.run(["tsp", "-I", "file", out, "-P", "continuity",
                                "-P", "pcrverify", "--absolute", "--jitter-max", "2",
                                "-O", "drop"], capture_output=True, text=True)
            need("0 with jitter" in p.stderr and "continuity" not in
                 p.stderr.replace("* pcrverify", ""),
                 f"{name}: TSDuck disagrees: {p.stderr[-300:]}")
        done.append(name)
    need(done, "no test stream could be encoded")
    extra = " and TSDuck" if shutil.which("tsp") else ""
    return f"{', '.join(done)}: identical, decode clean in ffmpeg{extra}"


# ---------------------------------------------------------------- live tests

_port = [39000 + (os.getpid() % 500) * 2]


def live(feed, *args, tail=1.2, capture_name="live.ts", with_proc=False, env=None):
    """
    Starts caricoder3 UDP -> UDP on loopback, calls feed(player), then
    collects the capture and the statistics. Returns (stats, capture, log).
    """
    pin, pout = _port[0], _port[0] + 1
    _port[0] += 2
    sf = w(capture_name + ".json")
    if os.path.exists(sf):
        os.remove(sf)
    cap = Capture(pout)
    proc = subprocess.Popen(
        [BIN, "-i", f"udp://127.0.0.1:{pin}", "-o", f"udp://127.0.0.1:{pout}",
         "--stats-file", sf, "--stats-interval", "0", *args],
        stderr=subprocess.PIPE, text=True, env=env)
    try:
        time.sleep(0.3)
        player = Player("127.0.0.1", pin)
        if with_proc:
            feed(player, proc)
        else:
            feed(player)
        player.close()
        time.sleep(tail)
        st = json.load(open(sf)) if os.path.exists(sf) else None
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            _, log = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            _, log = proc.communicate()
            raise Fail("did not stop on SIGTERM")
        cap.stop(w(capture_name))
    need(proc.returncode == 0, f"exit status {proc.returncode}\n{log[-800:]}")
    need(st is not None, "no statistics file written")
    return st, cap, log


def wrong_clock(sources, out_path, slack_ms=400):
    """
    Number of PES packets in the output whose timestamps sit on the wrong
    clock: further ahead of the output PCR than anything in the sources is
    ahead of theirs. (Comparing content alone would not notice: a packet
    stamped for the wrong clock still has the right payload. The late side
    is covered by lateness_ms().)
    """
    most = max(max(Analysis(s).lead_ms()) for s in sources)
    return sum(1 for x in Analysis(out_path).lead_ms() if x > most + slack_ms)


def no_pause(cap):
    """
    The output must not stop while the input misbehaves. Measured on the
    wall clock, so the limit has to leave room for the test machine: on a
    busy two-core VM the same binary shows stray gaps of 30 to 140 ms at
    random moments, whatever the input is doing. A real pause in the
    program is longer and sits where the input event is. That the stream
    itself is unbroken is checked separately, on its own clock (PCR
    interval).
    """
    need(cap.max_gap_ms() < 250, f"output paused for {cap.max_gap_ms():.0f} ms")


def live_rtp(feed, *args, **kw):
    """Same, with the input wrapped in RTP."""
    def rtp_feed(player):
        player.rtp = True
        feed(player)
    return live(rtp_feed, *args, **kw)


@test
def live_cbr():
    """Live UDP, CBR: conformant capture, complete content, steady pacing."""
    src = synth("live_src", duration=10.0)
    # Pacing can only be judged on a run the test machine itself did not
    # disturb. The sender notes how late its own timer woke it; when the
    # whole machine stalls (a busy VM does, for tens of milliseconds), the
    # sender and the program under test stall together, and the output
    # shows a deviation of the same size that says nothing about the
    # program. Such a run is repeated.
    for attempt in range(4):
        box = {}

        def feed(p):
            p.play(src)
            box["wake"] = p.worst_wake * 1000

        st, cap, _ = live(feed, "-r", "4M", capture_name="live_cbr.ts")
        if box["wake"] < 5.0:
            break
    rep = must_pass(w("live_cbr.ts"), cbr=True)
    must_match(src, w("live_cbr.ts"), subset=True)
    orig = sum(len(v) for v in Analysis(src).pes.values())
    got = sum(len(v) for v in Analysis(w("live_cbr.ts")).pes.values())
    need(got >= orig - 3, f"only {got} of {orig} PES in the output")
    need(st["input"]["cc_errors"] == 0 and st["output"]["dropped_units"] == 0,
         "errors on a clean loopback run")
    need(st["clock"]["discontinuities"] == 0, "spurious discontinuity")
    need(st["input"]["pcr_missing"] == 0, "PCR treated as missing")
    pace = cap.pacing_error_ms(4_000_000)
    if box["wake"] >= 5.0:
        return f"{got}/{orig} PES, PCR accuracy {rep['pcr_accuracy_ns']} ns; pacing NOT " \
               f"judged: the test machine stalled in all 4 runs (last: {box['wake']:.0f} ms)"
    need(pace["p99"] < 5.0, f"pacing p99 {pace['p99']:.2f} ms on a quiet machine "
                            f"(sender woke at most {box['wake']:.1f} ms late)")
    return f"{got}/{orig} PES, PCR accuracy {rep['pcr_accuracy_ns']} ns, pacing " \
           f"median {pace['median']:.2f} ms / p99 {pace['p99']:.2f} ms, clock " \
           f"{st['clock']['drift_ppm']:+.0f} ppm"


@test
def live_vbr():
    """Live UDP, VBR output."""
    src = synth("live_src", duration=10.0)
    st, cap, _ = live(lambda p: p.play(src), capture_name="live_vbr.ts")
    must_pass(w("live_vbr.ts"))
    must_match(src, w("live_vbr.ts"), subset=True)
    a = Analysis(w("live_vbr.ts"))
    need(a.pid_packets.get(0x1FFF, 0) == 0, "null packets in VBR output")
    orig = sum(len(v) for v in Analysis(src).pes.values())
    got = sum(len(v) for v in a.pes.values())
    need(got >= orig - 3, f"only {got} of {orig} PES in the output")
    return f"{got}/{orig} PES, no null packets, {st['output']['bitrate'] / 1e6:.2f} Mb/s"


@test
def live_rtp_input():
    """RTP-wrapped input is detected and unwrapped."""
    src = synth("live_src", duration=10.0)
    st, _, _ = live_rtp(lambda p: p.play(src), "-r", "4M", capture_name="live_rtp.ts")
    must_pass(w("live_rtp.ts"), cbr=True)
    must_match(src, w("live_rtp.ts"), subset=True)
    need(st["input"]["rtp"] is True, "RTP not detected")
    need(st["input"]["cc_errors"] == 0, "continuity errors")
    return "RTP detected, output conformant"


@test
def live_stream_without_data():
    """A stream the PMT lists stops arriving while the rest goes on: said once, and once when it is back."""
    src = synth("live_src", duration=10.0)
    data = read(src)
    n = len(data) // 188
    keep = bytearray()
    for i in range(n):
        pkt = data[i * 188:(i + 1) * 188]
        pid = ((pkt[1] & 0x1F) << 8) | pkt[2]
        if pid == 1652 and 0.25 * n <= i < 0.85 * n:
            continue
        keep += pkt
    gap = write(w("live_nodata_src.ts"), bytes(keep))
    seen = {}

    def feed(p):
        p.play(gap, at={5.0: lambda: seen.update(st=json.load(open(w("live_nodata.ts.json"))))})
    st, cap, log = live(feed, "-r", "4M", capture_name="live_nodata.ts")
    warn = [l for l in log.splitlines() if "no data for" in l]
    back = [l for l in log.splitlines() if "data again" in l]
    need(len(warn) == 1 and "0x0674" in warn[0] and "audio eng" in warn[0], f"warnings: {warn or log[-600:]}")
    need(len(back) == 1 and "0x0674" in back[0], f"back: {back}")
    mid = {x["pid"]: x.get("idle_ms") for x in seen["st"]["service"]["streams"]}
    need(mid.get(1652, 0) > 1500 and mid.get(1651, 9999) < 1000, f"idle_ms mid-gap: {mid}")
    return f"{warn[0].split('  ')[-1].strip()}; then: {back[0].split('  ')[-1].strip()}"


def _content_faults_src():
    """Real H.264 + AAC: moving 4 s, black and silent 16 s, moving 4 s, a still picture 16 s, moving 4 s."""
    src = w("content_faults.ts")
    if os.path.exists(src):
        return src
    v = ("testsrc2=size=320x240:rate=25:d=4[v0];color=black:size=320x240:rate=25:d=16[v1];"
         "testsrc2=size=320x240:rate=25:d=4[v2];color=0x608090:size=320x240:rate=25:d=16[v3];"
         "testsrc2=size=320x240:rate=25:d=4[v4];"
         "sine=f=800:sample_rate=48000:d=4[a0];anullsrc=r=48000:cl=mono:d=16[a1];"
         "sine=f=800:sample_rate=48000:d=4[a2];sine=f=500:sample_rate=48000:d=16[a3];"
         "sine=f=800:sample_rate=48000:d=4[a4];"
         "[v0][a0][v1][a1][v2][a2][v3][a3][v4][a4]concat=n=5:v=1:a=1[v][a]")
    p = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-filter_complex", v,
                        "-map", "[v]", "-map", "[a]", "-pix_fmt", "yuv420p", "-g", "25",
                        "-c:v", "libx264", "-preset", "veryfast", "-b:v", "800k",
                        "-c:a", "aac", "-b:a", "96k", "-f", "mpegts", "-muxrate", "1500k", src],
                       capture_output=True, text=True)
    return None if p.returncode else src


@test
def live_content_faults():
    """Black picture, silent audio and a frozen picture are each reported once after 10 s, and when they end."""
    src = _content_faults_src()
    if not src:
        return "SKIPPED: this ffmpeg cannot make the test stream"
    st, cap, log = live(lambda p: p.play(src), "--thumb", w("content.jpg"), "--thumb-interval", "1",
                        capture_name="live_content.ts", tail=2.0)
    lines = log.splitlines()
    pick = lambda s: [l for l in lines if s in l]
    black, unblack = pick("picture: black for"), pick("no longer black")
    frozen, moving = pick("picture: frozen"), pick("moving again")
    silent, sound = pick("silent for"), pick("sound again")
    need(len(black) == 1 and len(unblack) == 1, f"black: {black} / {unblack}; content {st.get('content')}")
    need(len(frozen) == 1 and len(moving) == 1, f"frozen: {frozen} / {moving}")
    need(len(silent) == 1 and len(sound) == 1, f"silent: {silent} / {sound}\n{log[-1500:]}")
    order = [lines.index(x[0]) for x in (black, unblack, frozen, moving)]
    need(order == sorted(order), "reported out of order")
    need(lines.index(silent[0]) < lines.index(sound[0]) < lines.index(frozen[0]), "silence not where it was")
    need("content" in st and st["content"]["audio"], f"no content stats: {st.get('content')}")
    return "black, silent and frozen each said once after 10 s and once at the end"


def _shift_pts(data, pid, from_frac, delta_90k):
    """The stream with `pid`'s PTS/DTS moved by delta (90 kHz) from that share of it on."""
    out = bytearray(data)
    n = len(data) // 188
    def rw(b, off):
        v = ((b[off] >> 1) & 7) << 30 | b[off + 1] << 22 | (b[off + 2] >> 1) << 15 | b[off + 3] << 7 | b[off + 4] >> 1
        v = (v + delta_90k) % (1 << 33)
        b[off] = (b[off] & 0xF1) | ((v >> 29) & 0x0E)
        b[off + 1] = (v >> 22) & 0xFF
        b[off + 2] = ((v >> 14) & 0xFE) | 1
        b[off + 3] = (v >> 7) & 0xFF
        b[off + 4] = ((v << 1) & 0xFE) | 1
    for i in range(int(n * from_frac), n):
        p = i * 188
        if ((out[p + 1] & 0x1F) << 8 | out[p + 2]) != pid or not (out[p + 1] & 0x40):
            continue
        q = p + 4
        if out[p + 3] & 0x20:
            q += 1 + out[q]
        if q + 14 > p + 188 or out[q:q + 3] != b"\x00\x00\x01":
            continue
        flags = out[q + 7]
        if flags & 0x80:
            rw(out, q + 9)
        if flags & 0x40:
            rw(out, q + 14)
    return bytes(out)


@test
def live_av_timing_moves():
    """The source moves its audio 200 ms against its video halfway: the A/V timing it signals is tracked, and the move is reported once."""
    src = _real_h264_aac(40)
    if not src:
        return "SKIPPED: this ffmpeg cannot encode H.264/AAC"
    a = Analysis(src)
    apid = [e["pid"] for e in a.pmt["streams"] if e["type"] == 0x0F][0]
    shifted = write(w("av_shift_src.ts"), _shift_pts(read(src), apid, 0.55, 18000))
    env = dict(os.environ, CC3_SYNC_BASE_WINDOWS="3", CC3_SYNC_OVER_WINDOWS="2")
    st, cap, log = live(lambda p: p.play(shifted), capture_name="live_av_shift.ts", env=env, tail=1.5)
    base = [l for l in log.splitlines() if "(the baseline)" in l]
    moved = [l for l in log.splitlines() if "A/V timing moved by" in l]
    need(base, f"no baseline: {log[-1500:]}")
    import re
    got = [int(m.group(1)) for l in moved for m in [re.search(r"moved by \+?(-?\d+) ms", l)] if m]
    need(len(moved) == 1 and got and abs(got[0] - 200) <= 15, f"moves: {moved}\n{log[-1500:]}")
    s = st["sync"]["audio"][0]
    need(abs(s["shift_ms"] - 200) < 15, f"sync stats: {st['sync']}")
    return f"{base[0].split('): ')[-1].strip()}; then: {moved[0].split('): ')[-1].strip()}"


@test
def live_input_outage():
    """Input vanishes for 3 s: the output never stops and resumes cleanly."""
    src = synth("live_src", duration=10.0)
    st, cap, _ = live(lambda p: p.play(src, gate=lambda t: not 3.0 <= t < 6.0),
                      "-r", "4M", capture_name="live_outage.ts")
    rep = must_pass(w("live_outage.ts"), cbr=True, allow_late=True)
    must_match(src, w("live_outage.ts"), subset=True)
    no_pause(cap)
    need(st["clock"]["discontinuities"] == 0,
         "outage on an unchanged clock was treated as a new time base")
    need(st["clock"]["ts_offset_ms"] == 0, "time stamps were shifted")
    need(st["input"]["pcr_missing"] == 0, "PCR treated as missing after the outage")
    a_, b_ = Analysis(src), Analysis(w("live_outage.ts"))
    kept = sum(len(v) for v in b_.pes.values()) / sum(len(v) for v in a_.pes.values())
    need(kept > 0.66, f"only {kept:.0%} of the content came through a 30% outage")
    return f"longest output gap {cap.max_gap_ms():.1f} ms across a 3 s outage, " \
           f"PCR interval max {rep['pcr_interval_max_ms']} ms, time line unchanged"


@test
def live_failover_and_back():
    """Two inputs: the primary dies, the backup takes over; the primary returns and is taken back."""
    a = synth("live_a", duration=5.0, pcr_base=10.0, seed=31)
    b = synth("live_b", duration=5.0, pcr_base=7000.0, seed=32)
    pin, pin2 = _port[0], _port[0] + 1
    _port[0] += 2
    box = {}

    def feed(p, proc):
        import threading
        p2 = Player("127.0.0.1", pin2)
        # the backup plays all along (a different clock); the primary plays
        # a, dies for 6 s, then plays a again
        t = threading.Thread(target=lambda: [p2.play(b) for _ in range(4)], daemon=True)
        t.start()
        p.play(a)
        box["died"] = time.time()
        time.sleep(6.0)
        p.play(a)                       # back, and kept alive to the end
        box["back"] = time.time()
        p.play(a)
        t.join(timeout=25)
        p2.close()

    st, cap, log = live(feed, "-r", "4M", "-i", f"udp://127.0.0.1:{pin2}", "--switch-after", "1500",
                        "--retry-primary", "2", capture_name="live_failover.ts", with_proc=True, tail=2.0)
    need(st["input"]["switches"] >= 2, f"switches: {st['input']['switches']} (expected a switch away and one back)")
    srcs = st["input"]["sources"]
    need(len(srcs) == 2 and srcs[0]["state"] == "in use", f"sources at the end: {srcs}")
    need("switching" in log and "back on" in log, "the log does not tell of the switch and the return")
    rep = must_pass(w("live_failover.ts"), cbr=True, allow_late=True)
    out = Analysis(w("live_failover.ts"))
    have = {r["sha_nots"] for v in out.pes.values() for r in v}
    nb = sum(1 for v in Analysis(b).pes.values() for r in v if r["sha_nots"] in have)
    need(nb > 100, f"only {nb} PES of the backup came through")
    no_pause(cap)
    return f"{st['input']['switches']} switches, {nb} PES from the backup while the primary was down, " \
           f"output gap max {cap.max_gap_ms():.1f} ms, PCR interval max {rep['pcr_interval_max_ms']} ms"


@test
def live_source_restart():
    """Source stops and comes back on a different clock."""
    a = synth("live_a", duration=5.0, pcr_base=10.0, seed=31)
    b = synth("live_b", duration=5.0, pcr_base=7000.0, seed=32)

    def feed(p):
        p.play(a)
        time.sleep(1.5)
        p.play(b)

    st, cap, _ = live(feed, "-r", "4M", capture_name="live_restart.ts")
    rep = must_pass(w("live_restart.ts"), cbr=True, allow_late=True)
    both = write(w("live_ab.ts"), read(a) + read(b))
    must_match(both, w("live_restart.ts"), subset=True, shifted=True)
    need(st["clock"]["discontinuities"] == 1,
         f"{st['clock']['discontinuities']} discontinuities, expected 1")
    no_pause(cap)
    an_b, out = Analysis(b), Analysis(w("live_restart.ts"))
    out_v = len(out.pes[1651])
    need(out_v > len(an_b.pes[1651]) + 50, "second source not carried")
    # The new source must be there from its first packet: nothing of it may
    # be lost while the program makes up its mind about the new clock.
    have = {r["sha_nots"] for v in out.pes.values() for r in v}
    for pid, lst in an_b.pes.items():
        missing = [i for i, r in enumerate(lst) if r["sha_nots"] not in have]
        need(not [i for i in missing if i < len(lst) - 3],
             f"PID {pid}: PES {missing[:8]} of the new source missing")
    # only what was left stranded at the end of the first source may go
    need(st["output"]["dropped_units"] <= 6,
         f"{st['output']['dropped_units']} units dropped")
    late = out.lateness_ms()
    need(len(late) <= 6, f"{len(late)} PES left after their time")
    odd = wrong_clock([a, b], w("live_restart.ts"))
    need(odd == 0, f"{odd} PES carry the wrong clock's timestamps")
    return f"1 discontinuity absorbed, new source complete from its first packet, " \
           f"output gap max {cap.max_gap_ms():.1f} ms, PCR continuous " \
           f"(max interval {rep['pcr_interval_max_ms']} ms)"


@test
def live_source_stalls_and_stays_late():
    """Source freezes for 2.6 s and carries on that much late (seen on the real feed)."""
    src = synth("live_src", duration=10.0)
    st, cap, _ = live(lambda p: p.play(src, stall=(3.0, 2.6, 1.0)), "-r", "4M",
                      capture_name="live_late.ts")
    must_pass(w("live_late.ts"), cbr=True, allow_late=True)
    rep = must_match(src, w("live_late.ts"), subset=True, shifted=True)
    need(st["clock"]["discontinuities"] == 1,
         f"{st['clock']['discontinuities']} discontinuities, expected 1")
    need(2400 < st["clock"]["ts_offset_ms"] < 2800,
         f"time line moved by {st['clock']['ts_offset_ms']:.0f} ms, expected about +2600")
    need(st["output"]["dropped_units"] == 0,
         f"{st['output']['dropped_units']} units dropped across the step")
    a, b = Analysis(src), Analysis(w("live_late.ts"))
    orig = sum(len(v) for v in a.pes.values())
    got = sum(len(v) for v in b.pes.values())
    need(got >= orig - 3, f"only {got} of {orig} PES delivered")
    stale = [x for x in b.lateness_ms() if x > 100]
    need(len(stale) <= 3, f"{len(stale)} PES sent well after their time")
    odd = wrong_clock([src], w("live_late.ts"))
    need(odd == 0, f"{odd} PES carry the wrong clock's timestamps")
    no_pause(cap)
    return f"{got}/{orig} PES, 0 dropped, none late, time line moved " \
           f"{st['clock']['ts_offset_ms'] / 1000:+.2f} s once"


@test
def live_source_skips_ahead():
    """Source jumps 2 s forward in its own time (content skipped upstream)."""
    src = synth("live_src", duration=10.0)
    data = read(src)
    n = len(data) // TS
    cut_a, cut_b = n * 4 // 10 // 7 * 7, n * 6 // 10 // 7 * 7
    cut = write(w("skip.ts"), data[:cut_a * TS] + data[cut_b * TS:])
    st, cap, _ = live(lambda p: p.play(cut, rate=4_000_000), "-r", "4M",
                      capture_name="live_skip.ts")
    must_pass(w("live_skip.ts"), cbr=True, allow_late=True)
    must_match(src, w("live_skip.ts"), subset=True, shifted=True)
    need(st["clock"]["discontinuities"] == 1,
         f"{st['clock']['discontinuities']} discontinuities, expected 1")
    need(-2200 < st["clock"]["ts_offset_ms"] < -1800,
         f"time line moved by {st['clock']['ts_offset_ms']:.0f} ms, expected about -2000")
    need(st["output"]["dropped_units"] == 0,
         f"{st['output']['dropped_units']} units dropped across the step")
    fed = Analysis(cut)
    got = sum(len(v) for v in Analysis(w("live_skip.ts")).pes.values())
    want = sum(len(v) for v in fed.pes.values())
    # the packets cut in two at the splice are rightly discarded
    need(got >= want - 8, f"only {got} of {want} PES delivered")
    late = Analysis(w("live_skip.ts")).lateness_ms()
    need(len(late) <= 3, f"{len(late)} PES left after their time")
    # What arrived between the skip and the next PCR (30 ms) cannot be
    # told from the old clock's content: a known limit, kept small here.
    odd = wrong_clock([src], w("live_skip.ts"))
    need(odd <= 4, f"{odd} PES carry the wrong clock's timestamps")
    no_pause(cap)
    return f"{got}/{want} PES, 0 dropped, {len(late)} late, {odd} mis-stamped, time line " \
           f"moved {st['clock']['ts_offset_ms'] / 1000:+.2f} s once"


@test
def live_corrupt_pcr():
    """Live: single PCRs with a wild value do not disturb the service."""
    src = synth("live_src", duration=10.0)
    bad = _corrupt_pcrs(src, w("live_badpcr.ts"),
                        {80: 4.0, 120: -4.0, 160: 0.6, 200: -0.3, 240: 0.15})
    st, cap, _ = live(lambda p: p.play(bad, rate=4_000_000), "-r", "4M",
                      capture_name="live_badpcr_out.ts")
    must_pass(w("live_badpcr_out.ts"), cbr=True, allow_late=True)
    must_match(src, w("live_badpcr_out.ts"), subset=True)
    late = Analysis(w("live_badpcr_out.ts")).lateness_ms()
    need(len(late) <= 6 and max(late, default=0) < 200,
         f"{len(late)} PES late, worst {max(late, default=0):.0f} ms")
    orig = sum(len(v) for v in Analysis(src).pes.values())
    got = sum(len(v) for v in Analysis(w("live_badpcr_out.ts")).pes.values())
    need(got >= orig - 3, f"only {got} of {orig} PES delivered")
    need(st["clock"]["discontinuities"] == 0, "a damaged PCR moved the time line")
    need(st["input"]["pcr_glitches"] == 5 and st["input"]["pcr_jumps"] == 0,
         f"{st['input']['pcr_glitches']} glitches, {st['input']['pcr_jumps']} jumps")
    need(st["output"]["dropped_units"] == 0, f"{st['output']['dropped_units']} units dropped")
    return f"{got}/{orig} PES, time line untouched, 5 damaged PCRs set aside " \
           f"({len(late)} PES late, worst {max(late, default=0):.0f} ms)"


@test
def live_step_with_fast_start():
    """Source skips 3 s ahead and delivers the next second at double speed.

    The clock error keeps moving for half a second before it settles, so
    the step can only be confirmed late. The content that arrived meanwhile
    must then be moved onto the new clock and sent, not dropped. It cannot
    all be on time: the source itself delivered it half a second behind
    where its clock ends up."""
    src = synth("live_src", duration=10.0)
    data = read(src)
    n = len(data) // TS
    cut_a, cut_b = n * 3 // 10 // 7 * 7, n * 6 // 10 // 7 * 7
    cut = write(w("faststart.ts"), data[:cut_a * TS] + data[cut_b * TS:])
    st, cap, _ = live(lambda p: p.play(cut, rate=4_000_000, tempo=(3.0, 4.0, 2.0)),
                      "-r", "4M", capture_name="live_faststart.ts")
    must_pass(w("live_faststart.ts"), cbr=True, allow_late=True)
    must_match(src, w("live_faststart.ts"), subset=True, shifted=True)
    out = Analysis(w("live_faststart.ts"))
    got = sum(len(v) for v in out.pes.values())
    want = sum(len(v) for v in Analysis(cut).pes.values())
    need(got >= want - 8, f"only {got} of {want} PES delivered")
    need(st["output"]["dropped_units"] <= 2, f"{st['output']['dropped_units']} units dropped")
    need(st["clock"]["discontinuities"] == 1,
         f"{st['clock']['discontinuities']} discontinuities, expected 1")
    late = out.lateness_ms()
    need(max(late, default=0) < 900, f"a PES left {max(late, default=0):.0f} ms after its time")
    need(len(late) <= 180, f"{len(late)} PES left after their time")
    odd = wrong_clock([src], w("live_faststart.ts"))
    need(odd <= 4, f"{odd} PES carry the wrong clock's timestamps")
    no_pause(cap)
    return f"{got}/{want} PES, {st['output']['dropped_units']} dropped, {odd} mis-stamped, " \
           f"time line moved once; {len(late)} PES late while it was in doubt " \
           f"(worst {max(late, default=0):.0f} ms)"


@test
def live_step_with_sparse_pcr():
    """Source skips 3 s ahead on a stream that carries a PCR only every 240 ms.

    With so few PCRs the step takes about a second to confirm, longer than
    content may be held back. It is sent on, then moved onto the new clock
    while still queued. Late for a moment, but none of it lost."""
    src = synth("sparse_src", duration=10.0, seed=91, pcr_ms=240)
    data = read(src)
    n = len(data) // TS
    # Resume 100 ms after a PCR, so 140 ms of the new clock's content
    # arrives before any PCR says the clock has changed.
    cut_a, cut_b = n * 3 // 10 // 7 * 7, n * 61 // 100 // 7 * 7
    cut = write(w("sparse.ts"), data[:cut_a * TS] + data[cut_b * TS:])
    st, cap, _ = live(lambda p: p.play(cut, rate=4_000_000), "-r", "4M",
                      capture_name="live_sparse.ts")
    must_pass(w("live_sparse.ts"), cbr=True, allow_late=True)
    must_match(src, w("live_sparse.ts"), subset=True, shifted=True)
    out = Analysis(w("live_sparse.ts"))
    got = sum(len(v) for v in out.pes.values())
    # What follows the input's last PCR cannot be timed and is not expected.
    ref = Analysis(cut)
    last_pcr = ref.pcrs[-1][0]
    want = sum(1 for v in ref.pes.values() for r in v if r["last"] < last_pcr)
    need(got >= want - 8, f"only {got} of {want} PES delivered")
    need(st["clock"]["discontinuities"] == 1,
         f"{st['clock']['discontinuities']} discontinuities, expected 1")
    need(st["output"]["dropped_units"] <= 2, f"{st['output']['dropped_units']} units dropped")
    late = out.lateness_ms()
    need(max(late, default=0) < 900, f"a PES left {max(late, default=0):.0f} ms after its time")
    need(len(late) <= 180, f"{len(late)} PES left after their time")
    # Known limit: what arrives after the step but before the first PCR of
    # the new clock is taken for the old clock's, and keeps timestamps
    # that are off by the step. No more than that one PCR gap of content.
    odd = 0
    for pid, lst in out.pes.items():
        pts = [r["pts"] for r in lst if r["pts"] is not None]
        brk = [i for i in range(1, len(pts)) if abs(pts[i] - pts[i - 1]) > 45000]
        need(len(brk) in (0, 2), f"PID {pid}: timestamps break {len(brk)} times")
        if brk:
            odd += brk[1] - brk[0]
    need(odd <= 30, f"{odd} PES carry the wrong clock's timestamps")
    return f"{got}/{want} PES, {st['output']['dropped_units']} dropped, time line moved " \
           f"{st['clock']['ts_offset_ms'] / 1000:+.2f} s once; {len(late)} PES late while " \
           f"it was in doubt (worst {max(late, default=0):.0f} ms); {odd} PES from before " \
           f"the first new PCR mis-stamped"


@test
def live_restart_with_sparse_pcr():
    """Source comes back on a clock two hours away, PCR only every 240 ms."""
    a = synth("sparse_a", duration=5.0, pcr_base=10.0, seed=41, pcr_ms=240)
    b = synth("sparse_b", duration=5.0, pcr_base=7000.0, seed=42, pcr_ms=240)

    def feed(p):
        p.play(a, rate=4_000_000)
        time.sleep(1.5)
        p.play(b, rate=4_000_000)

    st, cap, _ = live(feed, "-r", "4M", capture_name="live_sparse_restart.ts")
    must_pass(w("live_sparse_restart.ts"), cbr=True, allow_late=True)
    both = write(w("live_sparse_ab.ts"), read(a) + read(b))
    must_match(both, w("live_sparse_restart.ts"), subset=True, shifted=True)
    need(st["clock"]["discontinuities"] == 1,
         f"{st['clock']['discontinuities']} discontinuities, expected 1")
    an_b, out = Analysis(b), Analysis(w("live_sparse_restart.ts"))
    have = {r["sha_nots"] for v in out.pes.values() for r in v}
    last_pcr = an_b.pcrs[-1][0]
    for pid, lst in an_b.pes.items():
        missing = [i for i, r in enumerate(lst)
                   if r["sha_nots"] not in have and r["last"] < last_pcr]
        need(not missing, f"PID {pid}: PES {missing[:8]} of the new source missing")
    need(st["output"]["dropped_units"] <= 30,       # the stranded end of the first source
         f"{st['output']['dropped_units']} units dropped")
    odd = wrong_clock([a, b], w("live_sparse_restart.ts"))
    need(odd == 0, f"{odd} PES carry the wrong clock's timestamps")
    late = out.lateness_ms()
    need(max(late, default=0) < 900, f"a PES left {max(late, default=0):.0f} ms after its time")
    no_pause(cap)
    return f"new source complete from its first packet, none mis-stamped, " \
           f"{len(late)} PES late while the clock was in doubt (worst {max(late, default=0):.0f} ms)"


def _step_then_quiet(name, first, second):
    """Plays packets first[0]..first[1], then second[0]..second[1] (fractions
    of a 10 s stream) with the PCR PID blanked from 0.15 s to 2.0 s into the
    second piece. Returns per audio PID (delivered, expected, longest hole ms)."""
    src = synth("live_src", duration=10.0)
    data = read(src)
    n = len(data) // TS
    null = bytes([0x47, 0x1F, 0xFF, 0x10]) + bytes([0xFF]) * 184
    cut = lambda f: int(n * f) // 7 * 7 * TS
    part2 = bytearray(data[cut(second[0]):cut(second[1])])
    per_s = n / 10.0
    for i in range(int(0.15 * per_s), int(2.0 * per_s)):
        p = i * TS
        if ((part2[p + 1] & 0x1F) << 8 | part2[p + 2]) == 1651:
            part2[p:p + TS] = null
    fed = write(w(name + "_in.ts"), data[cut(first[0]):cut(first[1])] + bytes(part2))
    st, cap, _ = live(lambda p: p.play(fed, rate=4_000_000), "-r", "4M",
                      capture_name=name + ".ts")
    must_pass(w(name + ".ts"), cbr=True, allow_late=True)
    out = Analysis(w(name + ".ts"))
    ref = Analysis(write(w(name + "_p2.ts"), bytes(part2)))
    have = {r["sha_nots"] for v in out.pes.values() for r in v}
    res = []
    for pid in (1652, 1653):
        lst = ref.pes[pid]
        got = sum(1 for r in lst if r["sha_nots"] in have)
        t = [out.stc_at(r["first"]) for r in out.pes[pid]]
        hole = max(b - a for a, b in zip(t, t[1:])) / 27000.0
        res.append((got, len(lst), hole))
    need(st["clock"]["held_units"] == 0 and st["output"]["queued_units"] == 0,
         "units left inside")
    no_pause(cap)
    return st, res


@test
def live_step_then_pcr_stops():
    """Source steps 3 s back, and 150 ms later its PCR goes quiet for 1.85 s
    while the audio keeps coming.

    Two faults at once. With the PCR gone before the step could be
    confirmed, the audio is timed from our own clock. It must keep flowing
    at its proper pace, and must not be moved when the step is confirmed
    after the PCR returns."""
    out = []
    for name, first, second in (("live_stepquiet_back", (0, 0.4), (0.1, 0.6)),
                                ("live_stepquiet_fwd", (0, 0.3), (0.6, 1.0))):
        st, res = _step_then_quiet(name, first, second)
        for got, want, hole in res:
            need(got >= want - 6, f"{name}: only {got} of {want} audio PES delivered")
            need(hole < 500, f"{name}: audio silent for {hole:.0f} ms")
        need(st["clock"]["discontinuities"] == 1,
             f"{name}: {st['clock']['discontinuities']} discontinuities, expected 1")
        out.append(f"{res[0][0]}/{res[0][1]} and {res[1][0]}/{res[1][1]} audio PES, "
                   f"longest hole {max(r[2] for r in res):.0f} ms")
    return f"3 s back: {out[0]}; 3 s ahead: {out[1]}"


@test
def live_input_ends_mid_question():
    """Input stops 0.2 s after its clock stepped, before that can be confirmed.

    Nobody can say which clock that last 0.2 s belongs to. It must not stay
    inside for ever, the output must stay well formed, and every unit must
    be accounted for: sent, or dropped and counted."""
    a = synth("live_a", duration=5.0, pcr_base=10.0, seed=31)
    b = synth("live_b", duration=5.0, pcr_base=7000.0, seed=32)
    data = read(b)
    n = len(data) // TS
    # a slice from the middle of the second stream, so content is flowing
    tail_b = write(w("midq_b.ts"), data[n * 2 // 5 // 7 * 7 * TS:(n * 2 // 5 // 7 * 7 + 133 * 7) * TS])

    def feed(p):
        p.play(a, stop_after=4.5)
        p.play(tail_b, rate=4_000_000, stop_after=4.5)

    st, cap, _ = live(feed, "-r", "4M", capture_name="live_midq.ts", tail=2.5)
    must_pass(w("live_midq.ts"), cbr=True, allow_late=True)
    need(st["clock"]["discontinuities"] == 0, "step confirmed: the test did not end mid-question")
    need(st["clock"]["held_units"] == 0,
         f"{st['clock']['held_units']} units still held with no input to release them")
    units_in = st["input"]["pes_units"] + st["input"]["sections"]
    out = st["output"]
    need(units_in == out["units"] + out["dropped_units"] + out["queued_units"],
         f"{units_in} units in, {out['units']} sent + {out['dropped_units']} dropped + "
         f"{out['queued_units']} queued")
    odd = wrong_clock([a], w("live_midq.ts"))
    need(odd == 0, f"{odd} PES sent with the wrong clock's timestamps")
    no_pause(cap)
    return f"nothing left held; {units_in} units in = {out['units']} sent + " \
           f"{out['dropped_units']} dropped + {out['queued_units']} queued; none mis-stamped"


@test
def live_packet_loss():
    """2% datagram loss on the wire."""
    src = synth("live_src", duration=10.0)
    rng = random.Random(7)
    st, cap, _ = live(lambda p: p.play(src, gate=lambda t: rng.random() > 0.02),
                      "-r", "4M", capture_name="live_loss.ts")
    rep = must_pass(w("live_loss.ts"), cbr=True, allow_late=True)
    must_match(src, w("live_loss.ts"), subset=True)
    need(st["input"]["cc_errors"] > 0, "loss not detected")
    need(st["clock"]["discontinuities"] == 0, "loss was treated as a new time base")
    return f"{st['input']['cc_errors']} input CC errors, output CC errors " \
           f"{rep['cc_errors']}, PCR accuracy {rep['pcr_accuracy_ns']} ns"


@test
def live_pcr_stops():
    """Live: the PCR (and video) stop while audio carries on."""
    src, cut_src = _pcr_stop_stream()
    st, cap, _ = live(lambda p: p.play(cut_src), "-r", "4M", capture_name="live_pcrstop.ts")
    must_pass(w("live_pcrstop.ts"), cbr=True, allow_late=True)
    must_match(src, w("live_pcrstop.ts"), subset=True)
    a, b = Analysis(src), Analysis(w("live_pcrstop.ts"))
    need(len(b.pes[1652]) >= len(a.pes[1652]) - 6,
         f"audio: {len(b.pes[1652])} of {len(a.pes[1652])} PES")
    need(st["output"]["dropped_units"] <= 1, f"{st['output']['dropped_units']} units dropped")
    need(st["clock"]["discontinuities"] == 0, "time line was re-based")
    need(st["input"]["pcr_missing"] > 50, "the missing PCR went unnoticed")
    return f"audio {len(b.pes[1652])}/{len(a.pes[1652])} PES; timed from the recovered " \
           f"clock for 11 s without a PCR"


@test
def live_pcr_pauses_and_returns():
    """Live: the PCR stream stops for 4 s and comes back; the others never notice."""
    src = synth("pcrpause_src", duration=12.0, seed=81)
    data = read(src)
    n = len(data) // TS
    out = bytearray()
    for i in range(n):
        pid = (data[i * TS + 1] & 0x1F) << 8 | data[i * TS + 2]
        hole = n * 3 // 12 < i < n * 7 // 12
        out += gen_ts.NULL if (hole and pid == 1651) else data[i * TS:(i + 1) * TS]
    cut = write(w("pcrpause.ts"), bytes(out))
    st, cap, _ = live(lambda p: p.play(cut), "-r", "4M", capture_name="live_pcrpause.ts")
    must_pass(w("live_pcrpause.ts"), cbr=True, allow_late=True)
    must_match(src, w("live_pcrpause.ts"), subset=True)
    a, b = Analysis(src), Analysis(w("live_pcrpause.ts"))
    for pid in (1652, 1653):
        need(len(b.pes[pid]) >= len(a.pes[pid]) - 4,
             f"PID {pid}: {len(b.pes[pid])} of {len(a.pes[pid])} PES")
    need(len(b.pes[1651]) > len(a.pes[1651]) * 0.55, "video did not resume")
    need(st["clock"]["discontinuities"] == 0, "time line was re-based")
    # only the video PES cut off at each edge of the hole may be discarded
    need(st["input"]["pes_dropped"] <= 2, f"{st['input']['pes_dropped']} PES discarded")
    return f"audio {len(b.pes[1652])}/{len(a.pes[1652])} and " \
           f"{len(b.pes[1653])}/{len(a.pes[1653])} PES straight through; video " \
           f"resumed; {st['input']['pes_dropped']} PES discarded in all"


@test
def live_low_rate_pcr_stops():
    """Live, 64 kb/s service (a datagram every 160 ms): PCR stops, audio continues."""
    prog = {"number": 300, "pmt_pid": 0x100, "pcr_pid": 0x1F0, "name": "Radio",
            "provider": "Coop", "streams": [
                {"pid": 0x101, "type": 0x03, "kind": "audio", "bitrate": 24_000,
                 "frame_ms": 48.0}]}
    src = synth("radio_src", duration=14.0, muxrate=64_000, programs=[prog], seed=82,
                pcr_ms=95, psi_ms=500, sdt_ms=2000)
    a = Analysis(src)
    need(len(a.pes[0x101]) > 250 and a.total_cc_errors() == 0,
         "test stream itself is broken")
    data = read(src)
    n = len(data) // TS
    out = bytearray()
    for i in range(n):
        pid = (data[i * TS + 1] & 0x1F) << 8 | data[i * TS + 2]
        out += gen_ts.NULL if (i > n * 4 // 14 and pid == 0x1F0) else data[i * TS:(i + 1) * TS]
    cut = write(w("radio.ts"), bytes(out))
    st, cap, _ = live(lambda p: p.play(cut), "-r", "160k", capture_name="live_radio.ts",
                      tail=1.5)
    must_pass(w("live_radio.ts"), cbr=True, allow_late=True)
    must_match(src, w("live_radio.ts"), subset=True)
    b = Analysis(w("live_radio.ts"))
    need(len(b.pes[0x101]) >= len(a.pes[0x101]) - 12,
         f"audio: {len(b.pes[0x101])} of {len(a.pes[0x101])} PES")
    need(st["input"]["pcr_missing"] > 20, "the missing PCR went unnoticed")
    return f"audio {len(b.pes[0x101])}/{len(a.pes[0x101])} PES, 10 s of it with no PCR"


@test
def live_delayed_slow_drain():
    """Input held up for 0.8 s, backlog then released at twice real time."""
    src = synth("live_src", duration=10.0)
    st, cap, _ = live(lambda p: p.play(src, stall=(3.0, 0.8, 2.0)), "-r", "4M",
                      capture_name="live_drain.ts")
    must_pass(w("live_drain.ts"), cbr=True, allow_late=True)
    must_match(src, w("live_drain.ts"), subset=True)
    need(st["clock"]["discontinuities"] == 0,
         f"{st['clock']['discontinuities']} time-base steps for data that was only late")
    need(st["clock"]["ts_offset_ms"] == 0, "time stamps were shifted")
    orig = sum(len(v) for v in Analysis(src).pes.values())
    got = sum(len(v) for v in Analysis(w("live_drain.ts")).pes.values())
    need(got >= orig - 3, f"only {got} of {orig} PES delivered")
    return f"no time-base step, {got}/{orig} PES delivered"


@test
def live_delayed_burst():
    """Input held up for 0.8 s then delivered in a burst: time line untouched."""
    src = synth("live_src", duration=10.0)
    st, cap, _ = live(lambda p: p.play(src, stall=(4.0, 0.8)), "-r", "4M",
                      capture_name="live_burst.ts")
    rep = must_pass(w("live_burst.ts"), cbr=True, allow_late=True)
    must_match(src, w("live_burst.ts"), subset=True)
    need(st["clock"]["discontinuities"] == 0,
         f"{st['clock']['discontinuities']} time-base steps for data that was only late")
    need(st["clock"]["ts_offset_ms"] == 0, "time stamps were shifted")
    no_pause(cap)
    orig = sum(len(v) for v in Analysis(src).pes.values())
    got = sum(len(v) for v in Analysis(w("live_burst.ts")).pes.values())
    need(got >= orig - 3, f"only {got} of {orig} PES delivered")
    need(st["input"]["pcr_missing"] == 0,
         "the wall clock was used to time data that was merely late")
    return f"no time-base step, {got}/{orig} PES delivered, PCR accuracy " \
           f"{rep['pcr_accuracy_ns']} ns"


@test
def live_process_stall():
    """The process itself is frozen for 2.5 s (as in a VM pause)."""
    src = synth("live_src", duration=10.0)
    marks = {}

    def feed(p, proc):
        def freeze():
            proc.send_signal(signal.SIGSTOP)
            marks["stop"] = time.time()
            time.sleep(2.5)
            marks["cont"] = time.time()   # the capture's clock
            proc.send_signal(signal.SIGCONT)
        p.play(src, at={4.0: freeze})

    st, cap, _ = live(feed, "-r", "4M", capture_name="live_stall.ts", with_proc=True)
    # The freeze is a real hole in the output; everything around it must be clean.
    must_pass(w("live_stall.ts"), allow_late=True,
              limits={"pcr_interval_ms": 3500, "pat_interval_ms": 3500,
                      "pmt_interval_ms": 3500, "sdt_interval_ms": 5000})
    must_match(src, w("live_stall.ts"), subset=True)
    need(st["output"]["clock_skips"] >= 1, "stall not noticed")
    burst = sum(len(c) for c, ts in zip(cap.chunks, cap.times)
                if marks["cont"] <= ts < marks["cont"] + 0.2)
    burst_s = burst * 8 / 4e6
    need(burst_s < 0.9, f"{burst_s:.2f} s of stream sent in the 0.2 s after resuming")
    out = Analysis(w("live_stall.ts"))
    need(out.pcr_disc >= 1, "PCR step not flagged in the stream")
    # Each stream may finish the one PES packet it was in the middle of
    # when time stopped; nothing else from before the freeze may follow.
    stale = [x for x in out.lateness_ms() if x > 1000]
    need(len(stale) <= 3, f"{len(stale)} PES sent more than 1 s after their time")
    need(st["clock"]["discontinuities"] == 0, "input time line was re-based")
    return f"skipped ahead instead of bursting ({burst_s:.2f} s of stream in the first " \
           f"0.2 s), step flagged on the PCR, input time line untouched"


@test
def live_low_delay():
    """Live with a 100 ms buffer."""
    src = synth("live_src", duration=10.0)
    st, cap, _ = live(lambda p: p.play(src), "-r", "4M", "-d", "100",
                      capture_name="live_lowdelay.ts")
    rep = must_pass(w("live_lowdelay.ts"), cbr=True)
    must_match(src, w("live_lowdelay.ts"), subset=True)
    orig = sum(len(v) for v in Analysis(src).pes.values())
    got = sum(len(v) for v in Analysis(w("live_lowdelay.ts")).pes.values())
    need(got >= orig - 3, f"only {got} of {orig} PES in the output")
    need(st["output"]["dropped_units"] == 0, "units dropped")
    return f"{got}/{orig} PES, nothing dropped, nothing late, PCR accuracy " \
           f"{rep['pcr_accuracy_ns']} ns"


@test
def live_multicast():
    """Multicast in and out; a second stream on the same port does not leak in."""
    src = synth("live_a", duration=5.0, pcr_base=10.0, seed=31)
    pin = _port[0]
    _port[0] += 2
    gin, gout, other = "239.77.1.1", "239.77.1.2", "239.77.9.9"
    import socket
    try:
        cap = Capture(pin + 1, host=gout, join_if="127.0.0.1")
        # Make this host a member of the other group as well. Without that
        # the kernel would not deliver it to anyone and the test would
        # pass even with a receive socket bound to every address.
        member = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        member.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                          socket.inet_aton(other) + socket.inet_aton("127.0.0.1"))
    except OSError as e:
        return f"SKIPPED: multicast not available here ({e})"
    proc = subprocess.Popen(
        [BIN, "-q", "-i", f"udp://{gin}:{pin}?iface=lo",
         "-o", f"udp://{gout}:{pin + 1}?iface=lo&ttl=1", "-r", "4M",
         "--stats-file", w("mcast.json")], stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(0.4)
        if proc.poll() is not None:
            cap.stop()
            return "SKIPPED: multicast not available here (" + \
                   proc.stderr.read().strip()[-80:] + ")"
        try:
            # every datagram also goes, as zeros, to another group on the same port
            p = Player(gin, pin, mcast_if="127.0.0.1", also=(other, pin))
            p.play(src)
            p.close()
        except OSError as e:
            cap.stop()
            return f"SKIPPED: multicast not available here ({e})"
        time.sleep(1.2)
        st = json.load(open(w("mcast.json")))
    finally:
        proc.send_signal(signal.SIGTERM)
        proc.communicate(timeout=5)
    cap.stop(w("mcast_out.ts"))
    member.close()
    need(st["input"]["packets"] > 0, "nothing received from the group")
    must_pass(w("mcast_out.ts"), cbr=True)
    must_match(src, w("mcast_out.ts"), subset=True)
    need(st["input"]["sync_errors"] == 0 and st["input"]["cc_errors"] == 0,
         "the other group's datagrams got into the input")
    return "joined, received and sent on the loopback interface; other group kept out"


def ctl(sock, *args):
    """caricoder3 ctl: returns (exit status, parsed reply)."""
    p = subprocess.run([BIN, "ctl", sock, *args], capture_output=True, text=True)
    try:
        reply = json.loads(p.stdout) if p.stdout.strip() else None
    except ValueError:
        raise Fail(f"ctl {' '.join(args)}: reply is not JSON: {p.stdout[:200]}")
    return p.returncode, reply


@test
def live_outputs_add_remove():
    """Outputs added and removed over the control socket while the channel runs."""
    src = synth("live_src", duration=10.0)
    pin = _port[0]
    _port[0] += 4
    sock = w("ctl.sock")
    cfg = w("outputs.json")
    for f in (sock, cfg):
        if os.path.exists(f):
            os.remove(f)
    cap_a, cap_b, cap_c = Capture(pin + 1), Capture(pin + 2), Capture(pin + 3)
    proc = subprocess.Popen(
        [BIN, "-q", "-i", f"udp://127.0.0.1:{pin}", "-o", f"udp://127.0.0.1:{pin + 1}",
         "-r", "4M", "--control", sock, "--config", cfg,
         "--stats-file", w("outputs.stats.json"), "--stats-interval", "0"],
        stderr=subprocess.PIPE, text=True)
    events = {}
    try:
        time.sleep(0.3)
        rc, rep = ctl(sock, "list-outputs")
        need(rc == 0 and rep["ok"] and len(rep["outputs"]) == 1 and
             rep["outputs"][0]["id"] == "out1", f"list-outputs at start: {rep}")
        rc, rep = ctl(sock, "remove-output", "nothing")
        need(rc == 1 and not rep["ok"], f"removing an unknown output: {rc} {rep}")
        rc, rep = ctl(sock, "add-output", "bogus://x")
        need(rc == 1 and not rep["ok"], f"adding an unknown scheme: {rc} {rep}")
        rc, rep = ctl(sock, "nonsense")
        need(rc == 1 and not rep["ok"], f"unknown command: {rc} {rep}")
        p = Player("127.0.0.1", pin)
        plan = [
            # at second, what
            (3.0, lambda: ctl(sock, "add-output", f"udp://127.0.0.1:{pin + 2}", "b", "--save")),
            (4.0, lambda: ctl(sock, "add-output", f"udp://127.0.0.1:{pin + 3}")),
            (6.0, lambda: ctl(sock, "remove-output", "out1")),
            (7.0, lambda: ctl(sock, "remove-output", "b")),
        ]
        t0 = time.time()

        def gate(elapsed):
            while plan and elapsed >= plan[0][0]:
                at, fn = plan.pop(0)
                events[at] = fn()
            return True
        p.play(src, gate=gate)
        p.close()
        time.sleep(1.2)
        st = json.load(open(w("outputs.stats.json")))
        rc, listing = ctl(sock, "list-outputs")
        rc2, stats = ctl(sock, "stats")
    finally:
        proc.send_signal(signal.SIGTERM)
        _, log = proc.communicate(timeout=5)
        cap_a.stop(w("out_a.ts"))
        cap_b.stop(w("out_b.ts"))
        cap_c.stop(w("out_c.ts"))
    need(proc.returncode == 0, f"exit status {proc.returncode}\n{log[-600:]}")
    for at, (rc, rep) in events.items():
        need(rc == 0 and rep["ok"], f"command at {at} s failed: {rep}")
    need(events[3.0][1]["id"] == "b" and events[4.0][1]["id"] == "out2",
         f"ids given: {events[3.0][1]} {events[4.0][1]}")
    need(len(listing["outputs"]) == 1 and listing["outputs"][0]["id"] == "out2",
         f"outputs left at the end: {listing}")
    need(rc2 == 0 and stats["ok"] and "outputs" in stats["stats"] and
         stats["stats"]["outputs"][0]["id"] == "out2", "stats over the socket")
    need(len(st["outputs"]) == 1 and st["outputs"][0]["packets"] > 0 and
         st["outputs"][0]["overruns"] == 0, f"outputs in the statistics file: {st['outputs']}")
    saved = json.load(open(cfg))
    need(saved["outputs"] == [], f"config after the saved output was removed: {saved}")
    a, b, c = (open(w(n), "rb").read() for n in ("out_a.ts", "out_b.ts", "out_c.ts"))
    # Each output must be a clean stream on its own, and the later ones
    # must carry exactly what the first one carried from the moment they
    # joined: the same packets, in the same order, nothing missing.
    # (a was cut off in mid-stream when removed: whole packets, but the
    # last PES is as unfinished as it was at that moment)
    need(len(a) % TS == 0, "the removed output did not end on a packet boundary")
    must_pass(w("out_c.ts"), cbr=True)
    must_match(src, w("out_c.ts"), subset=True)
    need(len(b) > 2 * 4_000_000 // 8 and len(c) > 5 * 4_000_000 // 8,
         f"later outputs too short: b {len(b)} c {len(c)} bytes")
    # b ran 3..7 s, c from 4 s on: c's first second is inside b, b's last
    # second inside c; a stopped at 6 s, so its last second is in c as well.
    sec = 4_000_000 // 8 // TS * TS
    need(c[:sec] in b, "output c's first packets are not in output b")
    need(b[len(b) - sec:] in c, "output b's last packets are not in output c")
    need(a[len(a) - sec:] in c, "the first output's last packets are not in the one that followed")
    return f"a {len(a) // TS} packets until removed, b {len(b) // TS} (saved then removed), " \
           f"c {len(c) // TS}; later outputs carry the same packets as the first"


@test
def live_outputs_config_reload():
    """The config file: outputs at start, SIGHUP adds and removes."""
    src = synth("live_src", duration=8.0)
    pin = _port[0]
    _port[0] += 4
    sock = w("ctl2.sock")
    cfg = w("outputs2.json")
    json.dump({"outputs": [{"id": "main", "url": f"udp://127.0.0.1:{pin + 1}"}]}, open(cfg, "w"))
    cap_a, cap_b = Capture(pin + 1), Capture(pin + 2)
    proc = subprocess.Popen(
        [BIN, "-q", "-i", f"udp://127.0.0.1:{pin}", "-r", "4M",
         "--control", sock, "--config", cfg, "--stats-interval", "0"],
        stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(0.3)
        rc, rep = ctl(sock, "list-outputs")
        need(rc == 0 and [o["id"] for o in rep["outputs"]] == ["main"] and
             rep["outputs"][0]["saved"], f"outputs from the config: {rep}")
        p = Player("127.0.0.1", pin)
        done = []

        def gate(elapsed):
            if elapsed >= 3.0 and "swap" not in done:
                done.append("swap")
                json.dump({"outputs": [{"id": "second", "url": f"udp://127.0.0.1:{pin + 2}"}]},
                          open(cfg, "w"))
                proc.send_signal(signal.SIGHUP)
            return True
        p.play(src, gate=gate)
        p.close()
        time.sleep(1.2)
        rc, rep = ctl(sock, "list-outputs")
    finally:
        proc.send_signal(signal.SIGTERM)
        _, log = proc.communicate(timeout=5)
        cap_a.stop(w("cfg_a.ts"))
        cap_b.stop(w("cfg_b.ts"))
    need(proc.returncode == 0, f"exit status {proc.returncode}\n{log[-600:]}")
    need([o["id"] for o in rep["outputs"]] == ["second"], f"outputs after SIGHUP: {rep}")
    a, b = open(w("cfg_a.ts"), "rb").read(), open(w("cfg_b.ts"), "rb").read()
    need(len(a) > 2 * 500_000 and len(b) > 3 * 500_000, f"captures: a {len(a)} b {len(b)}")
    must_pass(w("cfg_b.ts"), cbr=True)
    return f"main {len(a) // TS} packets, then second {len(b) // TS} after SIGHUP"


@test
def live_output_dead_port_isolated():
    """An output nobody listens to loses its datagrams; the other output is untouched."""
    src = synth("live_src", duration=6.0)
    pin = _port[0]
    _port[0] += 4
    cap = Capture(pin + 1)
    proc = subprocess.Popen(
        [BIN, "-q", "-i", f"udp://127.0.0.1:{pin}", "-o", f"udp://127.0.0.1:{pin + 1}",
         "-o", f"udp://127.0.0.1:{pin + 3}", "-r", "4M",
         "--stats-file", w("dead.json"), "--stats-interval", "0"],
        stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(0.3)
        p = Player("127.0.0.1", pin)
        p.play(src)
        p.close()
        time.sleep(1.2)
        st = json.load(open(w("dead.json")))
    finally:
        proc.send_signal(signal.SIGTERM)
        _, log = proc.communicate(timeout=5)
        cap.stop(w("dead_out.ts"))
    need(proc.returncode == 0, f"exit status {proc.returncode}\n{log[-600:]}")
    must_pass(w("dead_out.ts"), cbr=True)
    must_match(src, w("dead_out.ts"), subset=True)
    good, dead = st["outputs"]
    need(good["errors"] == 0 and good["overruns"] == 0, f"the good output: {good}")
    # (the two sinks read the ring in batches of 448; a snapshot may catch one a batch ahead)
    need(abs(dead["packets"] - good["packets"]) <= 448, f"both outputs were offered the same packets: {st['outputs']}")
    return f"good output {good['packets']} packets, 0 errors; dead one {dead['errors']} send errors"


def _transport_run(out_url, receiver, name, duration=8.0, extra=(), mid=None):
    """
    Runs the channel with one output of a transport type and a receiver
    process (started by `receiver(port)` -> Popen writing TS to stdout, or
    a (Popen, Capture) pair). `mid(proc, sock)` runs at 3 s. Returns
    (stats listing, log, packets received).
    """
    src = synth("live_src", duration=duration)
    pin = _port[0]
    _port[0] += 4
    sock = w(name + ".sock")
    out = w(name + ".ts")
    proc = subprocess.Popen(
        [BIN, "-q", "-i", f"udp://127.0.0.1:{pin}", "-o", out_url.format(port=pin + 1),
         "-r", "4M", "--control", sock, *extra], stderr=subprocess.PIPE, text=True)
    rx = cap = None
    try:
        time.sleep(0.3)
        r = receiver(pin + 1, out)
        rx, cap = r if isinstance(r, tuple) else (r, None)
        time.sleep(1.0)
        p = Player("127.0.0.1", pin)
        done = []

        def gate(t):
            if mid and t >= 3.0 and not done:
                done.append(1)
                mid(proc, sock)
            return True
        p.play(src, gate=gate)
        p.close()
        time.sleep(0.8)
        rc, listing = ctl(sock, "list-outputs")
        time.sleep(0.7)
    finally:
        proc.send_signal(signal.SIGTERM)
        _, log = proc.communicate(timeout=10)
        if rx:
            time.sleep(0.5)
            rx.send_signal(signal.SIGINT)
            try:
                rx.wait(timeout=5)
            except subprocess.TimeoutExpired:
                rx.kill()
                rx.wait()
        if cap:
            cap.stop(out)
    need(proc.returncode == 0, f"exit status {proc.returncode}\n{log[-600:]}")
    need(rc == 0 and listing["ok"], f"list-outputs: {listing}")
    got = os.path.getsize(out) // TS if os.path.exists(out) else 0
    return listing["outputs"][0], log, got, src


def _srt_receiver(mode):
    def start(port, out):
        url = f"srt://127.0.0.1:{port}?mode=caller" if mode == "caller" else f"srt://:{port}?mode=listener"
        return subprocess.Popen(["srt-live-transmit", "-q", url, "file://con"],
                                stdout=open(out, "wb"), stderr=subprocess.DEVNULL)
    return start


def _rist_receiver(listen):
    def start(port, out):
        cap = Capture(port + 100)
        url = f"rist://@127.0.0.1:{port}" if listen else f"rist://127.0.0.1:{port}"
        rx = subprocess.Popen(["ristreceiver", "-p", "1", "-i", url,
                               "-o", f"udp://127.0.0.1:{port + 100}", "-v", "-1"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return rx, cap
    return start


def _stream_ok(out, src, what):
    must_pass(out, cbr=True)
    must_match(src, out, subset=True)
    orig = sum(len(v) for v in Analysis(src).pes.values())
    got = sum(len(v) for v in Analysis(out).pes.values())
    need(got >= orig - 6, f"{what}: only {got} of {orig} PES arrived")
    return got, orig


@test
def live_srt_listener():
    """SRT output in listener mode; srt-live-transmit calls in and receives the stream."""
    if "srt://" not in subprocess.run([BIN, "--backends"], capture_output=True, text=True).stdout:
        return "SKIPPED: built without libsrt"
    if not shutil.which("srt-live-transmit"):
        return "SKIPPED: srt-tools not installed"
    o, log, got, src = _transport_run("srt://:{port}", _srt_receiver("caller"), "srt_listener")
    need(o["type"] == "srt" and o["state"].startswith("listening: 1 receiver"), f"state: {o}")
    need(o["errors"] == 0 and o["overruns"] == 0, f"errors: {o}")
    pes, orig = _stream_ok(w("srt_listener.ts"), src, "SRT listener")
    return f"1 receiver joined, {pes}/{orig} PES received, RTT {o.get('rtt_ms')} ms, " \
           f"{o['retransmitted']} retransmitted"


@test
def live_srt_caller_encrypted():
    """SRT output as caller with a passphrase; the listener must know it."""
    if "srt://" not in subprocess.run([BIN, "--backends"], capture_output=True, text=True).stdout:
        return "SKIPPED: built without libsrt"
    if not shutil.which("srt-live-transmit"):
        return "SKIPPED: srt-tools not installed"

    def start(port, out):
        return subprocess.Popen(["srt-live-transmit", "-q",
                                 f"srt://:{port}?mode=listener&passphrase=caricoder3test&pbkeylen=16",
                                 "file://con"], stdout=open(out, "wb"), stderr=subprocess.DEVNULL)
    o, log, got, src = _transport_run("srt://127.0.0.1:{port}?latency=200&passphrase=caricoder3test&pbkeylen=16",
                                      start, "srt_caller")
    need(o["state"] == "connected", f"state: {o}")
    need(o["errors"] == 0, f"errors: {o}")
    pes, orig = _stream_ok(w("srt_caller.ts"), src, "SRT caller")
    return f"connected with AES-128, {pes}/{orig} PES received"


@test
def live_srt_reconnect():
    """SRT caller: the listener goes away and comes back; the output reconnects by itself."""
    if "srt://" not in subprocess.run([BIN, "--backends"], capture_output=True, text=True).stdout:
        return "SKIPPED: built without libsrt"
    if not shutil.which("srt-live-transmit"):
        return "SKIPPED: srt-tools not installed"
    box = {}

    def start(port, out):
        box["port"], box["out"] = port, out
        box["first"] = subprocess.Popen(["srt-live-transmit", "-q", f"srt://:{port}?mode=listener", "file://con"],
                                        stdout=open(out + ".1", "wb"), stderr=subprocess.DEVNULL)
        return box["first"]

    def mid(proc, sock):
        box["first"].send_signal(signal.SIGINT)
        box["first"].wait(timeout=5)
        time.sleep(1.5)                     # the caller sees the break, waits, retries
        box["second"] = subprocess.Popen(["srt-live-transmit", "-q", f"srt://:{box['port']}?mode=listener", "file://con"],
                                         stdout=open(box["out"], "wb"), stderr=subprocess.DEVNULL)
    o, log, got, src = _transport_run("srt://127.0.0.1:{port}", start, "srt_reconnect", duration=10.0, mid=mid)
    try:
        box["second"].send_signal(signal.SIGINT)
        box["second"].wait(timeout=5)
    except Exception:
        box["second"].kill()
    need(o["state"] == "connected", f"state at the end: {o}")
    need(o["reconnects"] >= 1, f"no reconnect counted: {o}")
    need(o["errors"] > 0, f"nothing counted as lost while the listener was away: {o}")
    got2 = os.path.getsize(w("srt_reconnect.ts")) // TS
    need(got2 > 4 * 4_000_000 // 8 // TS, f"second listener got only {got2} packets")
    return f"listener restarted at 3 s: {o['reconnects']} reconnect, {o['errors']} datagrams lost " \
           f"meanwhile, {got2} packets to the new listener"


@test
def live_rist_caller():
    """RIST output (main profile) to a ristreceiver listening."""
    if "rist://" not in subprocess.run([BIN, "--backends"], capture_output=True, text=True).stdout:
        return "SKIPPED: built without librist"
    if not shutil.which("ristreceiver"):
        return "SKIPPED: rist-tools not installed"
    o, log, got, src = _transport_run("rist://127.0.0.1:{port}", _rist_receiver(True), "rist_caller")
    need(o["type"] == "rist" and o["state"] == "connected", f"state: {o}")
    need(o["errors"] == 0 and o["reconnects"] == 0, f"errors: {o}")
    pes, orig = _stream_ok(w("rist_caller.ts"), src, "RIST caller")
    return f"connected, {pes}/{orig} PES received, RTT {o.get('rtt_ms')} ms, {o['retransmitted']} retransmitted"


@test
def live_rist_listener_encrypted():
    """RIST output listening, with a shared secret; ristreceiver calls in."""
    if "rist://" not in subprocess.run([BIN, "--backends"], capture_output=True, text=True).stdout:
        return "SKIPPED: built without librist"
    if not shutil.which("ristreceiver"):
        return "SKIPPED: rist-tools not installed"

    def start(port, out):
        cap = Capture(port + 100)
        rx = subprocess.Popen(["ristreceiver", "-p", "1",
                               "-i", f"rist://127.0.0.1:{port}?secret=caricoder3test&aes-type=128",
                               "-o", f"udp://127.0.0.1:{port + 100}", "-v", "-1"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return rx, cap
    o, log, got, src = _transport_run("rist://@127.0.0.1:{port}?secret=caricoder3test&aes-type=128",
                                      start, "rist_listener")
    need(o["state"].startswith("listening: 1 receiver"), f"state: {o}")
    pes, orig = _stream_ok(w("rist_listener.ts"), src, "RIST listener")
    return f"1 receiver, AES-128, {pes}/{orig} PES received"


# ---- link inputs: an outside sender pushes into caricoder3 over SRT or RIST ----

def _input_run(in_url, sender, name, duration=8.0, mid=None):
    """
    Runs the channel with a link input (`in_url`) and a UDP output, with
    `sender(port)` -> Popen reading UDP on `port` and pushing it to the
    input. Returns (stats JSON, log, output file, source file).
    """
    src = synth("live_src", duration=duration)
    pin = _port[0]
    _port[0] += 4
    sock = w(name + ".sock")
    out = w(name + ".ts")
    cap = Capture(pin + 2)
    proc = subprocess.Popen(
        [BIN, "-q", "-i", in_url.format(port=pin + 1), "-o", f"udp://127.0.0.1:{pin + 2}",
         "-r", "4M", "--control", sock], stderr=subprocess.PIPE, text=True)
    tx = None
    try:
        time.sleep(0.3)
        tx = sender(pin, pin + 1)
        time.sleep(1.5)                     # the link comes up
        p = Player("127.0.0.1", pin)
        done = []

        def gate(t):
            if mid and t >= 3.0 and not done:
                done.append(1)
                mid(proc, sock)
            return True
        p.play(src, gate=gate)
        p.close()
        time.sleep(1.0)
        rc, stats = ctl(sock, "stats")
        time.sleep(0.5)
    finally:
        if tx:
            tx.send_signal(signal.SIGINT)
            try:
                tx.wait(timeout=5)
            except subprocess.TimeoutExpired:
                tx.kill()
                tx.wait()
        proc.send_signal(signal.SIGTERM)
        _, log = proc.communicate(timeout=10)
        cap.stop(out)
    need(proc.returncode == 0, f"exit status {proc.returncode}\n{log[-600:]}")
    need(rc == 0 and stats["ok"], f"stats: {stats}")
    return stats["stats"], log, out, src


def _srt_sender(mode):
    def start(udp_port, port):
        url = f"srt://127.0.0.1:{port}?mode=caller" if mode == "caller" else f"srt://:{port}?mode=listener"
        return subprocess.Popen(["srt-live-transmit", "-q", f"udp://127.0.0.1:{udp_port}", url],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return start


def _rist_sender(listen):
    def start(udp_port, port):
        url = f"rist://@127.0.0.1:{port}" if listen else f"rist://127.0.0.1:{port}"
        return subprocess.Popen(["ristsender", "-p", "1", "-i", f"udp://127.0.0.1:{udp_port}",
                                 "-o", url, "-v", "-1"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return start


@test
def live_input_srt_listener():
    """SRT input listening; srt-live-transmit calls in and pushes the stream."""
    if "srt://" not in subprocess.run([BIN, "--backends"], capture_output=True, text=True).stdout:
        return "SKIPPED: built without libsrt"
    if not shutil.which("srt-live-transmit"):
        return "SKIPPED: srt-tools not installed"
    st, log, out, src = _input_run("srt://:{port}", _srt_sender("caller"), "in_srt_listener")
    link = st["input"]["link"]
    need(link["transport"] == "srt" and link["connected"] and link["state"].startswith("sender "), f"link: {link}")
    need(link["errors"] == 0 and link["reconnects"] == 0, f"link: {link}")
    pes, orig = _stream_ok(out, src, "SRT input")
    return f"{link['state']}, {pes}/{orig} PES through, RTT {link['rtt_ms']} ms, {link['recovered']} recovered"


@test
def live_input_srt_caller_encrypted():
    """SRT input as caller with a passphrase, to a listening srt-live-transmit."""
    if "srt://" not in subprocess.run([BIN, "--backends"], capture_output=True, text=True).stdout:
        return "SKIPPED: built without libsrt"
    if not shutil.which("srt-live-transmit"):
        return "SKIPPED: srt-tools not installed"

    def start(udp_port, port):
        return subprocess.Popen(["srt-live-transmit", "-q", f"udp://127.0.0.1:{udp_port}",
                                 f"srt://:{port}?mode=listener&passphrase=caricoder3test&pbkeylen=16"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    st, log, out, src = _input_run("srt://127.0.0.1:{port}?latency=200&passphrase=caricoder3test&pbkeylen=16",
                                   start, "in_srt_caller")
    link = st["input"]["link"]
    need(link["connected"] and link["state"] == "connected", f"link: {link}")
    pes, orig = _stream_ok(out, src, "SRT input caller")
    return f"connected with AES-128, {pes}/{orig} PES through"


@test
def live_input_srt_sender_restart():
    """SRT input listening: the sender dies and a new one connects; the input takes it."""
    if "srt://" not in subprocess.run([BIN, "--backends"], capture_output=True, text=True).stdout:
        return "SKIPPED: built without libsrt"
    if not shutil.which("srt-live-transmit"):
        return "SKIPPED: srt-tools not installed"
    box = {}

    def start(udp_port, port):
        box["udp"], box["port"] = udp_port, port
        box["first"] = _srt_sender("caller")(udp_port, port)
        return box["first"]

    def mid(proc, sock):
        box["first"].kill()
        box["first"].wait()
        time.sleep(0.5)
        box["second"] = _srt_sender("caller")(box["udp"], box["port"])
    st, log, out, src = _input_run("srt://:{port}", start, "in_srt_restart", duration=12.0, mid=mid)
    try:
        box["second"].send_signal(signal.SIGINT)
        box["second"].wait(timeout=5)
    except Exception:
        box["second"].kill()
    link = st["input"]["link"]
    need(link["connected"], f"the second sender is not connected: {link}")
    need(link["reconnects"] >= 1, f"the first sender's loss was not seen: {link}")
    got = sum(len(v) for v in Analysis(out).pes.values())
    orig = sum(len(v) for v in Analysis(src).pes.values())
    need(got > orig // 2, f"only {got} of {orig} PES through after the restart")
    return f"sender replaced at 3 s: {link['reconnects']} reconnect, {got}/{orig} PES through"


def _http_sender(*opts):
    def start(udp_port, port):
        return subprocess.Popen([sys.executable, os.path.join(HERE, "http_relay.py"), str(udp_port), str(port), *opts],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return start


@test
def live_input_http_chunked():
    """HTTP input: a chunked body through a redirect; the server's 2 s backlog is skipped, the clock not disturbed."""
    st, log, out, src = _input_run("http://127.0.0.1:{port}/redirect", _http_sender("--hold", "2"), "in_http_chunked",
                                   duration=10.0)
    link = st["input"]["link"]
    need(link["transport"] == "http" and link["connected"], f"link: {link}")
    need(st["clock"]["discontinuities"] == 0 and st["output"]["dropped_units"] == 0,
         f"clock {st['clock']}, dropped {st['output']['dropped_units']}")
    must_pass(out, cbr=True)
    must_match(src, out, subset=True)
    got = sum(len(v) for v in Analysis(out).pes.values())
    orig = sum(len(v) for v in Analysis(src).pes.values())
    need(got > orig // 2, f"only {got} of {orig} PES")
    return f"redirect followed, backlog skipped, no discontinuity, {got}/{orig} PES (the 2 s backlog and the start left out)"


@test
def live_input_http_reconnect():
    """HTTP input: a plain body; the server drops the connection; the input connects again and goes on."""
    st, log, out, src = _input_run("http://127.0.0.1:{port}/live", _http_sender("--plain", "--drop-after", "2"),
                                   "in_http_reconnect", duration=10.0)
    link = st["input"]["link"]
    need(link["connected"] and link["reconnects"] >= 1, f"link: {link}")
    must_pass(out, cbr=True, allow_late=True)       # as across any outage
    must_match(src, out, subset=True)
    got = sum(len(v) for v in Analysis(out).pes.values())
    orig = sum(len(v) for v in Analysis(src).pes.values())
    need(got > orig * 0.6, f"only {got} of {orig} PES through the reconnect")
    return f"dropped at 2 s, {link['reconnects']} reconnect, {got}/{orig} PES through"


@test
def live_input_rist_listener():
    """RIST input listening; ristsender calls in."""
    if "rist://" not in subprocess.run([BIN, "--backends"], capture_output=True, text=True).stdout:
        return "SKIPPED: built without librist"
    if not shutil.which("ristsender"):
        return "SKIPPED: rist-tools not installed"
    st, log, out, src = _input_run("rist://@127.0.0.1:{port}", _rist_sender(False), "in_rist_listener")
    link = st["input"]["link"]
    need(link["transport"] == "rist" and link["connected"], f"link: {link}")
    pes, orig = _stream_ok(out, src, "RIST input")
    return f"{link['state']}, {pes}/{orig} PES through, RTT {link['rtt_ms']} ms"


@test
def live_input_rist_caller_encrypted():
    """RIST input calling a listening ristsender, with a shared secret."""
    if "rist://" not in subprocess.run([BIN, "--backends"], capture_output=True, text=True).stdout:
        return "SKIPPED: built without librist"
    if not shutil.which("ristsender"):
        return "SKIPPED: rist-tools not installed"

    def start(udp_port, port):
        return subprocess.Popen(["ristsender", "-p", "1", "-i", f"udp://127.0.0.1:{udp_port}",
                                 "-o", f"rist://@127.0.0.1:{port}?secret=caricoder3test&aes-type=128", "-v", "-1"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    st, log, out, src = _input_run("rist://127.0.0.1:{port}?secret=caricoder3test&aes-type=128",
                                   start, "in_rist_caller")
    link = st["input"]["link"]
    need(link["connected"] and link["state"] == "connected", f"link: {link}")
    pes, orig = _stream_ok(out, src, "RIST input caller")
    return f"connected, AES-128, {pes}/{orig} PES through"


def _real_h264_aac(seconds=8):
    """A real H.264 + AAC stream from ffmpeg (made once), or None."""
    src = w(f"rtmp_src_{seconds}.ts")
    if not os.path.exists(src):
        p = subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
             "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25",
             "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000",
             "-t", str(seconds), "-pix_fmt", "yuv420p", "-g", "25",
             "-c:v", "libx264", "-preset", "veryfast", "-b:v", "2000k", "-bf", "2",
             "-c:a", "aac", "-b:a", "128k", "-f", "mpegts", "-muxrate", "3M", src],
            capture_output=True, text=True)
        if p.returncode:
            return None
    return src


def _frames(path):
    """(video frames, audio frames) ffprobe counts in a file."""
    out = []
    for kind in ("v", "a"):
        p = subprocess.run(["ffprobe", "-v", "error", "-select_streams", kind + ":0",
                            "-count_packets", "-show_entries", "stream=nb_read_packets",
                            "-of", "csv=p=0", path], capture_output=True, text=True)
        out.append(int((p.stdout.split() or ["0"])[0]))
    return tuple(out)


def _rtmp_server(port, out):
    """ffmpeg listening as an RTMP server, writing what it gets to `out`."""
    return subprocess.Popen(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                             "-listen", "1", "-i", f"rtmp://127.0.0.1:{port}/live/test",
                             "-c", "copy", out], stderr=subprocess.DEVNULL)


@test
def live_rtmp_push():
    """RTMP push of H.264 + AAC to a server (ffmpeg listening); every frame arrives, in order."""
    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        return "SKIPPED: ffmpeg not installed"
    src = _real_h264_aac()
    if not src:
        return "SKIPPED: this ffmpeg cannot encode H.264/AAC"
    pin = _port[0]
    _port[0] += 4
    sock = w("rtmp.sock")
    srv = _rtmp_server(pin + 1, w("rtmp_out.flv"))
    time.sleep(0.5)
    proc = subprocess.Popen(
        [BIN, "-q", "-i", f"udp://127.0.0.1:{pin}", "-o", f"rtmp://127.0.0.1:{pin + 1}/live/test",
         "-r", "4M", "--control", sock], stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(1.0)
        p = Player("127.0.0.1", pin)
        p.play(src)
        p.close()
        time.sleep(1.2)
        rc, listing = ctl(sock, "list-outputs")
    finally:
        proc.send_signal(signal.SIGTERM)
        _, log = proc.communicate(timeout=10)
        try:
            srv.wait(timeout=8)
        except subprocess.TimeoutExpired:
            srv.kill()
            srv.wait()
    need(proc.returncode == 0, f"exit status {proc.returncode}\n{log[-600:]}")
    o = listing["outputs"][0]
    need(o["type"] == "rtmp" and o["state"] == "publishing", f"state: {o}")
    need(o["errors"] == 0 and o["reconnects"] == 0, f"errors: {o}")
    sv, sa = _frames(src)
    gv, ga = _frames(w("rtmp_out.flv"))
    # (the last PES of each stream stays in the input demultiplexer when
    # the source just stops, as in every live test: one picture, one
    # audio PES of 8 frames)
    need(gv >= sv - 2 and ga >= sa - 10, f"frames: video {gv}/{sv}, audio {ga}/{sa}")
    # decode clean, and timestamps in order
    p = subprocess.run(["ffmpeg", "-v", "error", "-xerror", "-i", w("rtmp_out.flv"), "-f", "null", "-"],
                       capture_output=True, text=True)
    need(p.returncode == 0 and not p.stderr.strip(), f"ffmpeg decoding the FLV: {p.stderr[:300]}")
    p = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                        "packet=dts_time,flags", "-of", "csv=p=0", w("rtmp_out.flv")],
                       capture_output=True, text=True)
    rows = [r.split(",") for r in p.stdout.split() if r]
    dts = [float(r[0]) for r in rows if r[0] != "N/A"]
    need(all(b >= a for a, b in zip(dts, dts[1:])), "video DTS not in order in the FLV")
    need(rows and "K" in rows[0][1], "the FLV does not start on a key frame")
    return f"video {gv}/{sv} frames, audio {ga}/{sa}, {o['packets']} tags, starts on a key frame, " \
           f"decodes clean"


@test
def live_rtmp_reconnect():
    """RTMP push: the server goes away and comes back; publishing resumes on a key frame."""
    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        return "SKIPPED: ffmpeg not installed"
    src = _real_h264_aac(12)
    if not src:
        return "SKIPPED: this ffmpeg cannot encode H.264/AAC"
    pin = _port[0]
    _port[0] += 4
    sock = w("rtmp2.sock")
    srv = _rtmp_server(pin + 1, w("rtmp_first.flv"))
    time.sleep(0.5)
    proc = subprocess.Popen(
        [BIN, "-q", "-i", f"udp://127.0.0.1:{pin}", "-o", f"rtmp://127.0.0.1:{pin + 1}/live/test",
         "-r", "4M", "--control", sock], stderr=subprocess.PIPE, text=True)
    box = {}
    try:
        time.sleep(1.0)
        p = Player("127.0.0.1", pin)

        def gate(t):
            if t >= 4.0 and "cut" not in box:
                box["cut"] = 1
                srv.kill()
                srv.wait()
                time.sleep(1.0)
                box["second"] = _rtmp_server(pin + 1, w("rtmp_second.flv"))
            return True
        p.play(src, gate=gate)
        p.close()
        time.sleep(1.5)
        rc, listing = ctl(sock, "list-outputs")
    finally:
        proc.send_signal(signal.SIGTERM)
        _, log = proc.communicate(timeout=10)
        if "second" in box:
            try:
                box["second"].wait(timeout=8)
            except subprocess.TimeoutExpired:
                box["second"].kill()
                box["second"].wait()
    need(proc.returncode == 0, f"exit status {proc.returncode}\n{log[-600:]}")
    o = listing["outputs"][0]
    need(o["state"] == "publishing" and o["reconnects"] >= 1, f"state at the end: {o}")
    gv, ga = _frames(w("rtmp_second.flv"))
    need(gv > 25 * 3, f"the second server got only {gv} video frames")
    p = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                        "packet=flags", "-of", "csv=p=0", w("rtmp_second.flv")],
                       capture_output=True, text=True)
    first = p.stdout.split()[0] if p.stdout.split() else ""
    need("K" in first, "the stream to the new server does not start on a key frame")
    p = subprocess.run(["ffmpeg", "-v", "error", "-xerror", "-i", w("rtmp_second.flv"), "-f", "null", "-"],
                       capture_output=True, text=True)
    need(p.returncode == 0 and not p.stderr.strip(), f"ffmpeg decoding the second FLV: {p.stderr[:300]}")
    return f"server restarted at 4 s: {o['reconnects']} reconnect, new server got {gv} video / {ga} audio " \
           f"frames from a key frame, decodes clean"


def _decodes_clean(path, what):
    """
    ffmpeg decodes the file with no complaint, except for the one AAC
    frame cut in half where the last segment ends (the rest of that PES
    is in the segment still being built; a player fetches it next).
    """
    p = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "null", "-"],
                       capture_output=True, text=True)
    bad = [l for l in p.stderr.splitlines()
           if l.strip() and "Input buffer exhausted" not in l and "Error submitting packet" not in l]
    need(p.returncode == 0 and not bad, f"{what} does not decode: {p.stderr[:300]}")


def _http_get(url):
    import urllib.request
    r = urllib.request.urlopen(url, timeout=5)
    return r.headers, r.read()


def _hls_check(base, out_path, want_variants=1):
    """
    Fetches the playlist(s) from an HLS output, every listed segment, and
    checks each segment opens with PAT/PMT and a key frame; writes the
    segments of variant 0 joined to out_path. Returns (n_segments,
    durations, master text).
    """
    h, master = _http_get(base + "/index.m3u8")
    master = master.decode()
    need(master.startswith("#EXTM3U"), f"not a playlist: {master[:80]}")
    if want_variants > 1:
        need(master.count("#EXT-X-STREAM-INF") == want_variants, f"master playlist:\n{master}")
        need("RESOLUTION=" in master, f"no RESOLUTION in the master playlist:\n{master}")
        lists = [l for l in master.split("\n") if l.endswith(".m3u8")]
        h, media = _http_get(base + "/" + lists[0])
        media = media.decode()
        prefix = lists[0].rsplit("/", 1)[0] + "/"
    else:
        need("#EXT-X-STREAM-INF" not in master, "one program but a master playlist")
        need(h.get("X-Next-Segment"), "no X-Next-Segment header")
        media, prefix = master, ""
    need("#EXT-X-TARGETDURATION" in media and "#EXT-X-MEDIA-SEQUENCE" in media, f"media playlist:\n{media}")
    segs = [l for l in media.split("\n") if l.endswith(".ts")]
    durs = [float(l.split(":")[1].rstrip(",")) for l in media.split("\n") if l.startswith("#EXTINF")]
    need(segs, f"no segments listed:\n{media}")
    with open(out_path, "wb") as f:
        for name in segs:
            h, d = _http_get(base + "/" + (name if name.startswith("v") else prefix + name))
            need(h.get("Content-Type") == "video/mp2t", f"{name}: content type {h.get('Content-Type')}")
            need(len(d) % TS == 0 and len(d) > 0, f"{name}: {len(d)} bytes")
            pids = [((d[i + 1] & 0x1F) << 8) | d[i + 2] for i in range(0, 3 * TS, TS)]
            need(pids[0] == 0, f"{name}: does not open with a PAT (PIDs {[hex(x) for x in pids]})")
            need(0x1FFF not in [((d[i + 1] & 0x1F) << 8) | d[i + 2] for i in range(0, len(d), TS)],
                 f"{name}: null packets in a segment")
            f.write(d)
    return len(segs), durs, master


@test
def live_hls_single():
    """HLS output: segments cut at key frames, served over HTTP, playable with ffmpeg."""
    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        return "SKIPPED: ffmpeg not installed"
    src = _real_h264_aac(12)
    if not src:
        return "SKIPPED: this ffmpeg cannot encode H.264/AAC"
    pin = _port[0]
    _port[0] += 4
    hdir = w("hls_dir")
    shutil.rmtree(hdir, ignore_errors=True)
    os.makedirs(hdir)
    proc = subprocess.Popen(
        [BIN, "-q", "-i", f"udp://127.0.0.1:{pin}", "-o", f"hls://:{pin + 1}?segment=2&dir={hdir}",
         "-r", "4M", "--control", w("hls.sock")], stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(0.5)
        p = Player("127.0.0.1", pin)
        box = {}

        def gate(t):
            if t >= 9.0 and "ff" not in box:   # a real HLS player, part way through
                box["ff"] = subprocess.run(
                    ["ffmpeg", "-v", "error", "-xerror", "-i", f"http://127.0.0.1:{pin + 1}/index.m3u8",
                     "-t", "3", "-f", "null", "-"], capture_output=True, text=True)
            return True
        p.play(src, gate=gate)
        p.close()
        time.sleep(0.5)
        n, durs, _ = _hls_check(f"http://127.0.0.1:{pin + 1}", w("hls_joined.ts"))
        rc, listing = ctl(w("hls.sock"), "list-outputs")
    finally:
        proc.send_signal(signal.SIGTERM)
        _, log = proc.communicate(timeout=10)
    need(proc.returncode == 0, f"exit status {proc.returncode}\n{log[-600:]}")
    o = listing["outputs"][0]
    need(o["type"] == "hls" and o["errors"] == 0, f"output: {o}")
    need(n >= 4, f"only {n} segments after 12 s with 2 s segments")
    need(all(1.5 <= d <= 2.5 for d in durs), f"segment durations: {durs}")
    _decodes_clean(w("hls_joined.ts"), "the joined segments")
    # every segment starts on a key frame
    for name in sorted(os.listdir(os.path.join(hdir, "v0"))):
        if not name.endswith(".ts"):
            continue
        p = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                            "packet=flags", "-of", "csv=p=0", os.path.join(hdir, "v0", name)],
                           capture_output=True, text=True)
        flags = p.stdout.split()
        need(flags and "K" in flags[0], f"{name} does not start on a key frame ({flags[:3]})")
    need(os.path.exists(os.path.join(hdir, "index.m3u8")), "no playlist written to the directory")
    need("ff" in box and box["ff"].returncode == 0 and not box["ff"].stderr.strip(),
         f"ffmpeg could not play the live playlist: {box.get('ff') and box['ff'].stderr[:300]}")
    return f"{n} segments of {min(durs):.2f}..{max(durs):.2f} s, each PAT/PMT + key frame first, " \
           f"joined they decode clean; ffmpeg played the live playlist; {o['state']}"


@test
def live_hls_renditions():
    """HLS from a channel with two renditions: a master playlist, one clean variant each."""
    skip = xc_ready("libavcodec", "x264", "fdk-aac", "mpg123")
    if skip:
        return skip
    src = v_source("xc_v_640", "h264", size="640x360", duration=8,
                   extra=("-output_ts_offset", "100"))
    pin = _port[0]
    _port[0] += 4
    proc = subprocess.Popen(
        [BIN, "-q", "-i", f"udp://127.0.0.1:{pin}", "-o", f"hls://:{pin + 1}?segment=2",
         "-r", "3000k", "--video", "h264:900k", "--video", "320x180:h264:300k", "--audio", "aac:96k",
         "--control", w("hls2.sock")], stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(0.5)
        p = Player("127.0.0.1", pin)
        p.play(src, rate=2_500_000)
        p.close()
        time.sleep(4.0)                     # the transcoders' delay
        n, durs, master = _hls_check(f"http://127.0.0.1:{pin + 1}", w("hls_v0.ts"), want_variants=2)
        h, v1 = _http_get(f"http://127.0.0.1:{pin + 1}/v1/index.m3u8")
        segs1 = [l for l in v1.decode().split("\n") if l.endswith(".ts")]
        with open(w("hls_v1.ts"), "wb") as f:
            for name in segs1:
                f.write(_http_get(f"http://127.0.0.1:{pin + 1}/v1/" + name)[1])
    finally:
        proc.send_signal(signal.SIGTERM)
        _, log = proc.communicate(timeout=10)
    need(proc.returncode == 0, f"exit status {proc.returncode}\n{log[-600:]}")
    need(n >= 2 and len(segs1) >= 2, f"segments: v0 {n}, v1 {len(segs1)}")
    for name, want in (("hls_v0.ts", "640x360"), ("hls_v1.ts", "320x180")):
        # (ffprobe lists the streams once per program and once more for
        # the file, so a single program shows each stream twice)
        p = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "stream=codec_type,width,height",
                            "-of", "csv=p=0", w(name)], capture_output=True, text=True)
        need(f"video,{want.replace('x', ',')}" in p.stdout.replace("\n", " "),
             f"{name}: streams {p.stdout.strip()} (want video {want})")
        need(p.stdout.count("video") <= 2, f"{name}: a variant must carry one video stream: {p.stdout.strip()}")
        p = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "program=program_id",
                            "-of", "csv=p=0", w(name)], capture_output=True, text=True)
        need(len(p.stdout.split()) == 1, f"{name}: a variant must be one program: {p.stdout.split()}")
        _decodes_clean(w(name), name)
    need("RESOLUTION=640x360" in master and "RESOLUTION=320x180" in master, f"master:\n{master}")
    return f"master playlist with 2 variants (640x360, 320x180), {n} + {len(segs1)} segments, " \
           f"each variant a single-program stream that decodes clean"


@test
def supervisor_api():
    """The supervisor: makes, runs, restarts, stops and deletes a channel over its API; proxies HLS."""
    import base64
    import urllib.request
    import urllib.error
    root = w("super")
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(os.path.join(root, "channels"))
    os.makedirs(os.path.join(root, "run"))
    port = _port[0] + 1
    pin = _port[0] + 2
    _port[0] += 4
    cfg = os.path.join(root, "caricoder3.json")
    json.dump({"listen": "127.0.0.1", "port": port, "channels_dir": os.path.join(root, "channels"),
               "run_dir": os.path.join(root, "run"), "password": "testpass1"}, open(cfg, "w"))
    src = _real_h264_aac(12)
    if not src:
        return "SKIPPED: this ffmpeg cannot encode H.264/AAC"
    auth = {"Authorization": "Basic " + base64.b64encode(b"admin:testpass1").decode()}

    def api(path, method="GET", body=None, headers=auth):
        r = urllib.request.Request(f"http://127.0.0.1:{port}{path}", method=method, headers=headers,
                                   data=json.dumps(body).encode() if body is not None else None)
        try:
            with urllib.request.urlopen(r, timeout=10) as resp:
                return resp.status, json.load(resp)
        except urllib.error.HTTPError as e:
            try:
                return e.code, json.load(e)
            except ValueError:
                return e.code, None
    sup = subprocess.Popen([BIN, "serve", "--config", cfg], stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(0.7)
        code, j = api("/api/system")
        need(code == 200 and j["ok"] and j["channels"] == 0, f"system: {code} {j}")
        code, j = api("/api/channels", headers={})
        need(code == 401, f"no password, yet {code}")
        r = urllib.request.urlopen(urllib.request.Request(f"http://127.0.0.1:{port}/", headers=auth), timeout=5)
        need(r.status == 200 and b"<title>caricoder3</title>" in r.read(), "the GUI page")
        code, j = api("/api/channels/bad%20name", "PUT", {"input": "x"})
        need(code == 400, f"a bad name was taken: {code} {j}")
        code, j = api("/api/channels/t1", "PUT", {"input": f"udp://127.0.0.1:{pin}", "muxrate": "4M", "enabled": True})
        need(code == 200 and j["ok"], f"create: {code} {j}")
        time.sleep(1.0)
        code, j = api("/api/channels/t1")
        need(code == 200 and j["channel"]["state"] == "running", f"after create: {j}")
        code, j = api("/api/channels/t1/outputs", "POST", {"url": "hls://?segment=2", "id": "hls"})
        need(code == 200 and j["ok"] and j["id"] == "hls", f"add output: {code} {j}")
        p = Player("127.0.0.1", pin)
        box = {}

        def gate(t):
            if t >= 1.5 and "pv_start" not in box:
                # the preview: a unicast input, so it reads a loopback copy of the output
                box["pv_start"] = api("/api/channels/t1/preview", "POST", {"drop": []})
            if t >= 10.0 and "pv_pl" not in box:
                t0 = time.time()
                box["pv_pl"] = urllib.request.urlopen(f"http://127.0.0.1:{port}/hls/t1.preview/index.m3u8", timeout=5).read().decode()
                box["pv_pl_s"] = time.time() - t0
                seg = [l for l in box["pv_pl"].split("\n") if l.endswith(".ts")]
                if seg:
                    box["pv_seg"] = urllib.request.urlopen(f"http://127.0.0.1:{port}/hls/t1.preview/" + seg[-1], timeout=5).read()
                box["pv_chan"] = api("/api/channels/t1")[1]["channel"]
            if t >= 7.0 and "pl" not in box:
                r = urllib.request.urlopen(f"http://127.0.0.1:{port}/hls/t1/index.m3u8", timeout=5)
                box["pl"] = r.read().decode()
                seg = [l for l in box["pl"].split("\n") if l.endswith(".ts")]
                if seg:
                    box["seg"] = urllib.request.urlopen(f"http://127.0.0.1:{port}/hls/t1/" + seg[-1], timeout=5).read()
                code, j = api("/api/channels/t1")
                box["running"] = j["channel"]
                # the probe, through the API, while the source plays (a 1 s
                # listen, in a thread so the player is not held up)
                import threading
                box["probe_t"] = threading.Thread(
                    target=lambda: box.__setitem__("probe", api("/api/probe", "POST",
                                                                {"url": f"udp://127.0.0.1:{pin}", "seconds": 1})))
                box["probe_t"].start()
            return True
        p.play(src, gate=gate)
        p.close()
        need("seg" in box and len(box["seg"]) > 100 * TS, f"HLS through the supervisor: {box.get('pl')}")
        if "probe_t" in box:
            box["probe_t"].join(timeout=20)
        code, pj = box.get("probe", (0, None))
        need(code == 200 and pj and pj["ok"] and pj["programs"] and
             any(x["kind"] == "audio" and x.get("sample_rate") == 48000 for x in pj["programs"][0]["streams"]),
             f"/api/probe: {code} {pj}")
        c = box["running"]
        need(c["stats"]["input"]["bitrate"] > 1e6 and [o["id"] for o in c["outputs"]["outputs"]] == ["hls", "pvtap"],
             f"running channel: in {c['stats']['input']['bitrate']}, outputs {c['outputs']}")
        ss = c["stats"]["service"]["streams"]
        need(all(x.get("bitrate", 0) > 0 and x.get("bitrate_in", 0) > 0 for x in ss) and
             {x["kind"] for x in ss} == {"video", "audio"}, f"per-stream rates and kinds: {ss}")
        need(len(c["history"]["in"]) >= 2 and max(c["history"]["in"]) > 1e6, f"history: {c['history']}")
        code, j = box.get("pv_start", (0, None))
        need(code == 200 and j["url"] == "/hls/t1.preview/index.m3u8", f"preview start: {code} {j}")
        pc = box.get("pv_chan") or {}
        need(pc.get("preview", {}).get("running"), f"preview state: {pc.get('preview')}")
        need(box.get("pv_seg") and len(box["pv_seg"]) > 20 * TS and box["pv_pl"].count("#EXTINF") >= 3,
             f"preview HLS: {box.get('pv_pl')}")
        need(box["pv_pl_s"] < 1.0, f"a proxied playlist took {box['pv_pl_s']:.1f} s")
        code, j = api("/api/channels/t1/preview", "DELETE")
        time.sleep(1.5)
        code, j = api("/api/channels/t1")
        need(not j["channel"]["preview"]["running"] and [o["id"] for o in j["channel"]["outputs"]["outputs"]] == ["hls"],
             f"after the preview stopped: {j['channel']['preview']}, outputs {j['channel']['outputs']}")
        # an output changed in place; a URL that will not open leaves the old one
        code, j = api("/api/channels/t1/outputs/hls", "PUT", {"url": "hls://?segment=3"})
        need(code == 200 and j["ok"], f"output changed: {code} {j}")
        code, j = api("/api/channels/t1/outputs/hls", "PUT", {"url": "bogus://x"})
        need(code == 400, f"a bad URL was taken: {code} {j}")
        code, j = api("/api/channels/t1")
        need([(o["id"], o["url"]) for o in j["channel"]["outputs"]["outputs"]] == [("hls", "hls://?segment=3")],
             f"after the changes: {j['channel']['outputs']}")
        # stop, start, config change -> restart, delete
        code, j = api("/api/channels/t1/stop", "POST")
        time.sleep(1.5)
        code, j = api("/api/channels/t1")
        need(j["channel"]["state"] == "stopped" and j["channel"]["saved_outputs"]["outputs"][0]["id"] == "hls" and
             j["channel"]["saved_outputs"]["outputs"][0]["url"] == "hls://?segment=3",
             f"after stop: {j['channel']['state']} {j['channel'].get('saved_outputs')}")
        code, j = api("/api/channels/t1/start", "POST")
        time.sleep(1.5)
        code, j = api("/api/channels/t1")
        need(j["channel"]["state"] == "running" and [o["id"] for o in j["channel"]["outputs"]["outputs"]] == ["hls"],
             f"after start: {j['channel']['state']}")
        pid = j["channel"]["pid"]
        code, j = api("/api/channels/t1", "PUT", {"input": f"udp://127.0.0.1:{pin}", "muxrate": "5M", "enabled": True})
        time.sleep(2.5)
        code, j = api("/api/channels/t1")
        need(j["channel"]["state"] == "running" and j["channel"]["pid"] != pid and j["channel"]["config"]["muxrate"] == "5M",
             f"after a config change: {j['channel']['state']} pid {j['channel']['pid']} (was {pid})")
        # a channel that dies is started again
        os.kill(j["channel"]["pid"], signal.SIGKILL)
        time.sleep(2.5)
        code, j = api("/api/channels/t1")
        need(j["channel"]["state"] == "running" and j["channel"]["restarts"] >= 1,
             f"after a crash: {j['channel']['state']}, restarts {j['channel']['restarts']}, why {j['channel']['why']}")
        code, j = api("/api/channels/t1/log")
        need(code == 200 and any("caricoder3" in l for l in j["log"]), "the channel's log lines")
        code, j = api("/api/channels/t1", "DELETE")
        time.sleep(1.0)
        code, j = api("/api/channels")
        need(code == 200 and j["channels"] == [], f"after delete: {j}")
        need(not os.path.exists(os.path.join(root, "channels", "t1.json")), "the channel file is still there")
    finally:
        sup.send_signal(signal.SIGTERM)
        try:
            _, log = sup.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            sup.kill()
            _, log = sup.communicate()
            raise Fail("the supervisor did not stop")
    need(sup.returncode == 0, f"supervisor exit {sup.returncode}\n{log[-800:]}")
    need(not [l for l in log.splitlines() if "channel t1" in l and "did not stop" in l], "a channel had to be killed")
    return "created, ran, HLS proxied, preview made and stopped, stopped, started, restarted on a config change " \
           "and after a crash, deleted; GUI served; password required"


@test
def history_keeps_to_its_cap():
    """The supervisor's record (SQLite): 3 days of 120 channels into a 16 MB cap stays under it, summed into minutes and hours."""
    import sqlite3
    if subprocess.run(["pkg-config", "--exists", "sqlite3"]).returncode != 0:
        return "SKIPPED: no libsqlite3-dev"
    exe = w("history_cap")
    r = subprocess.run(["gcc", "-O1", "-DHAVE_SQLITE", "-o", exe, os.path.join(HERE, "history_cap.c"),
                        os.path.join(HERE, "..", "src", "history.c"), os.path.join(HERE, "..", "src", "log.c"),
                        "-lsqlite3", "-lpthread"], capture_output=True, text=True)
    need(r.returncode == 0, f"build: {r.stderr[-500:]}")
    db = w("history_cap.db")
    for suffix in ("", "-wal", "-shm"):
        if os.path.exists(db + suffix):
            os.unlink(db + suffix)
    r = subprocess.run([exe, db, "16", "3"], capture_output=True, text=True, timeout=600)
    need(r.returncode == 0, f"run: {r.stderr[-500:]}")
    size = os.path.getsize(db) + (os.path.getsize(db + "-wal") if os.path.exists(db + "-wal") else 0)
    need(size <= 16 * 1048576, f"{size / 1048576:.1f} MB, over the 16 MB cap")
    con = sqlite3.connect(db)
    n10, t10 = con.execute("SELECT COUNT(*), MIN(t) FROM s10").fetchone()
    n60 = con.execute("SELECT COUNT(*) FROM s60").fetchone()[0]
    n3600, t3600 = con.execute("SELECT COUNT(*), MIN(t) FROM s3600").fetchone()
    up = con.execute("SELECT MAX(up) FROM s3600").fetchone()[0]
    ev = con.execute("SELECT COUNT(*) FROM events").fetchone()[0]
    con.close()
    need(n10 > 0 and n60 > 0 and n3600 > 0, f"rows: 10 s {n10}, minutes {n60}, hours {n3600}")
    need(up == 3600, f"an hour of a channel running counts {up} s")
    need(t3600 < time.time() - 2.5 * 86400, "the hours did not reach back")
    need(t10 > time.time() - 2.5 * 86400, "the oldest detail did not go first under the cap")
    need(ev > 50, f"only {ev} events kept: they should go last")
    return f"{size / 1048576:.1f} MB of 16; detail back {(time.time() - t10) / 3600:.0f} h, hours back " \
           f"{(time.time() - t3600) / 86400:.1f} days, {ev} events"


@test
def supervisor_incidents():
    """The watchdog: an input lost on two channels of one source is one source incident with both in it; it closes when the input is back; limits read and set through the API."""
    import base64
    import urllib.request
    import urllib.error
    root = w("super_inc")
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(os.path.join(root, "channels"))
    os.makedirs(os.path.join(root, "run"))
    port, pin = _port[0] + 1, _port[0] + 2
    _port[0] += 4
    cfg = os.path.join(root, "caricoder3.json")
    json.dump({"listen": "127.0.0.1", "port": port, "channels_dir": os.path.join(root, "channels"),
               "run_dir": os.path.join(root, "run"), "password": "testpass1",
               "history": os.path.join(root, "history.db"),
               "watchdog": {"min_s": 5, "clear_s": 4, "off": ["cpu"]}}, open(cfg, "w"))
    src = synth("syn_mpts", mpts=True)
    auth = {"Authorization": "Basic " + base64.b64encode(b"admin:testpass1").decode()}

    def api(path, method="GET", body=None):
        r = urllib.request.Request(f"http://127.0.0.1:{port}{path}", method=method, headers=auth,
                                   data=json.dumps(body).encode() if body is not None else None)
        try:
            with urllib.request.urlopen(r, timeout=10) as resp:
                return resp.status, json.load(resp)
        except urllib.error.HTTPError as e:
            return e.code, None
    sup = subprocess.Popen([BIN, "serve", "--config", cfg], stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(0.7)
        code, j = api("/api/watchdog")
        need(code == 200 and j["limits"]["min_s"] == 5 and j["limits"]["off"] == ["cpu"], f"watchdog: {code} {j}")
        for name, prog in (("i1", 1650), ("i2", 2000)):
            code, j = api(f"/api/channels/{name}", "PUT", {"input": f"udp://127.0.0.1:{pin}", "program": prog, "enabled": True})
            need(code == 200, f"create {name}: {code} {j}")
        time.sleep(1.5)
        p = Player("127.0.0.1", pin)
        p.play(src, stop_after=4.0)         # 4 s of input, then nothing
        time.sleep(14)
        code, j = api("/api/incidents?open=1")
        need(code == 200, f"incidents: {code}")
        inc = j["incidents"]
        group = [x for x in inc if x["channel"] is None and x["rule"] == "no_input"]
        mine = [x for x in inc if x["channel"] in ("i1", "i2") and x["rule"] == "no_input"]
        need(len(group) == 1 and "2 channels" in group[0]["summary"], f"source incident: {inc}")
        need(len(mine) == 2 and all(x["parent"] == group[0]["id"] for x in mine), f"channel incidents: {mine}")
        need(mine[0]["detail"].get("then", {}).get("input"), f"no evidence kept: {mine[0]['detail']}")
        code, one = api(f"/api/incidents/{group[0]['id']}")
        need(code == 200 and len(one["members"]) == 2 and any("no input" in e["text"] for e in one["events"]),
             f"one incident: {one}")
        # the input comes back: all of it closes
        dbg = {}
        p.play(src, stop_after=8.0, at={6.0: lambda: dbg.update(w=api("/api/watchdog")[1]["watching"], c=api("/api/channels/i1")[1]["channel"]["stats"]["input"])})
        time.sleep(1)
        code, j = api("/api/incidents")
        still = [x for x in j["incidents"] if x["closed"] is None and x["rule"] == "no_input"]
        others = sorted({x["rule"] for x in j["incidents"] if x["channel"] or x["group"]} - {"no_input"})   # not the server's (this disk may be low)
        need(not others, f"other incidents for an input that stopped: {others}")
        need(not still, f"still open: {[(x['id'], x['channel']) for x in still]}; watching {dbg.get('w')}; i1 input bitrate {dbg.get('c', {}).get('bitrate')} age {dbg.get('c', {}).get('last_packet_age_ms')}")
        src_inc = [x for x in j["incidents"] if x["rule"] == "no_input" and x["channel"] is None][0]
        lasted = src_inc["closed"] - src_inc["opened"]
        p.close()
        # limits through the API, kept in the config file
        code, j = api("/api/watchdog", "PUT", {"cc_per_min": 25, "off": []})
        need(code == 200 and j["limits"]["cc_per_min"] == 25 and j["limits"]["min_s"] == 5, f"put: {code} {j}")
        need(json.load(open(cfg))["watchdog"]["cc_per_min"] == 25, "limits not saved")
        # many verdicts at once: JSON lines; the bad ones said, the good ones kept
        lines = "\n".join([json.dumps({"id": src_inc["id"], "verdict": "wrong", "category": "network", "cause": "the test stopped the input", "note": "review"}),
                           json.dumps({"id": src_inc["id"], "verdict": "maybe"}),
                           json.dumps({"id": 999999, "verdict": "right"}), ""])
        r = urllib.request.Request(f"http://127.0.0.1:{port}/api/verdicts/import", method="POST", headers=auth, data=lines.encode())
        with urllib.request.urlopen(r, timeout=10) as resp:
            imp = json.load(resp)
        need(imp["applied"] == 1 and imp["failed"] == 2 and "line 3: no such incident" in imp["errors"], f"import: {imp}")
        for _ in range(20):
            code, one = api(f"/api/incidents/{src_inc['id']}")
            if one and one.get("incident", one).get("verdict"):
                break
            time.sleep(0.25)
        got = one.get("incident", one)
        need(got.get("verdict") == "wrong" and (got.get("actual") or {}).get("category") == "network", f"verdict not kept: {got.get('verdict')} {got.get('actual')}")
        r = urllib.request.Request(f"http://127.0.0.1:{port}/api/incidents?limit=2000&download=1", headers=auth)
        with urllib.request.urlopen(r, timeout=10) as resp:
            need("attachment" in (resp.headers.get("Content-Disposition") or ""), "the download is not offered as a file")
            need(any(x["id"] == src_inc["id"] for x in json.load(resp)["incidents"]), "download without the incidents")
    finally:
        sup.send_signal(signal.SIGTERM)
        try:
            sup.wait(timeout=10)
        except subprocess.TimeoutExpired:
            sup.kill()
    return f"source incident with both channels in it, closed after the input came back (lasted {lasted} s); limits set and saved; verdicts imported in bulk"


@test
def supervisor_disks():
    """The disks: each filesystem once, with what of ours is on it; a disk under the critical limit is an incident and the history makes room."""
    import base64
    import urllib.request
    root = w("super_disk")
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(os.path.join(root, "channels"))
    os.makedirs(os.path.join(root, "run"))
    port = _port[0] + 1
    _port[0] += 2
    cfg = os.path.join(root, "caricoder3.json")
    hist = os.path.join(root, "history.db")
    # every disk is "nearly full" by these limits
    json.dump({"listen": "127.0.0.1", "port": port, "channels_dir": os.path.join(root, "channels"),
               "run_dir": os.path.join(root, "run"), "password": "testpass1", "history": hist,
               "watchdog": {"min_s": 1, "disk_crit_pct": 100}}, open(cfg, "w"))
    auth = {"Authorization": "Basic " + base64.b64encode(b"admin:testpass1").decode()}

    def api(path):
        with urllib.request.urlopen(urllib.request.Request(f"http://127.0.0.1:{port}{path}", headers=auth), timeout=10) as r:
            return json.load(r)
    sup = subprocess.Popen([BIN, "serve", "--config", cfg], stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(16)
        sy = api("/api/system")
        disks = sy["disks"]
        mounts = [d["mount"] for d in disks]
        need("/" in mounts and len(mounts) == len(set(mounts)), f"disks: {mounts}")
        ours = [o for d in disks for o in d["ours"]]
        need(any(o["what"] == "history" and o["path"] == hist and o["bytes"] > 0 for o in ours), f"ours: {ours}")
        need(sy["history_squeezed"], "the history did not make room")
        inc = api("/api/incidents?open=1")["incidents"]
        need(any(x["rule"] == "disk_critical" for x in inc), f"incidents: {inc}")
        ev = api("/api/events?limit=50")["events"]
        need(any("nearly full" in e["text"] for e in ev), "no event about making room")
    finally:
        sup.send_signal(signal.SIGTERM)
        try:
            sup.wait(timeout=10)
        except subprocess.TimeoutExpired:
            sup.kill()
    return f"{len(disks)} filesystem(s): {', '.join(mounts)}; 'nearly full' opened an incident and the history made room"


@test
def supervisor_evidence():
    """Evidence: a black picture on a multicast channel gets its picture and 20 s of the source kept by itself; by hand, a unicast channel's output is recorded instead; a ping; the files can be fetched."""
    import base64
    import threading
    import urllib.request
    import urllib.error
    src = _content_faults_src()
    if not src:
        return "SKIPPED: this ffmpeg cannot make the test stream"
    root = w("super_ev")
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(os.path.join(root, "channels"))
    os.makedirs(os.path.join(root, "run"))
    port, pm, pu = _port[0] + 1, _port[0] + 2, _port[0] + 3
    _port[0] += 4
    cfg = os.path.join(root, "caricoder3.json")
    evdir = os.path.join(root, "evidence")
    json.dump({"listen": "127.0.0.1", "port": port, "channels_dir": os.path.join(root, "channels"),
               "run_dir": os.path.join(root, "run"), "password": "testpass1",
               "history": os.path.join(root, "history.db"), "evidence": evdir,
               "watchdog": {"min_s": 5, "content_s": 5, "clear_s": 4, "off": ["cpu", "disk_low", "disk_critical", "disk_filling"]}},
              open(cfg, "w"))
    auth = {"Authorization": "Basic " + base64.b64encode(b"admin:testpass1").decode()}

    def api(path, method="GET", body=None, raw=False):
        r = urllib.request.Request(f"http://127.0.0.1:{port}{path}", method=method, headers=auth,
                                   data=json.dumps(body).encode() if body is not None else None)
        try:
            with urllib.request.urlopen(r, timeout=10) as resp:
                return resp.status, (resp.read() if raw else json.load(resp))
        except urllib.error.HTTPError as e:
            return e.code, None
    group = f"239.255.10.3:{pm}"
    sup = subprocess.Popen([BIN, "serve", "--config", cfg], stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(0.7)
        for name, url in (("m1", f"udp://{group}"), ("u1", f"udp://127.0.0.1:{pu}")):
            code, j = api(f"/api/channels/{name}", "PUT", {"input": url, "enabled": True})
            need(code == 200, f"create {name}: {code} {j}")
        time.sleep(1.5)
        pm_ = Player("239.255.10.3", pm)
        pu_ = Player("127.0.0.1", pu)
        th = [threading.Thread(target=lambda: pm_.play(src, stop_after=26)),
              threading.Thread(target=lambda: pu_.play(synth("live_src", duration=10.0)))]
        for t in th:
            t.start()
        time.sleep(3)
        code, j = api("/api/tools/record", "POST", {"channel": "u1", "seconds": 4})
        need(code == 200, f"record u1: {code} {j}")
        code, j = api("/api/tools/ping", "POST", {"host": "127.0.0.1", "seconds": 3})
        need(code == 200, f"ping: {code} {j}")
        code, j = api("/api/tools/probe", "POST", {"channel": "u1"})
        need(code == 400, f"a unicast source was probed beside its channel: {code}")
        for t in th:
            t.join()
        # the recording began when the incident opened (about 10 s in) and lasts 20 s
        for _ in range(30):
            time.sleep(1)
            code, j = api("/api/evidence?channel=m1")
            if any(e["tool"] == "record" for e in (j or {}).get("evidence", [])):
                break
        code, j = api("/api/incidents?channel=m1")
        black = [x for x in j["incidents"] if x["rule"] == "black" and x["channel"] == "m1"]
        need(black, f"no black incident: {[x['rule'] for x in j['incidents']]}")
        code, one = api(f"/api/incidents/{black[0]['id']}")
        tools = {e["tool"]: e for e in one["evidence"]}
        need("picture" in tools and tools["picture"]["file_bytes"] > 1000, f"picture: {one['evidence']}")
        rec = tools.get("record")
        need(rec and rec["args"]["from"] == "source" and rec["file_bytes"] > 100000 and rec["result"].get("ok"),
             f"recording: {one['evidence']}")
        code, data = api(f"/api/evidence/{rec['id']}/file", raw=True)
        need(code == 200 and data[:1] == b"\x47" and len(data) == rec["file_bytes"], f"fetch: {code} {len(data or b'')}")
        code, j = api("/api/evidence?channel=u1")
        urec = [e for e in j["evidence"] if e["tool"] == "record"]
        need(urec and urec[0]["args"]["from"] == "output" and urec[0]["file_bytes"] > 10000, f"u1: {j}")
        code, j = api("/api/evidence")
        ping = [e for e in j["evidence"] if e["tool"] == "ping"]
        need(ping and (ping[0]["result"].get("loss_pct") == 0 or (not shutil.which("ping") and "not installed" in ping[0]["result"].get("error", ""))),
             f"ping: {ping}")
        a = Analysis(os.path.join(evdir, rec["file"]))
        need(a.pat and a.pmt, "the recording is not a playable stream")
    finally:
        sup.send_signal(signal.SIGTERM)
        try:
            sup.wait(timeout=10)
        except subprocess.TimeoutExpired:
            sup.kill()
    return f"black: picture {tools['picture']['file_bytes'] // 1024} kB + source {rec['file_bytes'] // 1024} kB; " \
           f"u1 output {urec[0]['file_bytes'] // 1024} kB; ping {ping[0]['result'].get('rtt_avg_ms', ping[0]['result'].get('error'))}"


@test
def supervisor_peers():
    """Two transcoders as peers: hello with the shared key; asked by hand, the other sees the source; when it is lost, the incident asks the other, which does not see it either; a wrong key is refused."""
    import base64
    import threading
    import urllib.request
    import urllib.error
    pa, pb, pm = _port[0] + 1, _port[0] + 2, _port[0] + 3
    _port[0] += 4
    sups, roots = [], {}
    auth = {"Authorization": "Basic " + base64.b64encode(b"admin:testpass1").decode()}

    def api(port, path, method="GET", body=None, headers=None):
        r = urllib.request.Request(f"http://127.0.0.1:{port}{path}", method=method, headers=headers or auth,
                                   data=json.dumps(body).encode() if body is not None else None)
        try:
            with urllib.request.urlopen(r, timeout=15) as resp:
                return resp.status, json.load(resp)
        except urllib.error.HTTPError as e:
            return e.code, None
    try:
        for name, port, other in (("tB", pb, pa), ("tA", pa, pb)):     # B first: A's first hello finds it
            root = w(f"super_peer_{name}")
            shutil.rmtree(root, ignore_errors=True)
            os.makedirs(os.path.join(root, "channels"))
            os.makedirs(os.path.join(root, "run"))
            cfg = os.path.join(root, "caricoder3.json")
            json.dump({"listen": "127.0.0.1", "port": port, "channels_dir": os.path.join(root, "channels"),
                       "run_dir": os.path.join(root, "run"), "password": "testpass1",
                       "history": os.path.join(root, "history.db"), "evidence": os.path.join(root, "evidence"),
                       "name": name, "peer_key": "shared-secret-1",
                       "peers": [{"name": "tB" if name == "tA" else "tA", "url": f"http://127.0.0.1:{other}"}],
                       "watchdog": {"min_s": 5, "clear_s": 4, "off": ["cpu", "disk_low", "disk_critical", "disk_filling"]}},
                      open(cfg, "w"))
            sups.append(subprocess.Popen([BIN, "serve", "--config", cfg], stderr=subprocess.PIPE, text=True))
            time.sleep(0.6)
        code, j = api(pa, "/api/channels/m1", "PUT", {"input": f"udp://239.255.10.5:{pm}", "enabled": True})
        need(code == 200, f"create: {code}")
        code, j = api(pb, "/api/peer/hello", headers={"Authorization": "CC3-Peer wrong"})
        need(code == 401, f"a wrong key was taken: {code}")
        code, j = api(pb, "/api/peer/hello", headers={"Authorization": "CC3-Peer shared-secret-1"})
        need(code == 200 and j["name"] == "tB", f"hello: {code} {j}")
        time.sleep(1.5)
        code, j = api(pa, "/api/peers")
        need(j["peers"][0]["reachable"] and j["peers"][0]["host"] == "tB", f"A's peers: {j['peers']}")
        pl = Player("239.255.10.5", pm)
        box = {}
        th = threading.Thread(target=lambda: pl.play(synth("live_src", duration=10.0), stop_after=9,
                                                     at={3.0: lambda: box.update(r=api(pa, "/api/tools/ask_peers", "POST", {"channel": "m1"}))}))
        th.start()
        th.join()
        need(box.get("r", (0,))[0] == 200, f"ask by hand: {box.get('r')}")
        time.sleep(16)
        code, j = api(pa, "/api/peers")
        texts = [m["text"] for m in j["messages"]]
        need(any("I see it here" in t for t in texts), f"conversation: {texts}")
        need(any("Nothing from it here either" in t for t in texts), f"conversation: {texts}")
        code, inc = api(pa, "/api/incidents?channel=m1")
        lost = [x for x in inc["incidents"] if x["rule"] == "no_input"]
        need(lost, f"no incident: {inc}")
        code, one = api(pa, f"/api/incidents/{lost[0]['id']}")
        pc = [e for e in one["evidence"] if e["tool"] == "peer_check"]
        need(pc and pc[0]["result"]["ok"] and not pc[0]["result"]["seen"], f"peer evidence: {one['evidence']}")
        code, jb = api(pb, "/api/peers")
        need(any("asks" in m["text"] for m in jb["messages"]), f"B's side: {[m['text'] for m in jb['messages']]}")
    finally:
        for p in sups:
            p.send_signal(signal.SIGTERM)
        for p in sups:
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
    return f"{len(texts)} lines of conversation; by hand: \"{next(t for t in texts if 'I see it' in t)}\"; lost: \"{next(t for t in texts if 'Nothing from it' in t)}\""


@test
def supervisor_technician():
    """The technician: an incident opens, the model server is started for it, and the answer (JSON to the schema) is kept on the incident and said in the conversation. Needs CC3_LLAMA_SERVER and CC3_TEST_MODEL (any small GGUF)."""
    import base64
    import urllib.request
    import urllib.error
    server, model = os.environ.get("CC3_LLAMA_SERVER"), os.environ.get("CC3_TEST_MODEL")
    if not (server and model and os.path.exists(server) and os.path.exists(model)):
        return "SKIPPED: set CC3_LLAMA_SERVER and CC3_TEST_MODEL to run it"
    root = w("super_tech")
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(os.path.join(root, "channels"))
    os.makedirs(os.path.join(root, "run"))
    port, pin = _port[0] + 1, _port[0] + 2
    _port[0] += 3
    cfg = os.path.join(root, "caricoder3.json")
    json.dump({"listen": "127.0.0.1", "port": port, "channels_dir": os.path.join(root, "channels"),
               "run_dir": os.path.join(root, "run"), "password": "testpass1",
               "history": os.path.join(root, "history.db"), "evidence": os.path.join(root, "evidence"),
               "technician": {"enabled": True, "server": server, "model": model, "gpu": "cpu", "threads": 2,
                              "context": 16384, "delay_s": 1, "port": port + 7, "knowledge": os.path.join(ROOT, "knowledge")},
               "watchdog": {"min_s": 4, "clear_s": 4, "off": ["cpu", "disk_low", "disk_critical", "disk_filling"]}},
              open(cfg, "w"))
    auth = {"Authorization": "Basic " + base64.b64encode(b"admin:testpass1").decode()}

    def api(path, method="GET", body=None):
        r = urllib.request.Request(f"http://127.0.0.1:{port}{path}", method=method, headers=auth,
                                   data=json.dumps(body).encode() if body is not None else None)
        try:
            with urllib.request.urlopen(r, timeout=10) as resp:
                return resp.status, json.load(resp)
        except urllib.error.HTTPError as e:
            return e.code, None
    sup = subprocess.Popen([BIN, "serve", "--config", cfg], stderr=subprocess.PIPE, text=True)
    diag, st = None, None
    try:
        time.sleep(0.7)
        code, j = api("/api/channels/t1", "PUT", {"input": f"udp://127.0.0.1:{pin}", "enabled": True})
        need(code == 200, f"create: {code}")
        p = Player("127.0.0.1", pin)
        p.play(synth("live_src", duration=10.0), stop_after=3)        # then nothing: no input
        for _ in range(120):
            time.sleep(1)
            code, j = api("/api/incidents?channel=t1")
            got = [x for x in (j or {}).get("incidents", []) if x.get("diagnosis")]
            if got:
                diag = got[0]
                break
        code, st = api("/api/technician")
        need(diag, f"no diagnosis; technician: {st}")
        d = diag["diagnosis"]
        need(set(d) >= {"cause", "category", "confidence", "explanation", "next_checks", "ticket"}, f"diagnosis: {d}")
        need(d["category"] in ("source_provider", "source_ird", "satellite_signal", "network", "transcoder",
                               "normal_content", "unknown"), f"category: {d['category']}")
        code, pj = api("/api/peers")
        need(any(m["peer"] == "technician" for m in pj["messages"]), "the conversation has no line from the technician")
        need(st["done"] >= 1 and st["where"] == "cpu", f"state: {st}")
        # a person says it was something else: kept, in the training data, and given with the next one like it
        said = "The IRD on 192.168.110.10 lost lock and rebooted"
        code, j = api(f"/api/incidents/{diag['id']}/verdict", "POST",
                      {"verdict": "wrong", "category": "source_ird", "cause": said, "note": "its front panel said so"})
        need(code == 200, f"verdict: {code}")
        time.sleep(3)
        code, one = api(f"/api/incidents/{diag['id']}")
        x = one["incident"]
        need(x["verdict"] == "wrong" and x["actual"]["category"] == "source_ird", f"kept: {x['verdict']} {x.get('actual')}")
        r = urllib.request.Request(f"http://127.0.0.1:{port}/api/training/export", headers=auth)
        with urllib.request.urlopen(r, timeout=20) as resp:
            lines = [json.loads(l) for l in resp.read().decode().splitlines() if l.strip()]
        need(len(lines) == 1 and lines[0]["messages"][-1]["role"] == "assistant", f"export: {len(lines)} lines")
        target = json.loads(lines[0]["messages"][-1]["content"])
        need(target["cause"] == said and target["category"] == "source_ird", f"what it should have said: {target}")
        need(len(lines[0]["messages"][0]["content"]) > 1000, "no notes in the training prompt")
        p.play(synth("live_src", duration=10.0), stop_after=3)        # the same again
        nxt = None
        for _ in range(30):
            time.sleep(1)
            code, j = api("/api/incidents?channel=t1")
            nxt = [x for x in j["incidents"] if x["id"] != diag["id"] and x["rule"] == "no_input"]
            if nxt:
                break
        need(nxt, "no second incident")
        r = urllib.request.Request(f"http://127.0.0.1:{port}/api/technician/prompt?incident={nxt[0]['id']}", headers=auth)
        with urllib.request.urlopen(r, timeout=20) as resp:
            prompt = json.load(resp)
        need(said in prompt["messages"][1]["content"], "the verdict is not given with the next incident like it")
        um = prompt["messages"][1]["content"]
        need("Key facts" in um and "- rule: " in um and "- began: " in um, f"no key facts ahead of the report: {um[:400]!r}")
        # a question in plain words
        code, j = api("/api/technician/ask", "POST", {"question": "What is wrong with t1 right now?"})
        need(code == 200 and j["id"], f"ask: {code} {j}")
        ans = None
        for _ in range(60):
            time.sleep(1)
            code, ans = api(f"/api/technician/ask?id={j['id']}")
            if ans and ans["state"] in ("answered", "failed"):
                break
        need(ans and ans["state"] == "answered" and ans["answer"], f"answer: {ans}")
        time.sleep(3)                       # the record is written every 2 s
        code, pj = api("/api/peers")
        chat = [m for m in pj["messages"] if m["incident"] is None and m["peer"] in ("person", "technician")]
        need(len(chat) >= 2, f"the question and answer are not in the conversation: {chat}")
    finally:
        sup.send_signal(signal.SIGTERM)
        try:
            sup.wait(timeout=20)
        except subprocess.TimeoutExpired:
            sup.kill()
    return f"answered in {st['last_seconds']:.0f} s on the {st['where']} (random test model: the form is tested); a 'wrong, it was the IRD' verdict kept, exported as training data and given with the next incident like it"


@test
def supervisor_shared_input():
    """Channels on one input share a reader: the input is read once, each channel gets its program; the reader follows them."""
    import base64
    import urllib.request
    import urllib.error
    root = w("super_share")
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(os.path.join(root, "channels"))
    os.makedirs(os.path.join(root, "run"))
    port, pin = _port[0] + 1, _port[0] + 2
    o1, o2 = _port[0] + 3, _port[0] + 4
    _port[0] += 6
    cfg = os.path.join(root, "caricoder3.json")
    json.dump({"listen": "127.0.0.1", "port": port, "channels_dir": os.path.join(root, "channels"),
               "run_dir": os.path.join(root, "run"), "password": "testpass1"}, open(cfg, "w"))
    src = synth("syn_mpts", mpts=True)
    auth = {"Authorization": "Basic " + base64.b64encode(b"admin:testpass1").decode()}

    def api(path, method="GET", body=None):
        r = urllib.request.Request(f"http://127.0.0.1:{port}{path}", method=method, headers=auth,
                                   data=json.dumps(body).encode() if body is not None else None)
        try:
            with urllib.request.urlopen(r, timeout=10) as resp:
                return resp.status, json.load(resp)
        except urllib.error.HTTPError as e:
            return e.code, None
    sup = subprocess.Popen([BIN, "serve", "--config", cfg], stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(0.7)
        # a unicast input: without the reader only one of the two could have it
        for name, prog in (("s1", 1650), ("s2", 2000)):
            code, j = api(f"/api/channels/{name}", "PUT", {"input": f"udp://127.0.0.1:{pin}", "program": prog, "enabled": True})
            need(code == 200, f"create {name}: {code} {j}")
        time.sleep(1.5)
        for name, op in (("s1", o1), ("s2", o2)):
            code, j = api(f"/api/channels/{name}/outputs", "POST", {"url": f"udp://127.0.0.1:{op}", "save": False})
            need(code == 200, f"output {name}: {code} {j}")
        rx = {}
        import threading

        def capture(op, key):
            import socket
            sk = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sk.bind(("127.0.0.1", op))
            sk.settimeout(0.5)
            data = bytearray()
            t_end = time.time() + 9
            while time.time() < t_end:
                try:
                    data += sk.recv(2048)
                except OSError:
                    pass
            sk.close()
            rx[key] = bytes(data)
        th = [threading.Thread(target=capture, args=(o1, "s1")), threading.Thread(target=capture, args=(o2, "s2"))]
        for t in th:
            t.start()
        box = {}

        def gate(t):
            if t >= 6.0 and "sys" not in box:
                box["sys"] = api("/api/system")[1]
                box["s1"] = api("/api/channels/s1")[1]["channel"]
            return True
        p = Player("127.0.0.1", pin)
        p.play(src, gate=gate)
        p.close()
        for t in th:
            t.join()
        shares = box["sys"]["shares"]
        need(len(shares) == 1 and shares[0]["channels"] == 2 and shares[0]["running"], f"shares: {shares}")
        rd = shares[0]["reader"]
        need(rd and all(o["pmt_seen"] and o["packets"] > 500 for o in rd["outputs"]), f"reader: {rd}")
        inp = box["s1"]["stats"]["input"]
        need(inp["shared"] and inp["url"] == f"udp://127.0.0.1:{pin}" and inp["cc_errors"] == 0, f"s1 input: {inp}")
        for name, prog in (("s1", 1650), ("s2", 2000)):
            path = w(f"share_{name}.ts")
            open(path, "wb").write(rx[name])
            a = Analysis(path)
            need(a.pat and prog in a.pat["programs"] and len(a.pat["programs"]) == 1 and a.pmt and a.pmt["program"] == prog,
                 f"{name}: PAT {a.pat}, PMT {a.pmt and a.pmt.get('program')}")
            need(sum(a.cc_errors.values()) == 0 and len(rx[name]) > 200 * TS, f"{name}: {len(rx[name])} bytes, CC {a.cc_errors}")
        # one channel goes: the reader stays for the other; both go: it stops
        api("/api/channels/s2", "DELETE")
        time.sleep(1.0)
        sh = api("/api/system")[1]["shares"]
        need(len(sh) == 1 and sh[0]["channels"] == 1 and sh[0]["running"], f"after one left: {sh}")
        api("/api/channels/s1", "DELETE")
        time.sleep(1.5)
        sh = api("/api/system")[1]["shares"]
        need(sh == [], f"after both left: {sh}")
    finally:
        sup.send_signal(signal.SIGTERM)
        try:
            _, log = sup.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            sup.kill()
            _, log = sup.communicate()
            raise Fail("the supervisor did not stop")
    need(sup.returncode == 0, f"supervisor exit {sup.returncode}\n{log[-800:]}")
    need("reader ended" not in log, "a reader died:\n" + log[-800:])
    return "2 channels on one unicast input through one reader, each its own program, CC clean; reader kept for one, stopped for none"


@test
def live_udp_multicast_loop():
    """A multicast UDP output reaches a receiver on this machine; with ?loop=0 it does not (Flussonic's multicast_loop=false)."""
    import socket
    import struct
    import threading
    src = synth("live_src", duration=3.0)
    got = {}
    for opt in ("", "?loop=0"):
        pin, grp_port = _port[0], _port[0] + 1
        _port[0] += 2
        rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        rx.bind(("", grp_port))
        try:
            rx.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                          struct.pack("4s4s", socket.inet_aton("239.77.7.7"), socket.inet_aton("0.0.0.0")))
        except OSError:
            rx.close()
            return "SKIPPED: this machine cannot join a multicast group"
        rx.settimeout(0.2)
        count = [0]
        stop = [False]

        def loop_rx():
            while not stop[0]:
                try:
                    rx.recv(2048)
                    count[0] += 1
                except OSError:
                    pass
        t = threading.Thread(target=loop_rx, daemon=True)
        t.start()
        proc = subprocess.Popen([BIN, "-q", "-i", f"udp://127.0.0.1:{pin}", "-o", f"udp://239.77.7.7:{grp_port}{opt}"],
                                stderr=subprocess.PIPE, text=True)
        time.sleep(0.3)
        Player("127.0.0.1", pin).play(src)
        time.sleep(1.0)
        proc.send_signal(signal.SIGTERM)
        proc.communicate(timeout=10)
        stop[0] = True
        t.join()
        rx.close()
        got[opt] = count[0]
    if got[""] == 0:
        return "SKIPPED: multicast is not delivered back on this machine at all (no route for it)"
    need(got["?loop=0"] == 0, f"loop=0 still delivered {got['?loop=0']} datagrams here")
    return f"{got['']} datagrams received here by default, none with loop=0"


@test
def live_systemd_watchdog():
    """Reports READY and keeps petting the systemd watchdog while healthy."""
    import socket
    # An abstract socket: no file, so no limit from how deep this folder is.
    name = f"caricoder3-test-{os.getpid()}"
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    s.bind("\0" + name)
    s.settimeout(0.2)
    env = dict(os.environ, NOTIFY_SOCKET="@" + name)
    pin = _port[0]
    _port[0] += 2
    proc = subprocess.Popen([BIN, "-q", "-i", f"udp://127.0.0.1:{pin}",
                             "-o", f"udp://127.0.0.1:{pin + 1}"],
                            env=env, stderr=subprocess.PIPE, text=True)
    msgs = []
    t_end = time.time() + 3.5
    while time.time() < t_end:
        try:
            msgs.append(s.recv(256).decode())
        except socket.timeout:
            pass
    proc.send_signal(signal.SIGTERM)
    proc.communicate(timeout=5)
    try:
        while True:
            msgs.append(s.recv(256).decode())
    except socket.timeout:
        pass
    s.close()
    need(proc.returncode == 0, f"exit status {proc.returncode}")
    need(msgs and msgs[0] == "READY=1", f"first message {msgs[:1]}")
    need(msgs.count("WATCHDOG=1") >= 2, f"watchdog messages: {msgs}")
    need("STOPPING=1" in msgs, "no STOPPING on shutdown")
    return f"READY, {msgs.count('WATCHDOG=1')} watchdog pings in 3.5 s with no " \
           f"input present, STOPPING on SIGTERM"


# ---------------------------------------------------------------- runner

# ------------------------------------------------------- transcoding tests
#
# These need ffmpeg (to make real streams and as the reference decoder),
# numpy, and a build with the codec libraries. Each is skipped, and says
# so, where one of those is missing.

BURSTS = "aevalsrc='if(lt(mod(t,1),0.05),random(0)*0.6-0.3,0)':s=48000"
V_SMALL = ["-c:v", "libx264", "-preset", "veryfast", "-b:v", "600k", "-g", "25", "-bf", "2"]


def backends(binary=None):
    """Names of the codec backends the build under test has, e.g. {'fdk-aac', 'lame'}."""
    p = subprocess.run([binary or BIN, "--backends"], capture_output=True, text=True)
    return {line.split()[0] for line in p.stdout.splitlines()
            if "available" in line and "unavailable" not in line}


def xc_ready(*needed):
    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        return "SKIPPED: ffmpeg not installed"
    try:
        import numpy                                    # noqa: F401
    except ImportError:
        return "SKIPPED: numpy not installed"
    missing = set(needed) - backends()
    if missing:
        return "SKIPPED: built without " + ", ".join(sorted(missing))
    return None


def av_source(name, audio, duration=8, video=V_SMALL, muxrate="2500k", mux_extra=()):
    """
    A real stream made by ffmpeg: a moving test picture and, per entry of
    `audio` (a list of ffmpeg argument lists), one audio stream carrying
    the same thing: a 50 ms burst of noise at the start of every second.
    """
    path = w(name + ".ts")
    if name in _cache and os.path.exists(path):
        return path
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
           "-f", "lavfi", "-i", f"testsrc2=size=320x240:rate=25:duration={duration}",
           "-f", "lavfi", "-i", f"{BURSTS}:d={duration}"]
    if len(audio) > 1:
        labels = "".join(f"[a{i}]" for i in range(len(audio)))
        cmd += ["-filter_complex", f"[1:a]asplit={len(audio)}{labels}", "-map", "0:v"]
        for i in range(len(audio)):
            cmd += ["-map", f"[a{i}]"]
    else:
        cmd += ["-map", "0:v", "-map", "1:a"]
    cmd += ["-pix_fmt", "yuv420p", *video]
    for i, a in enumerate(audio):
        cmd += [x.replace("#", str(i)) for x in a]
    cmd += ["-f", "mpegts", "-muxrate", muxrate, *mux_extra, path]
    p = subprocess.run(cmd, capture_output=True, text=True)
    need(p.returncode == 0, f"ffmpeg could not make {name}: {p.stderr[-400:]}")
    _cache[name] = True
    return path


def probe_audio(path):
    """[(codec, sample rate, channels)] of the audio streams, in order."""
    p = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a",
                        "-show_entries", "stream=codec_name,sample_rate,channels",
                        "-of", "json", path], capture_output=True, text=True)
    return [(s["codec_name"], int(s["sample_rate"]), int(s["channels"]))
            for s in json.loads(p.stdout or "{}").get("streams", [])]


def decodes_clean(path, what=""):
    p = subprocess.run(["ffmpeg", "-v", "error", "-xerror", "-i", path,
                        "-f", "null", "-"], capture_output=True, text=True)
    need(p.returncode == 0 and not p.stderr.strip(),
         f"{what}ffmpeg reports errors decoding the output: {p.stderr[:300]}")


def sync_ms(src, out, idx, src_idx=None, min_likeness=0.5):
    import av_sync
    ms, _, n_out = av_sync.offset_ms(src, idx if src_idx is None else src_idx, out, idx)
    # In place is not enough: it must also be the same audio (not at the wrong
    # speed, not garbled). Lossy coding of the test signal leaves this near 0.8-0.9;
    # HE-AAC, whose upper band is regenerated rather than transmitted, near 0.4.
    need(av_sync.last_likeness > min_likeness,
         f"audio stream {idx} of {os.path.basename(out)} is in place but is not the audio "
         f"of the source (likeness {av_sync.last_likeness:.2f})")
    return ms, n_out


def xstats(st, pid):
    for s in st["service"]["streams"]:
        if s["pid"] == pid:
            return s
    raise Fail(f"PID {pid} not in the statistics")


A_MP2 = ["-c:a:#", "mp2", "-b:a:#", "192k", "-ac:a:#", "2"]
A_AAC = ["-c:a:#", "aac", "-b:a:#", "128k", "-ac:a:#", "2"]
A_AC3 = ["-c:a:#", "ac3", "-b:a:#", "192k", "-ac:a:#", "2"]
OUT_CODEC = {"aac": ("aac", "fdk-aac"), "mp2": ("mp2", "twolame"), "mp3": ("mp3", "lame")}


@test
def xc_audio_every_pairing():
    """MP2, AAC and AC-3 in; AAC, MP2 and MP3 out: all nine, in sync to the sample."""
    skip = xc_ready("fdk-aac", "mpg123", "liba52", "twolame", "lame")
    if skip:
        return skip
    src = av_source("xc_three", [A_MP2, A_AAC, A_AC3])
    worst = 0.0
    for name, (codec, _) in OUT_CODEC.items():
        out = w(f"xc_three_{name}.ts")
        st = run_file(src, out, "--audio", name)
        must_pass(out)
        must_match(src, out, only={0x100})              # the video is not touched
        decodes_clean(out, f"to {name}: ")
        got = probe_audio(out)
        need(got == [(codec, 48000, 2)] * 3, f"to {name}: output audio is {got}")
        for i, pid in enumerate((0x101, 0x102, 0x103)):
            ms, n = sync_ms(src, out, i)
            worst = max(worst, abs(ms))
            need(abs(ms) < 1.0, f"{probe_audio(src)[i][0]} to {name}: audio is {ms:+.2f} ms off")
            need(n > 48000 * 7.5, f"to {name}: only {n / 48000:.2f} s of audio came out")
            x = xstats(st, pid)
            need(x["mode"] == "transcode" and x["decode_errors"] == 0 and
                 x["silence_ms"] == 0 and x["cut_ms"] == 0 and not x["failed"],
                 f"to {name}, PID {pid}: {x}")
        need(st["output"]["late_units"] == 0 and st["output"]["dropped_units"] == 0,
             f"to {name}: late or dropped units")
    return f"9 pairings conformant, decode clean, video untouched, worst sync error " \
           f"{worst:.3f} ms"


@test
def xc_audio_formats():
    """Mono, 44.1 and 32 kHz, 24 kHz, 5.1, LATM: each lands in sync at its own rate."""
    skip = xc_ready("fdk-aac", "mpg123", "liba52", "twolame", "lame")
    if skip:
        return skip
    cases = [
        ("mono_mp2", ["-c:a:#", "mp2", "-b:a:#", "96k", "-ac:a:#", "1"], (48000, 1), ()),
        ("mp2_44", ["-c:a:#", "mp2", "-b:a:#", "192k", "-ac:a:#", "2", "-ar:a:#", "44100"],
         (44100, 2), ()),
        ("mp2_32", ["-c:a:#", "mp2", "-b:a:#", "128k", "-ac:a:#", "2", "-ar:a:#", "32000"],
         (32000, 2), ()),
        ("mp2_24", ["-c:a:#", "mp2", "-b:a:#", "64k", "-ac:a:#", "2", "-ar:a:#", "24000"],
         (24000, 2), ()),
        ("mp3_in", ["-c:a:#", "libmp3lame", "-b:a:#", "128k", "-ac:a:#", "2"], (48000, 2), ()),
        ("aac_51", ["-c:a:#", "aac", "-b:a:#", "320k", "-ac:a:#", "6"], (48000, 2), ()),
        ("ac3_51", ["-c:a:#", "ac3", "-b:a:#", "448k", "-ac:a:#", "6"], (48000, 2), ()),
        ("ac3_44", ["-c:a:#", "ac3", "-b:a:#", "192k", "-ac:a:#", "2", "-ar:a:#", "44100"],
         (44100, 2), ()),
        ("aac_latm", A_AAC, (48000, 2), ("-mpegts_flags", "latm")),
        ("aac_mono_32", ["-c:a:#", "aac", "-b:a:#", "64k", "-ac:a:#", "1", "-ar:a:#", "32000"],
         (32000, 1), ()),
    ]
    done = []
    for name, a, (rate, ch), mux_extra in cases:
        src = av_source("xc_fmt_" + name, [a], duration=6, mux_extra=mux_extra)
        for to in ("aac", "mp2", "mp3"):
            out = w(f"xc_fmt_{name}_{to}.ts")
            st = run_file(src, out, "--audio", to)
            must_pass(out)
            decodes_clean(out, f"{name} to {to}: ")
            got = probe_audio(out)
            need(got == [(to, rate, ch)], f"{name} to {to}: output audio is {got}, "
                                          f"expected {rate} Hz, {ch} channel(s)")
            ms, n = sync_ms(src, out, 0)
            need(abs(ms) < 1.0, f"{name} to {to}: audio is {ms:+.2f} ms off")
            need(n > 48000 * 5.5, f"{name} to {to}: only {n / 48000:.2f} s of audio")
            x = xstats(st, 0x101)
            need(x["decode_errors"] == 0 and not x["failed"], f"{name} to {to}: {x}")
        done.append(name)
    return f"{len(done)} input formats, each to AAC, MP2 and MP3: rate and channels kept " \
           f"(5.1 mixed to stereo), in sync"


@test
def xc_audio_one_stream_only():
    """--audio PID=...: that stream is transcoded, the others pass byte for byte."""
    skip = xc_ready("fdk-aac", "liba52")
    if skip:
        return skip
    src = av_source("xc_three", [A_MP2, A_AAC, A_AC3])
    out = w("xc_one.ts")
    st = run_file(src, out, "--audio", "0x103=aac:96k")
    must_pass(out)
    must_match(src, out, only={0x100, 0x101, 0x102})
    need([c for c, _, _ in probe_audio(out)] == ["mp2", "aac", "aac"], str(probe_audio(out)))
    need(xstats(st, 0x101)["mode"] == "copy" and xstats(st, 0x103)["mode"] == "transcode",
         "wrong streams transcoded")
    a = Analysis(out)
    types = {s["pid"]: (s["type"], s["desc"]) for s in a.pmt["streams"]}
    need(types[0x103][0] == 0x0F, f"PMT says type 0x{types[0x103][0]:02X} for the AAC")
    need(b"AC-3" not in types[0x103][1] and 0x6A not in types[0x103][1][:1],
         "the AC-3 descriptor is still attached to what is now AAC")
    ms, _ = sync_ms(src, out, 2)
    need(abs(ms) < 1.0, f"audio is {ms:+.2f} ms off")
    return "AC-3 PID became AAC (PMT type and descriptors updated), the other three " \
           "streams identical"


@test
def xc_audio_packet_loss():
    """Lost input packets: the hole is filled with silence and sync is kept."""
    skip = xc_ready("fdk-aac", "mpg123", "liba52")
    if skip:
        return skip
    src = av_source("xc_three", [A_MP2, A_AAC, A_AC3])
    data = bytearray(read(src))
    n = len(data) // TS
    rng = random.Random(5)
    gone = 0
    # Runs of packets missing from the audio PIDs, between bursts and on them.
    for start in sorted(rng.sample(range(n // 10, n - n // 10), 40)):
        for i in range(start, start + rng.randint(1, 12)):
            pid = (data[i * TS + 1] & 0x1F) << 8 | data[i * TS + 2]
            if pid in (0x101, 0x102, 0x103):
                data[i * TS + 1] |= 0x1F                    # becomes a null packet
                data[i * TS + 2] = 0xFF
                gone += 1
    bad = write(w("xc_loss_in.ts"), bytes(data))
    out = w("xc_loss.ts")
    st = run_file(bad, out, "--audio", "aac")
    must_pass(out)
    decodes_clean(out)
    silence = 0
    for i, pid in enumerate((0x101, 0x102, 0x103)):
        x = xstats(st, pid)
        silence += x["silence_ms"]
        need(not x["failed"], f"PID {pid}: {x}")
        ms, n_out = sync_ms(src, out, i)
        need(abs(ms) < 1.0, f"PID {pid}: audio is {ms:+.2f} ms off after the losses")
        need(n_out > 48000 * 7.5, f"PID {pid}: only {n_out / 48000:.2f} s of audio")
    need(st["input"]["cc_errors"] > 0 and silence > 0,
         f"the losses were not noticed ({st['input']['cc_errors']} CC errors, "
         f"{silence} ms of silence)")
    return f"{gone} audio packets removed: {silence} ms filled with silence over three " \
           f"streams, output continuous and in sync"


@test
def xc_audio_garbage():
    """Damaged audio (bit errors that leave the transport layer intact)."""
    skip = xc_ready("fdk-aac", "mpg123", "liba52", "twolame")
    if skip:
        return skip
    src = av_source("xc_three", [A_MP2, A_AAC, A_AC3])
    data = read(src)
    n = len(data) // TS
    rng = random.Random(11)
    runs = 0
    for trial in range(12):
        d = bytearray(data)
        for _ in range(rng.choice((5, 40, 400))):
            i = rng.randrange(n)
            pid = (d[i * TS + 1] & 0x1F) << 8 | d[i * TS + 2]
            if pid not in (0x101, 0x102, 0x103) or (d[i * TS + 3] & 0x20):
                continue
            # anywhere in the payload: headers and audio data alike
            for _ in range(rng.choice((1, 1, 8, 60))):
                d[i * TS + rng.randrange(4, TS)] ^= 1 << rng.randrange(8)
        bad = write(w("xc_garbage_in.ts"), bytes(d))
        out = w("xc_garbage.ts")
        run_file(bad, out, "--audio", rng.choice(("aac", "mp2")))
        rep, fails = check(out, 0, False, None)
        fails = [f for f in fails if "after their DTS" not in f]
        need(not fails, f"trial {trial}: " + "; ".join(fails))
        runs += 1
    return f"{runs} damaged inputs: no crash, no sanitizer report, output still a valid stream"


@test
def xc_audio_undecodable_is_copied():
    """E-AC-3 cannot be decoded by the AC-3 library: the stream is copied, and said so."""
    skip = xc_ready("fdk-aac")
    if skip:
        return skip
    src = av_source("xc_eac3", [["-c:a:#", "eac3", "-b:a:#", "192k", "-ac:a:#", "2"]], duration=4)
    out = w("xc_eac3_out.ts")
    binary = ASAN if os.path.exists(ASAN) else BIN
    p = subprocess.run([binary, "-i", src, "-o", out, "--audio", "aac"],
                       capture_output=True, text=True, timeout=120)
    need(p.returncode == 0, p.stderr[-400:])
    need("cannot be decoded here" in p.stderr, "no warning that the stream is copied")
    must_pass(out)
    must_match(src, out)
    return "warned, copied untouched"


def he_aac_source(aot):
    """
    An HE-AAC (aot 5) or HE-AAC v2 (aot 29) stream, or None. ffmpeg cannot
    make one, so tests/he_aac_enc.c does, and that needs an fdk-aac library
    with the HE parts in it: the one installed, or the one under the
    directory named by CC3_FDK_FULL (a build of the original library).
    """
    path = w(f"xc_he{aot}.ts")
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return path
    helper = w("he_aac_enc")
    full = os.environ.get("CC3_FDK_FULL")
    cmd = ["cc", "-O1", "-o", helper, os.path.join(HERE, "he_aac_enc.c")]
    cmd += [f"-I{full}/include", f"{full}/lib/libfdk-aac.a", "-lm"] if full else ["-lfdk-aac"]
    if subprocess.run(cmd, capture_output=True).returncode:
        return None
    pcm = subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", f"{BURSTS}:d=6",
                          "-ac", "2", "-f", "s16le", "-"], capture_output=True).stdout
    enc = subprocess.run([helper, str(aot)], input=pcm, capture_output=True)
    if enc.returncode or not enc.stdout:
        return None                                     # library without HE-AAC
    write(w(f"xc_he{aot}.aac"), enc.stdout)
    p = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
                        "testsrc2=size=320x240:rate=25:duration=6", "-f", "aac", "-i",
                        w(f"xc_he{aot}.aac"), "-map", "0:v", "-map", "1:a",
                        "-pix_fmt", "yuv420p", *V_SMALL, "-c:a", "copy",
                        "-f", "mpegts", "-muxrate", "1500k", path],
                       capture_output=True, text=True)
    return path if p.returncode == 0 else None


@test
def xc_audio_he_aac():
    """HE-AAC and HE-AAC v2 in: full band and in sync if the AAC library can, a clear
    warning and a valid half-band stream if it cannot."""
    skip = xc_ready("fdk-aac", "twolame")
    if skip:
        return skip
    # CC3_BIN_FULL: a caricoder3 built against the full library, to test that case.
    binary = os.environ.get("CC3_BIN_FULL") or (ASAN if os.path.exists(ASAN) else BIN)
    res = []
    for aot, label in ((5, "HE-AAC"), (29, "HE-AAC v2")):
        src = he_aac_source(aot)
        if not src:
            return "SKIPPED: no fdk-aac library with HE-AAC to make the test stream " \
                   "(set CC3_FDK_FULL)"
        for to in ("aac", "mp2"):
            out = w(f"xc_he{aot}_{to}.ts")
            p = subprocess.run([binary, "-i", src, "-o", out, "--audio", to],
                               capture_output=True, text=True, timeout=120)
            need(p.returncode == 0 and "AddressSanitizer" not in p.stderr and
                 "runtime error" not in p.stderr, p.stderr[-600:])
            must_pass(out)
            decodes_clean(out, f"{label} to {to}: ")
            (codec, rate, ch), = probe_audio(out)
            if "has no SBR" in p.stderr:
                need(rate == 24000, f"{label} to {to}: {rate} Hz from a library without SBR")
                kind = "core only (24 kHz), warned"
            else:
                need((rate, ch) == (48000, 2), f"{label} to {to}: {rate} Hz, {ch} channel(s)")
                ms, _ = sync_ms(src, out, 0, min_likeness=0.25)
                need(abs(ms) < 1.0, f"{label} to {to}: audio is {ms:+.2f} ms off")
                kind = "full band, in sync"
        res.append(f"{label}: {kind}")
    return "; ".join(res)


@test
def live_xc_audio():
    """Live, CBR, three audio streams transcoded in their own threads."""
    skip = xc_ready("fdk-aac", "mpg123", "liba52")
    if skip:
        return skip
    src = av_source("xc_three", [A_MP2, A_AAC, A_AC3])
    st, cap, log = live(lambda p: p.play(src, rate=2_500_000), "-r", "2500k",
                        "--audio", "aac:96k", capture_name="live_xc_audio.ts", tail=2.0)
    out = w("live_xc_audio.ts")
    must_pass(out, cbr=True)
    must_match(src, out, only={0x100}, subset=True)
    need(probe_audio(out) == [("aac", 48000, 2)] * 3, str(probe_audio(out)))
    slack = []
    for i, pid in enumerate((0x101, 0x102, 0x103)):
        x = xstats(st, pid)
        need(x["mode"] == "transcode" and not x["failed"] and x["decode_errors"] == 0 and
             x["input_dropped"] == 0 and x["silence_ms"] == 0, f"PID {pid}: {x}")
        ms, n = sync_ms(src, out, i)
        need(abs(ms) < 1.0, f"PID {pid}: audio is {ms:+.2f} ms off")
        need(n > 48000 * 7.3, f"PID {pid}: only {n / 48000:.2f} s of audio")
        if "slack_ms" in x:
            slack.append(x["slack_ms"])
    need(st["output"]["late_units"] == 0 and st["output"]["dropped_units"] == 0,
         f"late {st['output']['late_units']}, dropped {st['output']['dropped_units']}")
    no_pause(cap)
    return f"3 streams to AAC in real time, in sync, nothing late or dropped " \
           f"(delay {st.get('delay_ms', 800)} ms)"


@test
def live_xc_audio_source_restart():
    """Source comes back on another clock while its audio is being transcoded."""
    skip = xc_ready("fdk-aac", "mpg123")
    if skip:
        return skip
    a = av_source("xc_ra", [A_MP2], duration=5)
    b = av_source("xc_rb", [A_MP2], duration=5, mux_extra=("-output_ts_offset", "7000"))

    def feed(p):
        p.play(a, rate=2_500_000)
        time.sleep(1.5)
        p.play(b, rate=2_500_000)

    st, cap, _ = live(feed, "-r", "2500k", "--audio", "aac", capture_name="live_xc_restart.ts",
                      tail=2.0)
    out = w("live_xc_restart.ts")
    must_pass(out, cbr=True, allow_late=True)
    need(st["clock"]["discontinuities"] == 1,
         f"{st['clock']['discontinuities']} discontinuities, expected 1")
    x = xstats(st, 0x101)
    need(not x["failed"] and x["decode_errors"] == 0 and x["input_dropped"] == 0, str(x))
    need(x["restarts"] == 1, f"audio time line taken up afresh {x['restarts']} times, expected 1")
    # The audio of both halves is there, on one time line that only moves forward.
    an = Analysis(out)
    pts = [r["pts"] for r in an.pes[0x101]]
    back = [i for i in range(1, len(pts)) if pts[i] <= pts[i - 1]]
    need(not back, f"audio time stamps go backwards at PES {back[:5]}")
    import av_sync
    n = len(av_sync.decode(out, 0))
    need(n > 48000 * 9.3, f"only {n / 48000:.2f} s of audio out of 10")
    late = an.lateness_ms()
    need(len(late) <= 3, f"{len(late)} PES left after their time")
    need(wrong_clock([a], out) == 0, "audio carries the wrong clock's timestamps")
    no_pause(cap)
    return f"{n / 48000:.2f} s of 10 s of audio delivered across the restart, time stamps " \
           f"only move forward, {len(late)} late"


# ---- video

def video_pts(path):
    """Presentation times (90 kHz) of the decoded pictures, in display order."""
    p = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                        "-show_entries", "frame=pts", "-of", "csv=p=0", path],
                       capture_output=True, text=True)
    return [int(x.strip(",")) for x in p.stdout.split() if x.strip(",").lstrip("-").isdigit()]


def probe_video(path):
    p = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                        "-show_entries",
                        "stream=codec_name,width,height,pix_fmt,field_order,r_frame_rate,"
                        "sample_aspect_ratio", "-of", "json", path],
                       capture_output=True, text=True)
    s = json.loads(p.stdout or "{}").get("streams", [{}])
    return s[0] if s else {}


def psnr(out, src):
    """
    Average PSNR of `out` against `src`, pictures paired by time stamp. A
    picture that is in the wrong place in time is compared with the wrong
    original, and on a moving test picture that shows at once.
    """
    # -copyts: pair pictures by the time stamps they carry, not by how far
    # each is from the start of its own file.
    return psnr_stream(out, src, "0:v:0")


def psnr_stream(out, src, out_stream, scale=None):
    """PSNR of one video stream of `out` (an ffmpeg stream specifier such as
    "0:v:1" or "0:p:2:v") against `src`, scaled to `scale` ("WxH") if given."""
    sc = f",scale={scale}:flags=area" if scale else ""
    p = subprocess.run(["ffmpeg", "-v", "info", "-copyts", "-i", out, "-i", src, "-lavfi",
                        f"[{out_stream}]format=yuv420p[a];[1:v]format=yuv420p{sc}[b];"
                        "[a][b]psnr=shortest=1:repeatlast=0",
                        "-an", "-f", "null", "-"], capture_output=True, text=True)
    for line in p.stderr.splitlines():
        if "PSNR" in line and "average:" in line:
            v = line.split("average:")[1].split()[0]
            return float("99" if v == "inf" else v)
    raise Fail("ffmpeg gave no PSNR figure: " + p.stderr[-300:])


def video_bitrate(path, pid):
    """Bit rate of the elementary stream on `pid`, from PES sizes and time stamps."""
    a = Analysis(path)
    lst = [r for r in a.pes[pid] if r["dts"] is not None]
    span = (lst[-1]["dts"] - lst[0]["dts"]) / 90000.0
    return sum(r["len"] for r in lst[:-1]) * 8 / span


def v_source(name, codec, extra=(), size="320x240", duration=8, pix="yuv420p", muxrate="2000k"):
    args = {"h264": ["-c:v", "libx264", "-preset", "veryfast", "-b:v", "900k", "-g", "25",
                     "-bf", "2"],
            "hevc": ["-c:v", "libx265", "-preset", "ultrafast", "-b:v", "900k",
                     "-x265-params", "log-level=error:keyint=25"],
            "mpeg2": ["-c:v", "mpeg2video", "-b:v", "1200k", "-g", "12", "-bf", "2"],
            "mpeg2i": ["-c:v", "mpeg2video", "-b:v", "1200k", "-g", "12", "-bf", "2",
                       "-flags", "+ilme+ildct", "-top", "1"]}[codec]
    path = w(name + ".ts")
    if name in _cache and os.path.exists(path):
        return path
    p = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                        "-f", "lavfi", "-i", f"testsrc2=size={size}:rate=25:duration={duration}",
                        "-f", "lavfi", "-i", f"{BURSTS}:d={duration}",
                        "-pix_fmt", pix, *args, *extra, "-c:a", "mp2", "-b:a", "128k",
                        "-f", "mpegts", "-muxrate", muxrate, path],
                       capture_output=True, text=True)
    need(p.returncode == 0, f"ffmpeg could not make {name}: {p.stderr[-400:]}")
    _cache[name] = True
    return path


def check_video(src, out, st, what, codec, rate, pix="yuv420p", min_psnr=27.0):
    must_pass(out)
    decodes_clean(out, what + ": ")
    v = probe_video(out)
    sv = probe_video(src)
    need(v.get("codec_name") == codec, f"{what}: output video is {v.get('codec_name')}")
    need((v["width"], v["height"]) == (sv["width"], sv["height"]),
         f"{what}: picture is {v['width']}x{v['height']}")
    need(v["pix_fmt"] == pix, f"{what}: pixel format {v['pix_fmt']}, expected {pix}")
    need(v["r_frame_rate"] == sv["r_frame_rate"], f"{what}: frame rate {v['r_frame_rate']}")
    # Every picture that comes out sits at a time the source had a picture at,
    # and all but the first few (before the decoder's first key frame) are there.
    a, b = video_pts(src), video_pts(out)
    need(b == sorted(b) and len(set(b)) == len(b), f"{what}: picture times not in order")
    need(set(b) <= set(a), f"{what}: {len(set(b) - set(a))} pictures at times the source "
                           f"had none")
    need(len(b) >= len(a) - 3, f"{what}: {len(b)} of {len(a)} pictures came out")
    q = psnr(out, src)
    need(q >= min_psnr, f"{what}: PSNR {q:.1f} dB against the source (pictures in the "
                        f"wrong place, or badly coded)")
    got = video_bitrate(out, 0x100)
    need(0.7 * rate < got < 1.2 * rate, f"{what}: {got / 1000:.0f} kb/s, asked for "
                                        f"{rate / 1000:.0f}")
    x = xstats(st, 0x100)
    need(x["mode"] == "transcode" and not x["failed"] and x["decode_errors"] == 0, f"{what}: {x}")
    need(st["output"]["late_units"] == 0 and st["output"]["dropped_units"] == 0,
         f"{what}: {st['output']['late_units']} late, {st['output']['dropped_units']} dropped")
    # the audio went through untouched and still lines up
    must_match(src, out, only={0x101})
    return q, got, len(b), len(a)


@test
def xc_video_cpu_all_four():
    """H.264 and H.265 in, H.264 and H.265 out, on the CPU: every picture, in place."""
    skip = xc_ready("libavcodec", "x264", "x265")
    if skip:
        return skip
    res = []
    for src_codec in ("h264", "hevc"):
        src = v_source("xc_v_" + src_codec, src_codec)
        for to in ("h264", "hevc"):
            out = w(f"xc_v_{src_codec}_to_{to}.ts")
            st = run_file(src, out, "--video", f"{to}:500k")
            q, got, n, total = check_video(src, out, st, f"{src_codec} to {to}", to, 500_000)
            res.append(f"{src_codec}>{to} {n}/{total} pictures, {q:.0f} dB, {got / 1000:.0f} kb/s")
    return "; ".join(res)


@test
def xc_video_mpeg2_in():
    """MPEG-2 video (stream type 0x02) decoded on the CPU and encoded as H.264 and H.265."""
    skip = xc_ready("libavcodec", "x264", "x265")
    if skip:
        return skip
    res = []
    src = v_source("xc_v_mpeg2", "mpeg2")
    for to in ("h264", "hevc"):
        out = w(f"xc_v_mpeg2_to_{to}.ts")
        st = run_file(src, out, "--video", f"{to}:500k")
        q, got, n, total = check_video(src, out, st, f"mpeg2 to {to}", to, 500_000)
        need(xstats(st, 0x100)["from"] == "mpeg2", f"input codec reported as {xstats(st, 0x100).get('from')}")
        res.append(f"mpeg2>{to} {n}/{total} pictures, {q:.0f} dB, {got / 1000:.0f} kb/s")
    # interlaced MPEG-2, as satellite feeds are
    # (ffmpeg's MPEG-2 overshoots its rate on this; a 2 Mb/s mux leaves its own audio late)
    src = v_source("xc_v_mpeg2i", "mpeg2i", size="720x576", muxrate="3500k")
    need(Analysis(src).late_pes() == 0, "the test source itself has late PES")
    out = w("xc_v_mpeg2i_to_h264.ts")
    st = run_file(src, out, "--video", "h264:800k")
    q, got, n, total = check_video(src, out, st, "interlaced mpeg2 to h264", "h264", 800_000, min_psnr=24.0)
    res.append(f"interlaced mpeg2>h264 {n}/{total} pictures, {q:.0f} dB")
    return "; ".join(res)


@test
def xc_video_sizes_follow_shape():
    """-1xH, Wx-1 and WxH:fit: the size follows the source's display shape (an anamorphic 960x720 shown 16:9, a 4:3 one)."""
    skip = xc_ready("libavcodec", "x264")
    if skip:
        return skip
    ana, sq = w("xc_ana.ts"), w("xc_sq43.ts")
    for path, sar in ((ana, "4/3"), (sq, "1")):
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=960x720:rate=25", "-t", "3",
                        "-vf", f"setsar={sar}", "-c:v", "libx264", "-preset", "ultrafast", "-b:v", "2M", "-f", "mpegts", path],
                       check=True)

    def shape(path):
        p = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v", "-show_entries",
                            "stream=width,height,sample_aspect_ratio", "-of", "csv=p=0", path],
                           capture_output=True, text=True)
        return [tuple(l.split(",")) for l in p.stdout.split()]
    cases = [(ana, "-1x720", ("1280", "720", "1:1")), (ana, "1280x-1", ("1280", "720", "1:1")),
             (ana, "-1x480", ("854", "480", "1:1")), (ana, "1280x720:fit", ("1280", "720", "1:1")),
             (sq, "1280x720:fit", ("960", "720", "1:1")), (sq, "-1x480", ("640", "480", "1:1")),
             (ana, "640x360", ("640", "360", "1:1"))]
    for src, size, want in cases:
        out = w("xc_size.ts")
        run_file(src, out, "--video", f"{size}:h264:800k")
        got = shape(out)
        need(got and got[0] == want, f"{os.path.basename(src)} at {size}: {got}, expected {want}")
    out = w("xc_size_ladder.ts")
    st = run_file(ana, out, "--video", "h264:1M", "--video", "-1x720:h264:800k", "--video", "-1x480:h264:500k")
    sizes = [x.get("size") for x in st["service"]["streams"] if x["kind"] == "video"]
    need(sizes == ["960x720", "1280x720", "854x480"], f"ladder: {sizes}")
    return f"{len(cases)} sizes as the display shape says; a ladder 960x720 (16:9) / 1280x720 / 854x480"


@test
def xc_video_bframes_profile():
    """--bframes and --profile: H.264 high/main/baseline, B-frames on and off; H.265 main from a 10-bit source."""
    skip = xc_ready("libavcodec", "x264", "x265")
    if skip:
        return skip
    src = w("xc_bp.ts")
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25", "-t", "3",
                    "-c:v", "libx264", "-preset", "ultrafast", "-b:v", "1M", "-f", "mpegts", src], check=True)
    src10 = w("xc_bp10.ts")
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25", "-t", "3",
                    "-pix_fmt", "yuv420p10le", "-c:v", "libx265", "-x265-params", "log-level=none", "-b:v", "1M",
                    "-f", "mpegts", src10], check=True)

    def look(path):
        p = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=profile,pix_fmt",
                            "-of", "csv=p=0", path], capture_output=True, text=True)
        f = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_frames", "-show_entries",
                            "frame=pict_type", "-of", "csv=p=0", path], capture_output=True, text=True)
        prof = p.stdout.splitlines()[0].split(",")
        return prof[0], prof[1], f.stdout.split().count("B")
    cases = [(src, "h264", [], "High", True), (src, "h264", ["--bframes", "0"], "High", False),
             (src, "h264", ["--profile", "main"], "Main", True), (src, "h264", ["--profile", "baseline"], "Constrained Baseline", False),
             (src, "hevc", ["--bframes", "0"], "Main", False), (src10, "hevc", ["--profile", "main"], "Main", None)]
    for s_, codec, opts, prof, bf in cases:
        out = w("xc_bp_out.ts")
        run_file(s_, out, "--video", f"{codec}:800k", *opts)
        p, fmt, nb = look(out)
        need(p == prof and (bf is None or (nb > 0) == bf), f"{codec} {opts}: profile {p}, {nb} B-frames")
        if s_ is src10:
            need(fmt == "yuv420p", f"H.265 main from 10 bits: {fmt}")
    return f"{len(cases)} profile/B-frame settings as asked; H.265 main makes 8 bits of a 10-bit source"


@test
def thumbnails_copy_and_transcode():
    """--thumb: a JPEG of the picture, from the transcoder's decoder and, on a copied stream, a decoded key frame."""
    skip = xc_ready("libavcodec", "x264")
    if skip:
        return skip
    src = _real_h264_aac(8)
    if not src:
        return "SKIPPED: no ffmpeg to make a real stream"
    res = []
    for mode, args in (("copy", []), ("transcode", ["--video", "h264:500k"])):
        jpg = w(f"thumb_{mode}.jpg")
        if os.path.exists(jpg):
            os.remove(jpg)
        run_file(src, w(f"thumb_{mode}.ts"), "--thumb", jpg, "--thumb-interval", "1", *args)
        need(os.path.exists(jpg) and os.path.getsize(jpg) > 500, f"{mode}: no thumbnail written")
        p = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=codec_name,width,height",
                            "-of", "csv=p=0", jpg], capture_output=True, text=True)
        need(p.stdout.strip().startswith("mjpeg,320,180"), f"{mode}: ffprobe says {p.stdout.strip()!r}")
        # it decodes to a picture that is not flat
        p = subprocess.run(["ffmpeg", "-v", "error", "-i", jpg, "-vf", "signalstats", "-f", "null", "-"], capture_output=True, text=True)
        need(p.returncode == 0, f"{mode}: the JPEG does not decode: {p.stderr[-200:]}")
        res.append(f"{mode}: {os.path.getsize(jpg)} bytes, 320x180")
    return "; ".join(res)


@test
def xc_video_and_audio_together():
    """Video and audio transcoded in one run stay in sync with each other."""
    skip = xc_ready("libavcodec", "x264", "fdk-aac", "mpg123")
    if skip:
        return skip
    src = v_source("xc_v_h264", "h264")
    out = w("xc_va.ts")
    st = run_file(src, out, "--video", "h264:500k", "--audio", "aac:96k")
    must_pass(out)
    decodes_clean(out)
    a, b = video_pts(src), video_pts(out)
    need(set(b) <= set(a) and len(b) >= len(a) - 3, f"{len(b)} of {len(a)} pictures")
    q = psnr(out, src)
    need(q >= 27, f"PSNR {q:.1f} dB")
    ms, n = sync_ms(src, out, 0)
    need(abs(ms) < 1.0, f"audio is {ms:+.2f} ms off")
    need(st["output"]["late_units"] == 0 and st["output"]["dropped_units"] == 0, "late or dropped")
    return f"pictures at their original times ({q:.0f} dB), audio {ms:+.3f} ms off"


@test
def xc_video_formats():
    """10-bit, interlaced, anamorphic and HD sources keep their shape through the encoder."""
    skip = xc_ready("libavcodec", "x264", "x265")
    if skip:
        return skip
    res = []

    # 10-bit H.265: stays 10-bit as H.265, becomes 8-bit as H.264
    src = v_source("xc_v_hevc10", "hevc", pix="yuv420p10le")
    need(probe_video(src)["pix_fmt"] == "yuv420p10le", "test source is not 10-bit")
    out = w("xc_v_hevc10_hevc.ts")
    st = run_file(src, out, "--video", "hevc:500k")
    check_video(src, out, st, "10-bit hevc to hevc", "hevc", 500_000, pix="yuv420p10le")
    out = w("xc_v_hevc10_h264.ts")
    st = run_file(src, out, "--video", "h264:500k")
    q, _, _, _ = check_video(src, out, st, "10-bit hevc to h264", "h264", 500_000)
    res.append(f"10-bit kept in H.265, 8-bit in H.264 ({q:.0f} dB)")

    # interlaced, top field first, 720x576 with non-square pixels
    src = v_source("xc_v_il", "h264", size="720x576",
                   extra=("-flags", "+ildct+ilme", "-top", "1", "-aspect", "16:9"))
    sv = probe_video(src)
    need(sv["field_order"] in ("tt", "tb"), f"test source is {sv['field_order']}")
    out = w("xc_v_il_h264.ts")
    st = run_file(src, out, "--video", "h264:1500k")
    check_video(src, out, st, "interlaced h264 to h264", "h264", 1_500_000, min_psnr=30)
    v = probe_video(out)
    need(v["field_order"] in ("tt", "tb"), f"interlaced came out as {v['field_order']}")
    need(v["sample_aspect_ratio"] == sv["sample_aspect_ratio"],
         f"pixel shape {v['sample_aspect_ratio']}, source {sv['sample_aspect_ratio']}")
    res.append(f"interlaced top-first 720x576 stays interlaced, pixel shape "
               f"{v['sample_aspect_ratio']} kept")

    # 1280x720
    src = v_source("xc_v_hd", "h264", size="1280x720", duration=4)
    out = w("xc_v_hd_hevc.ts")
    st = run_file(src, out, "--video", "hevc:1500k", "--preset", "ultrafast")
    check_video(src, out, st, "720p h264 to hevc", "hevc", 1_500_000, min_psnr=30)
    res.append("1280x720 through")
    return "; ".join(res)


@test
def xc_video_send_times():
    """New video is sent at its own bit rate: every picture complete before it is due,
    none earlier than the buffer allows, the receiver never holding more than the
    buffer, no burst for a key frame; in VBR and in CBR. --vbv and --gop do what they say."""
    skip = xc_ready("libavcodec", "x264")
    if skip:
        return skip
    # (a source whose clock does not start at 0, like any real one: the first
    # new pictures are sent before anything in the input was)
    src = v_source("xc_v_h264_late", "h264", extra=("-output_ts_offset", "20"))
    res = []
    rate = 500000
    for name, args, cbr in (("vbr", (), False), ("cbr", ("-r", "1200k"), True)):
        out = w(f"xc_v_lead_{name}.ts")
        st = run_file(src, out, "--video", "h264:500k", *args)
        must_pass(out, cbr=cbr)
        a = Analysis(out)
        lead = [x for x in _leads(a, 0x100)]
        need(min(lead) > 15, f"{name}: a picture was complete only {min(lead):.0f} ms "
                             f"before its decode time")
        need(max(lead) < 1000 + 40 + 60, f"{name}: a picture arrived {max(lead):.0f} ms early, "
                                         f"more than the 1000 ms buffer")
        # what the receiver must hold: never more than the encoder's buffer and a picture
        peak = a.buffer_peak(0x100)
        need(peak < rate / 8 * 1.04 * 1.1 + 4000, f"{name}: the receiver would hold "
             f"{peak:.0f} bytes of video; the buffer is {rate // 8} bytes")
        # the video is spread at its own bit rate: no burst for a key frame
        top = a.peak_rate(0x100, 400)
        need(top < rate * 1.25, f"{name}: video took {top / 1000:.0f} kb/s over 400 ms; "
                                f"it is coded at {rate // 1000} kb/s")
        res.append(f"{name}: complete {min(lead):.0f} to {max(lead):.0f} ms early, receiver "
                   f"holds at most {peak / 1000:.0f} kB, never above {top / 1000:.0f} kb/s")
    out = w("xc_v_lead_small.ts")
    st = run_file(src, out, "--video", "h264:500k", "--vbv", "300", "--gop", "500")
    must_pass(out)
    a = Analysis(out)
    lead = _leads(a, 0x100)
    need(min(lead) > 15 and max(lead) < 300 + 40 + 60,
         f"--vbv 300: complete {min(lead):.0f} to {max(lead):.0f} ms early")
    peak = a.buffer_peak(0x100)
    need(peak < rate / 8 * 0.34 * 1.1 + 4000, f"--vbv 300: the receiver would hold {peak:.0f} bytes")
    keys = key_frame_times(out)[1:]      # (the first picture is a key frame anyway)
    gaps = [b - a for a, b in zip(keys, keys[1:])]
    # key frames sit on the first picture at or after each 500 ms mark of the
    # stream's own clock (so renditions of one channel agree): at 25 fps that is
    # 12 or 13 pictures apart, never more, and never more than a picture past a mark
    need(gaps and max(gaps) <= 0.52 + 0.001 and min(gaps) >= 0.48 - 0.001,
         f"--gop 500: key frames {min(gaps, default=0):.2f} to {max(gaps, default=0):.2f} s apart")
    past = [t - (t // 0.5) * 0.5 for t in keys]
    need(max(past) < 0.04 + 0.001, f"--gop 500: a key frame {max(past) * 1000:.0f} ms past a 500 ms mark")
    gaps = [round(g * 25) for g in gaps]
    res.append(f"--vbv 300: at most {max(lead):.0f} ms early and {peak / 1000:.0f} kB held; "
               f"--gop 500 a key frame every {max(gaps)} pictures at most")
    # a long buffer needs a long delay: the default follows, a short one is refused
    out = w("xc_v_lead_big.ts")
    st = run_file(src, out, "--video", "h264:500k", "--vbv", "4000")
    must_pass(out)
    need(xstats(st, 0x100)["frames_out"] >= 198 and st["output"]["dropped_units"] == 0,
         f"--vbv 4000 at the default delay: {st['output']}")
    p = subprocess.run([BIN, "-q", "-i", src, "-o", out, "--video", "h264:500k", "--vbv", "4000",
                        "--delay", "3000"], capture_output=True, text=True)
    need(p.returncode == 2 and "too short" in p.stderr,
         f"--vbv 4000 with --delay 3000 was accepted (exit {p.returncode})")
    res.append("--vbv 4000: the default delay grows with it, --delay 3000 is refused")
    return "; ".join(res)


@test
def xc_video_lead_holds():
    """A minute of video that is hard to code, so the encoder uses all of its bit rate
    all of the time: pictures keep arriving well before they are due, with x264 and
    with x265 (which runs a little over its rate), in VBR and in a CBR mux."""
    skip = xc_ready("libavcodec", "x264", "x265")
    if skip:
        return skip
    src = w("xc_v_noisy2.ts")
    if "xc_v_noisy2" not in _cache or not os.path.exists(src):
        p = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
                            "-i", "testsrc2=size=320x240:rate=25:duration=60,noise=alls=40:allf=t",
                            "-f", "lavfi", "-i", f"{BURSTS}:d=60", "-pix_fmt", "yuv420p",
                            "-c:v", "libx264", "-preset", "ultrafast", "-b:v", "1500k",
                            "-maxrate", "1500k", "-bufsize", "1500k", "-g", "50",
                            "-c:a", "mp2", "-b:a", "128k", "-output_ts_offset", "30",
                            "-f", "mpegts", "-muxrate", "4000k", src],
                           capture_output=True, text=True)
        need(p.returncode == 0, f"ffmpeg could not make the noisy stream: {p.stderr[-400:]}")
        must_pass(src, cbr=True)            # the judge of "late" must not be late itself
        _cache["xc_v_noisy2"] = True
    res = []
    for what, args, cbr in (("x264", ("--video", "h264:150k"), False),
                            ("x264 in CBR", ("--video", "h264:150k", "-r", "400k"), True),
                            ("x265", ("--video", "hevc:200k"), False)):
        out = w("xc_v_noisy_out.ts")
        st = run_file(src, out, *args, binary=BIN)
        must_pass(out, cbr=cbr)
        a = Analysis(out)
        lead = _leads(a, 0x100)
        tail = lead[len(lead) * 2 // 3:]
        need(len(lead) >= 1450, f"{what}: {len(lead)} pictures")
        # (x265 overruns its buffer on its first pictures; what matters is that the
        # schedule does not slide afterwards)
        need(min(lead) > 20 and min(tail) > 250,
             f"{what}: pictures complete as little as {min(lead):.0f} ms before they are due "
             f"({min(tail):.0f} ms in the last 20 s): the schedule is sliding")
        got = video_bitrate(out, 0x100)
        res.append(f"{what}: {got / 1000:.0f} kb/s, complete {min(lead):.0f} ms early at "
                   f"least, {min(tail):.0f} in the last 20 s")
    return "; ".join(res)


def key_frame_times(path, stream="v:0"):
    """Presentation times (s) of the key frames of a video stream, in order."""
    p = subprocess.run(["ffprobe", "-v", "error", "-select_streams", stream, "-show_entries",
                        "frame=key_frame,pts_time", "-of", "csv=p=0", path],
                       capture_output=True, text=True)
    out = []
    for line in p.stdout.split():
        k, t = line.strip(",").split(",")[:2]
        if k == "1":
            out.append(float(t))
    return out


def _leads(a, pid):
    out = []
    for r in a.pes[pid]:
        t = a.stc_at(r["last"] + 1)
        if t is not None and r["dts"] is not None:
            out.append((r["dts"] * 300 - t) / 27000.0)
    return out


def es_join_source(name, parts, size="720x576", seconds=4):
    """One H.264 stream whose format changes on the way: each part is coded
    by itself (ffmpeg arguments), the elementary streams are joined end to
    end and wrapped as one transport stream."""
    path = w(name + ".ts")
    if name in _cache and os.path.exists(path):
        return path
    es = b""
    for i, extra in enumerate(parts):
        f = w(f"{name}_{i}.h264")
        p = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
                            "-i", f"testsrc2=size={size}:rate=25:duration={seconds}",
                            "-pix_fmt", "yuv420p", "-c:v", "libx264", "-preset", "veryfast",
                            "-b:v", "1500k", "-g", "25", "-bf", "0", *extra, "-f", "h264", f],
                           capture_output=True, text=True)
        need(p.returncode == 0, f"ffmpeg could not make {name}: {p.stderr[-400:]}")
        es += read(f)
    joined = w(name + ".h264")
    with open(joined, "wb") as f:
        f.write(es)
    p = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-r", "25",
                        "-i", joined, "-c", "copy", "-f", "mpegts", "-muxrate", "3000k", path],
                       capture_output=True, text=True)
    need(p.returncode == 0, f"ffmpeg could not wrap {name}: {p.stderr[-400:]}")
    _cache[name] = True
    return path


def frame_shapes(path):
    """Per picture, in order: (pixel shape, interlaced?)."""
    p = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                        "frame=sample_aspect_ratio,interlaced_frame", "-of", "json", path],
                       capture_output=True, text=True)
    return [(f.get("sample_aspect_ratio"), f.get("interlaced_frame"))
            for f in json.loads(p.stdout)["frames"]]


@test
def xc_video_shape_change():
    """A channel that goes from 4:3 to 16:9, or from progressive to interlaced, while
    running: the new video follows, at the picture where the source changed."""
    skip = xc_ready("libavcodec", "x264")
    if skip:
        return skip
    res = []
    for name, parts, what in (
            ("xc_v_sarchg", (("-aspect", "4:3"), ("-aspect", "16:9")), "4:3 to 16:9"),
            ("xc_v_ilchg", (("-aspect", "4:3"), ("-aspect", "4:3", "-flags", "+ildct+ilme",
                                                 "-top", "1")), "progressive to interlaced")):
        src = es_join_source(name, parts)
        out = w(name + "_out.ts")
        st = run_file(src, out, "--video", "h264:1500k")
        must_pass(out)
        decodes_clean(out, what + ": ")
        a, b = frame_shapes(src), frame_shapes(out)
        need(len(set(a)) == 2, f"{what}: the test stream does not change ({set(a)})")
        need(len(b) >= len(a) - 3, f"{what}: {len(b)} of {len(a)} pictures came out")
        need(b == a[len(a) - len(b):], f"{what}: the output has {sorted(set(b))} "
             f"({[b.count(x) for x in sorted(set(b))]} pictures), the source {sorted(set(a))} "
             f"({[a.count(x) for x in sorted(set(a))]})")
        need(xstats(st, 0x100)["frames_out"] == len(b), "pictures counted and found differ")
        res.append(f"{what}: {len(b)}/{len(a)} pictures, each as the source had it")
    return "; ".join(res)


@test
def xc_video_codec_change():
    """The source changes the coding of its video (H.264, then H.265, then back) with
    a new PMT: each transcoder gives out what it still held before it goes."""
    skip = xc_ready("libavcodec", "x264", "x265")
    if skip:
        return skip
    parts = [v_source("xc_v_cc0", "h264", duration=4),
             v_source("xc_v_cc1c", "hevc", duration=4, extra=("-output_ts_offset", "4.2")),
             v_source("xc_v_cc2c", "h264", duration=4, extra=("-output_ts_offset", "8.4"))]
    src = w("xc_v_codec_change.ts")
    with open(src, "wb") as f:
        for p in parts:
            f.write(read(p))
    out = w("xc_v_codec_change_out.ts")
    st = run_file(src, out, "--video", "h264:500k")
    an, o = Analysis(src), Analysis(out)
    need(o.sync_errors == 0, "output lost sync")
    n_in, n_out = len(an.pes[0x100]), len(o.pes[0x100])
    need(n_out >= n_in - 9, f"{n_out} of {n_in} pictures came out (a transcoder was "
                            f"dropped with pictures still in it)")
    need({r["pts"] for r in o.pes[0x100]} <= {r["pts"] for r in an.pes[0x100]},
         "pictures at times the source had none")
    types = {s["pid"]: s["type"] for s in o.pmt["streams"]}
    need(types[0x100] == 0x1B, f"PMT says type 0x{types[0x100]:02X} at the end")
    d = [r["dts"] for r in o.pes[0x100]]
    need(all(y > x for x, y in zip(d, d[1:])), "decode times go backwards at a change")
    # the new stream is sent after the end of the old one, not on top of it
    peak = o.buffer_peak(0x100)
    need(peak < 500000 / 8 * 1.04 * 1.1 + 6000, f"a receiver would hold {peak:.0f} bytes of "
         f"video around a change; the buffer is 62500")
    # (the copied audio of the joined test stream overlaps itself at the joins,
    # so ffmpeg's complaints about the whole file prove nothing: count pictures)
    got = video_pts(out)
    need(len(got) >= n_in - 12 and got == sorted(got) and len(set(got)) == len(got),
         f"{len(got)} pictures decode, or not in order")
    return f"{n_out} of {n_in} pictures out across two changes of coding, all H.264, " \
           f"{len(got)} decode, times only move forward"


def fakecpu_lib():
    lib = w("fakecpu.so")
    if not os.path.exists(lib):
        p = subprocess.run(["cc", "-shared", "-fPIC", "-O1", "-o", lib,
                            os.path.join(HERE, "fakecpu.c")], capture_output=True, text=True)
        if p.returncode:
            return None
    return lib


@test
def xc_video_many_processors():
    """On a server with 64 processors the codecs hold back no more pictures than on a
    small machine, so the default delay is enough there too."""
    skip = xc_ready("libavcodec", "x264", "x265")
    if skip:
        return skip
    lib = fakecpu_lib()
    if not lib:
        return "SKIPPED: could not build tests/fakecpu.c"
    src = v_source("xc_v_1080", "h264", size="1920x1080", duration=6)
    res = []
    for codec in ("h264", "hevc"):
        slack = {}
        for cpus in (2, 64):
            out = w(f"xc_v_cpus_{codec}_{cpus}.ts")
            sf = out + ".json"
            p = subprocess.run([BIN, "-q", "-i", src, "-o", out, "--stats-file", sf,
                                "--video", f"{codec}:3M"], capture_output=True, text=True,
                               timeout=600, env=dict(os.environ, LD_PRELOAD=lib,
                                                     FAKE_CPUS=str(cpus)))
            need(p.returncode == 0, p.stderr[-400:])
            st = json.load(open(sf))
            x = xstats(st, 0x100)
            need(x["frames_out"] >= 140, f"{codec}, {cpus} processors: {x['frames_out']} pictures")
            slack[cpus] = x["slack_min_ms"]
            must_pass(out)
        need(slack[64] > 1000, f"{codec}: with 64 processors the video has {slack[64]:.0f} ms "
                               f"of the 3 s default delay to spare")
        need(slack[2] - slack[64] < 400, f"{codec}: {slack[2]:.0f} ms to spare with 2 "
                                         f"processors, {slack[64]:.0f} with 64")
        res.append(f"{codec}: {slack[2]:.0f} ms to spare with 2 processors, "
                   f"{slack[64]:.0f} ms with 64")
    return "; ".join(res) + " (of the default 3 s delay, 1080p)"


@test
def xc_audio_decoder_buffer():
    """New audio stays inside a standard decoder's audio buffer (3584 bytes) and well
    ahead of its time, at low and high bit rates, also beside new video in a tight CBR
    mux. (MP2 at 384 kb/s goes over the buffer rather than lose its lead.)"""
    skip = xc_ready("libavcodec", "x264", "fdk-aac", "mpg123", "twolame", "lame")
    if skip:
        return skip
    src = av_source("xc_tstd", [A_AAC], duration=10)
    res = []
    # (CBR throughout: only there does every packet have an exact time)
    for spec, frame_ms, extra, cbr in (
            ("mp2:192k", 24.0, ("-r", "2800k"), False), ("mp2:384k", 24.0, ("-r", "3000k"), False),
            ("mp2:64k", 24.0, ("-r", "2800k"), False),
            ("aac:128k", 1024 / 48.0, ("-r", "2800k"), False),
            ("aac:256k", 1024 / 48.0, ("-r", "2800k"), False),
            ("mp3:128k", 24.0, ("-r", "2800k"), False),
            ("mp2:192k", 24.0, ("--video", "h264:600k", "-r", "950k"), True)):
        out = w("xc_tstd_out.ts")
        st = run_file(src, out, "--audio", spec, *extra)
        must_pass(out, cbr=True)
        a = Analysis(out)
        peak = a.buffer_peak(0x101, frame_ms)
        # (MP2 at 384 kb/s is the stated exception: there the 75 ms the audio is
        # kept ahead by is more than the buffer holds, and being ahead wins)
        limit = 5000 if spec == "mp2:384k" else 3584
        need(peak <= limit, f"{spec}{' beside video in CBR' if cbr else ''}: a decoder would "
                            f"hold {peak:.0f} bytes of audio; the limit here is {limit}")
        lead = [(r["dts"] * 300 - a.stc_at(r["last"] + 1)) / 27000.0 for r in a.pes[0x101]
                if a.stc_at(r["last"] + 1) is not None]
        need(min(lead) > 50, f"{spec}: an audio packet was complete only {min(lead):.0f} ms "
                             f"before it was due")
        res.append(f"{spec}{' + video, CBR' if cbr else ''}: {peak:.0f} bytes, "
                   f"{min(lead):.0f} ms early at least")
    return "; ".join(res)


@test
def xc_video_renditions():
    """Three renditions of one channel in one transport stream: a program each,
    audio shared, key frames on the same pictures, every program playable."""
    skip = xc_ready("libavcodec", "x264", "fdk-aac", "mpg123")
    if skip:
        return skip
    src = v_source("xc_v_640", "h264", size="640x360", duration=8,
                   extra=("-output_ts_offset", "100"))
    out = w("xc_v_renditions.ts")
    st = run_file(src, out, "--video", "h264:900k", "--video", "426x240:h264:400k",
                  "--video", "320x180:h264:200k", "--audio", "aac:96k")
    need(st["service"]["programs"] == 3, f"{st['service']['programs']} programs")
    o = Analysis(out)
    progs = o.pat["programs"]
    need(sorted(progs) == [1, 2, 3], f"PAT lists programs {sorted(progs)}")
    need(o.sync_errors == 0 and not o.cc_errors, "output damaged")
    # one PMT per program, the audio PID and the PCR PID in all three
    pmts, vpid = {}, {}
    for n in (1, 2, 3):
        an = Analysis(out, program=n)
        pmts[n] = an.pmt
        need(an.pmt["pcr_pid"] == 0x100, f"program {n}: PCR PID 0x{an.pmt['pcr_pid']:04X}")
        pids = {s_["pid"]: s_["type"] for s_ in an.pmt["streams"]}
        need(pids.get(0x101) == 0x0F, f"program {n}: audio {pids}")
        vids = [p_ for p_, t in pids.items() if t == 0x1B]
        need(len(vids) == 1, f"program {n}: video PIDs {vids}")
        vpid[n] = vids[0]
    # the first keeps the source's PID; the others go above everything the
    # input uses (its PMT is on 0x1000), so that a stream the source adds later
    # (an audio on 0x102, say) does not land on a rendition
    need(vpid == {1: 0x100, 2: 0x1001, 3: 0x1002}, f"video PIDs {vpid}")
    need({progs[n] for n in (2, 3)} == {0x1009, 0x100A}, f"PMT PIDs of the further programs: {progs}")
    need(len({r["sha"] for r in o.pes[0x101]}) == len(o.pes[0x101]), "audio PES repeated")
    # every program decodes, at its size, close to the (scaled) source
    res = []
    sizes = {1: (640, 360), 2: (426, 240), 3: (320, 180)}
    for n in (1, 2, 3):
        p = subprocess.run(["ffprobe", "-v", "error", "-select_streams", f"p:{n}:v",
                            "-show_entries", "stream=width,height", "-of", "csv=p=0", out],
                           capture_output=True, text=True)
        got = tuple(int(x) for x in p.stdout.split()[0].split(",")[:2])
        need(got == sizes[n], f"program {n} is {got}, expected {sizes[n]}")
        q = psnr_stream(out, src, f"0:p:{n}:v", scale=f"{sizes[n][0]}:{sizes[n][1]}")
        need(q >= 30, f"program {n}: PSNR {q:.1f} dB against the scaled source")
        res.append(f"{sizes[n][0]}x{sizes[n][1]} {q:.0f} dB")
    # key frames on the same pictures, every 2 s
    keys = {n: key_frame_times(out, f"p:{n}:v")[1:] for n in (1, 2, 3)}
    need(keys[1] == keys[2] == keys[3], f"key frames differ between renditions: "
         f"{keys[1][:4]} / {keys[2][:4]} / {keys[3][:4]}")
    gaps = [b - a for a, b in zip(keys[1], keys[1][1:])]
    need(gaps and 1.95 < min(gaps) and max(gaps) < 2.05, f"key frames {gaps} s apart")
    need(len(video_pts(out)) >= 195, "pictures missing from the first rendition")
    ms, _ = sync_ms(src, out, 0)
    need(abs(ms) < 1.0, f"audio {ms:+.2f} ms off")
    # ffmpeg's HLS packager, the reference for the later packager stage: every
    # rendition cuts into segments at the same times
    seg_dir = w("hls_out")
    shutil.rmtree(seg_dir, ignore_errors=True)
    os.makedirs(seg_dir)
    p = subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", out,
                        "-map", "0:p:1:v", "-map", "0:p:2:v", "-map", "0:p:3:v", "-map", "0:a:0",
                        "-c", "copy", "-f", "hls", "-hls_time", "2", "-hls_list_size", "0",
                        "-var_stream_map",
                        "v:0,agroup:a v:1,agroup:a v:2,agroup:a a:0,agroup:a",
                        "-master_pl_name", "master.m3u8",
                        os.path.join(seg_dir, "r%v.m3u8")], capture_output=True, text=True)
    need(p.returncode == 0, "ffmpeg could not package the output as HLS: " + p.stderr[-300:])
    durs = []
    for v in (0, 1, 2):
        pl = open(os.path.join(seg_dir, f"r{v}.m3u8")).read()
        durs.append([float(l.split(":")[1].rstrip(",")) for l in pl.splitlines()
                     if l.startswith("#EXTINF")])
    need(all(len(d) >= 3 for d in durs), f"too few segments: {[len(d) for d in durs]}")
    need(all(abs(a - b) < 0.05 for d in durs[1:] for a, b in zip(durs[0], d)),
         f"segment lengths differ between renditions: {durs}")
    return f"3 programs ({', '.join(res)}), one audio PID in all, key frames on the same " \
           f"pictures every 2 s, HLS segments aligned ({len(durs[0])} per rendition)"


@test
def xc_video_renditions_interlaced():
    """An interlaced source made into a smaller rendition comes out progressive
    (deinterlaced on the way), the full-size one still as fields."""
    skip = xc_ready("libavcodec", "x264")
    if skip:
        return skip
    src = v_source("xc_v_il_src", "h264", size="720x576",
                   extra=("-flags", "+ildct+ilme", "-top", "1", "-aspect", "16:9"))
    out = w("xc_v_il_renditions.ts")
    st = run_file(src, out, "--video", "h264:1500k", "--video", "640x360:h264:600k")
    must_pass(out)
    decodes_clean(out)
    full = probe_video(out)
    p = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "p:2:v", "-show_entries",
                        "stream=width,height,field_order,sample_aspect_ratio",
                        "-of", "json", out], capture_output=True, text=True)
    small = json.loads(p.stdout)["streams"][0]
    need(full["field_order"] in ("tt", "tb"), f"full size came out {full['field_order']}")
    need((small["width"], small["height"]) == (640, 360), f"small is {small}")
    need(small.get("field_order", "progressive") == "progressive",
         f"the small rendition is {small.get('field_order')}, not progressive")
    need(small["sample_aspect_ratio"] == "1:1", f"small pixel shape {small['sample_aspect_ratio']}: "
         "a 16:9 picture at 640x360 has square pixels")
    q = psnr_stream(out, src, "0:p:2:v", scale="640:360")
    need(q >= 28, f"small rendition: {q:.1f} dB against the scaled source")
    return f"720x576 interlaced kept as fields; 640x360 rendition progressive, square pixels, {q:.0f} dB"


@test
def xc_video_damaged_input():
    """Lost and damaged video packets: no crash, a valid stream, and the picture recovers."""
    skip = xc_ready("libavcodec", "x264")
    if skip:
        return skip
    src = v_source("xc_v_h264", "h264")
    data = read(src)
    n = len(data) // TS
    rng = random.Random(21)
    worst = 99.0
    for trial in range(8):
        d = bytearray(data)
        for _ in range(rng.choice((3, 10, 30))):
            i = rng.randrange(n // 6, n * 6 // 10)       # all damage in the first 60%
            pid = (d[i * TS + 1] & 0x1F) << 8 | d[i * TS + 2]
            if pid != 0x100:
                continue
            if rng.random() < 0.5:                      # a run of packets lost
                for k in range(i, min(n, i + rng.randint(1, 20))):
                    if ((d[k * TS + 1] & 0x1F) << 8 | d[k * TS + 2]) == 0x100:
                        d[k * TS + 1] |= 0x1F
                        d[k * TS + 2] = 0xFF
            elif not (d[i * TS + 3] & 0x20):            # bits flipped in the payload
                for _ in range(rng.choice((1, 8, 40))):
                    d[i * TS + rng.randrange(4, TS)] ^= 1 << rng.randrange(8)
        bad = write(w("xc_v_bad_in.ts"), bytes(d))
        out = w("xc_v_bad.ts")
        st = run_file(bad, out, "--video", "h264:500k")
        rep, fails = check(out, 0, False, None)
        fails = [f for f in fails if "after their DTS" not in f]
        need(not fails, f"trial {trial}: " + "; ".join(fails))
        p = subprocess.run(["ffmpeg", "-v", "error", "-i", out, "-f", "null", "-"],
                           capture_output=True, text=True)
        need(p.returncode == 0 and not p.stderr.strip(),
             f"trial {trial}: our output does not decode cleanly: {p.stderr[:200]}")
        b = video_pts(out)
        need(len(b) > 150, f"trial {trial}: only {len(b)} pictures came out")
        need(b == sorted(b), f"trial {trial}: picture times out of order")
        # The last two seconds are past all the damage and at least one key
        # frame of the source: there it must look like the source again.
        tail = psnr_tail(out, src, video_pts(src)[-50])
        worst = min(worst, tail)
        need(tail > 25, f"trial {trial}: picture did not recover (last second {tail:.1f} dB)")
    return f"8 damaged inputs: output always valid and decodable; picture back to " \
           f"{worst:.0f} dB or better after the damage"


def psnr_tail(out, src, from_pts):
    p = subprocess.run(["ffmpeg", "-v", "info", "-copyts", "-i", out, "-i", src, "-lavfi",
                        f"[0:v]format=yuv420p,trim=start_pts={from_pts},setpts=PTS[a];"
                        f"[1:v]format=yuv420p,trim=start_pts={from_pts},setpts=PTS[b];[a][b]psnr",
                        "-an", "-f", "null", "-"], capture_output=True, text=True)
    for line in p.stderr.splitlines():
        if "PSNR" in line and "average:" in line:
            v = line.split("average:")[1].split()[0]
            return float("99" if v == "inf" else v)
    raise Fail("ffmpeg gave no PSNR figure: " + p.stderr[-300:])


@test
def live_xc_video():
    """Live: video and audio transcoded in real time, CBR out."""
    skip = xc_ready("libavcodec", "x264", "fdk-aac", "mpg123")
    if skip:
        return skip
    src = v_source("xc_v_h264", "h264")
    st, cap, log = live(lambda p: p.play(src, rate=2_000_000), "-r", "1200k",
                        "--video", "h264:500k", "--audio", "aac:96k",
                        capture_name="live_xc_video.ts", tail=4.5)
    out = w("live_xc_video.ts")
    must_pass(out, cbr=True)
    decodes_clean(out)
    a, b = video_pts(src), video_pts(out)
    need(set(b) <= set(a), "pictures at times the source had none")
    need(len(b) >= len(a) - 30, f"{len(b)} of {len(a)} pictures came out")
    q = psnr(out, src)
    need(q >= 27, f"PSNR {q:.1f} dB")
    ms, n = sync_ms(src, out, 0)
    need(abs(ms) < 1.0, f"audio is {ms:+.2f} ms off")
    xv, xa = xstats(st, 0x100), xstats(st, 0x101)
    for x in (xv, xa):
        need(not x["failed"] and x["decode_errors"] == 0 and x["input_dropped"] == 0, str(x))
    need(st["output"]["late_units"] == 0 and st["output"]["dropped_units"] == 0,
         f"late {st['output']['late_units']}, dropped {st['output']['dropped_units']}")
    need(xv.get("slack_min_ms", -1) > 0 and xa.get("slack_min_ms", -1) > 0,
         f"units reached the mux with no time to spare: video {xv.get('slack_min_ms')} ms, "
         f"audio {xa.get('slack_min_ms')} ms")
    no_pause(cap)
    return f"{len(b)}/{len(a)} pictures in place ({q:.0f} dB), audio {ms:+.3f} ms off, " \
           f"nothing late or dropped; of the default 3 s delay the video had " \
           f"{xv['slack_min_ms']:.0f} ms to spare at worst, the audio {xa['slack_min_ms']:.0f} ms"


@test
def live_xc_overload():
    """Live: an encoder setting far too slow for this machine. The program says so,
    throws away input it can never catch up on (counted), keeps the audio whole, and
    stops when told."""
    skip = xc_ready("libavcodec", "x264")
    if skip:
        return skip
    src = v_source("xc_v_1080_long", "h264", size="1920x1080", duration=14)
    st, cap, log = live(lambda p: p.play(src, rate=2_000_000), "--video", "h264:3M",
                        "--preset", "veryslow", capture_name="live_xc_overload.ts", tail=5.0)
    out = w("live_xc_overload.ts")
    x = xstats(st, 0x100)
    need("slower than the stream" in log, "falling behind was not reported:\n" + log[-600:])
    need(x["input_dropped"] > 0, f"no input counted as dropped: {x}")
    need(x["queued"] < 60, f"{x['queued']} packets still waiting for the encoder: the queue "
                           f"is not being cleared")
    o = Analysis(out)
    need(o.sync_errors == 0 and not o.cc_errors, "the output is damaged")
    must_match(src, out, only={0x101}, subset=True)
    need(len(o.pes[0x101]) >= len(Analysis(src).pes[0x101]) - 3, "audio is missing")
    n = len(o.pes.get(0x100, []))
    return f"reported, {x['input_dropped']} packets of video input skipped in " \
           f"{log.count('slower than the stream')} catch-ups, audio complete and identical, " \
           f"{n} pictures still made it out in time"


@test
def live_xc_delay_too_short():
    """Live: --delay set shorter than the pictures the codecs hold back. The video
    cannot be in time; the program says why, and does not throw input away for it."""
    skip = xc_ready("libavcodec", "x264")
    if skip:
        return skip
    src = v_source("xc_v_h264", "h264")
    # (an encoder setting that looks further ahead than the default, and a delay
    # that would do for the default)
    st, cap, log = live(lambda p: p.play(src, rate=2_000_000), "--video", "h264:500k",
                        "--preset", "medium", "--delay", "1600",
                        capture_name="live_xc_shortdelay.ts", tail=3.0)
    x = xstats(st, 0x100)
    need("--delay is too short" in log, "nothing said about the delay:\n" + log[-600:])
    need("slower than the stream" not in log and x["input_dropped"] == 0,
         f"input was thrown away though the codec keeps up: {x}")
    need(x["frames_out"] >= 120, f"the transcoder stopped working: {x}")
    must_match(src, w("live_xc_shortdelay.ts"), only={0x101}, subset=True)
    return f"reported ({x['slack_min_ms']:.0f} ms to spare), no input discarded, audio untouched"


@test
def live_xc_nvidia_standin():
    """Live against the NVIDIA stand-ins, stopped with SIGTERM while the card holds
    pictures back (B-frames): the tear-down brings them out first. (The real driver
    crashed on a Tesla P40 when it did not; file runs flush before closing and so
    never showed it.)"""
    skip = xc_ready("libavcodec", "x264")
    if skip:
        return skip
    if "nvenc" not in {l.split()[0] for l in subprocess.run(
            [BIN, "--backends"], capture_output=True, text=True).stdout.splitlines()}:
        return "SKIPPED: built without NVIDIA support"
    d = nvmock_dir()
    if not d:
        return "SKIPPED: could not build the stand-in libraries"
    src = v_source("xc_v_h264", "h264")
    env = dict(os.environ, LD_LIBRARY_PATH=d, NVMOCK_CHECK_LEAKS="1")
    # (stopped part-way through the source: at 4.5 s the card is holding one or
    # two pictures back; at some other points the group count comes out even and
    # nothing is in flight, so the point matters)
    st, cap, log = live(lambda p: p.play(src, rate=2_000_000, stop_after=4.5),
                        "--video", "h264:500k", capture_name="live_xc_nv.ts", tail=0.2, env=env)
    need("NVMOCK VIOLATION" not in log, "the NVIDIA code broke a rule of the API: " +
         log[log.find("NVMOCK VIOLATION"):][:300])
    x = xstats(st, 0x100)
    need((x["decoder"], x["encoder"]) == ("nvdec", "nvenc") and x["frames_out"] > 50, str(x))
    return f"{x['frames_out']} pictures through the stand-in card, stopped with pictures " \
           f"in flight, clean tear-down"


@test
def live_xc_renditions():
    """Live: three renditions in real time, each its own thread, one output."""
    skip = xc_ready("libavcodec", "x264", "fdk-aac", "mpg123")
    if skip:
        return skip
    src = v_source("xc_v_640", "h264", size="640x360", duration=8,
                   extra=("-output_ts_offset", "100"))
    st, cap, log = live(lambda p: p.play(src, rate=2_500_000), "-r", "3000k",
                        "--video", "h264:900k", "--video", "426x240:h264:400k",
                        "--video", "320x180:h264:200k", "--audio", "aac:96k",
                        capture_name="live_xc_renditions.ts", tail=4.5)
    out = w("live_xc_renditions.ts")
    must_pass(out, cbr=True)
    need(st["service"]["programs"] == 3, f"{st['service']['programs']} programs")
    xs = [x for x in st["service"]["streams"] if x["mode"] == "transcode"]
    need(len(xs) == 4, f"{len(xs)} transcoders")
    for x in xs:
        need(not x["failed"] and x["decode_errors"] == 0 and x["input_dropped"] == 0, str(x))
        need(x["frames_out"] >= 150, f"{x['pid']}: {x['frames_out']} out")
        need(x.get("slack_min_ms", -1) > 0, f"{x['pid']}: {x.get('slack_min_ms')} ms to spare")
    need(st["output"]["late_units"] == 0 and st["output"]["dropped_units"] == 0,
         f"late {st['output']['late_units']}, dropped {st['output']['dropped_units']}")
    keys = {n: key_frame_times(out, f"p:{n}:v")[1:] for n in (1, 2, 3)}
    need(keys[1] == keys[2] == keys[3], "key frames differ between renditions")
    no_pause(cap)
    return f"3 renditions + audio in real time, nothing late or dropped, key frames aligned " \
           f"({len(keys[1])} each), least slack {min(x['slack_min_ms'] for x in xs):.0f} ms"


@test
def live_xc_video_source_restart():
    """Source comes back on another clock while video and audio are being transcoded."""
    skip = xc_ready("libavcodec", "x264", "fdk-aac", "mpg123")
    if skip:
        return skip
    a = v_source("xc_v_ra", "h264", duration=5)
    b = v_source("xc_v_rb", "h264", duration=5, extra=("-output_ts_offset", "7000"))

    def feed(p):
        p.play(a, rate=2_000_000)
        time.sleep(1.5)
        p.play(b, rate=2_000_000)

    st, cap, _ = live(feed, "-r", "1200k", "--video", "h264:500k", "--audio", "aac:96k",
                      capture_name="live_xc_v_restart.ts", tail=4.5)
    out = w("live_xc_v_restart.ts")
    must_pass(out, cbr=True, allow_late=True)
    decodes_clean(out)
    need(st["clock"]["discontinuities"] == 1,
         f"{st['clock']['discontinuities']} discontinuities, expected 1")
    pts = video_pts(out)
    need(pts == sorted(pts) and len(set(pts)) == len(pts), "picture times not in order")
    need(len(pts) >= 2 * 125 - 40, f"only {len(pts)} of 250 pictures came out")
    an = Analysis(out)
    for pid in (0x100, 0x101):
        d = [r["dts"] for r in an.pes[pid] if r["dts"] is not None]
        need(all(y > x for x, y in zip(d, d[1:])), f"PID {pid}: decode times go backwards")
    late = an.lateness_ms()
    need(len(late) <= 5, f"{len(late)} PES left after their time")
    xv = xstats(st, 0x100)
    need(not xv["failed"] and xv["input_dropped"] == 0, str(xv))
    no_pause(cap)
    return f"{len(pts)} of 250 pictures across the restart, times only move forward, " \
           f"{len(late)} late"


# ---- NVIDIA, against stand-in libraries

def nvmock_dir():
    """
    Builds tests/nvmock.c into stand-ins for the three NVIDIA driver
    libraries and returns the directory to put on LD_LIBRARY_PATH, or None.
    """
    d = w("nvmock")
    lib = os.path.join(d, "libcuda.so.1")
    if os.path.exists(lib) and os.path.getmtime(lib) >= os.path.getmtime(os.path.join(HERE, "nvmock.c")):
        return d
    os.makedirs(d, exist_ok=True)
    loader = read(os.path.join(ROOT, "third_party", "ffnvcodec", "dynlink_loader.h")).decode()
    src = read(os.path.join(HERE, "nvmock.c")).decode()
    import re
    have = set(re.findall(r"^(?:CUresult|NVENCSTATUS NVENCAPI) (\w+)\(", src, re.M))
    stubs = ["/* Generated by the test suite: names the loader requires that the stand-in\n"
             " * does not implement. Calling one is a violation. */\n"]
    seen = set()
    for opt, sym in re.findall(r'LOAD_SYMBOL(_OPT)?\(\w+, \w+, "(\w+)"\)', loader):
        if not opt and sym not in have and sym not in seen:
            seen.add(sym)
            stubs.append(f'int {sym}(void);\nint {sym}(void) {{ VIOLATION("{sym} is not '
                         f'implemented by the stand-in"); }}\n')
    with open(os.path.join(d, "nvmock_stubs.h"), "w") as f:
        f.write("".join(stubs))
    p = subprocess.run(["cc", "-shared", "-fPIC", "-O1", "-g", f"-I{d}",
                        "-o", lib, os.path.join(HERE, "nvmock.c")], capture_output=True, text=True)
    if p.returncode:
        return None
    for name in ("libnvcuvid.so.1", "libnvidia-encode.so.1"):
        if not os.path.exists(os.path.join(d, name)):
            os.symlink("libcuda.so.1", os.path.join(d, name))  # one library, one state
    return d


def run_mock(src, dst, *args, env_extra=None, expect=0):
    binary = ASAN if os.path.exists(ASAN) else BIN
    env = dict(SAN_ENV, LD_LIBRARY_PATH=nvmock_dir(), NVMOCK_CHECK_LEAKS="1",
               **(env_extra or {}))
    sf = dst + ".json"
    if os.path.exists(sf):
        os.remove(sf)
    p = subprocess.run([binary, "-i", src, "-o", dst, "--stats-file", sf, *args],
                       capture_output=True, text=True, timeout=300, env=env)
    if "NVMOCK VIOLATION" in p.stderr:
        raise Fail("the NVIDIA code broke a rule of the API: " +
                   p.stderr[p.stderr.index("NVMOCK VIOLATION"):][:300])
    if "AddressSanitizer" in p.stderr or "runtime error" in p.stderr:
        raise Fail("sanitizer error:\n" + p.stderr[-2000:])
    need(p.returncode == expect, f"exit status {p.returncode}\n{p.stderr[-600:]}")
    return (json.load(open(sf)) if os.path.exists(sf) else None), p.stderr


@test
def xc_nvidia_standin():
    """The NVIDIA code against stand-in driver libraries that enforce the API's rules.

    Not a test of video: the stand-ins encode nothing. It shows that our
    calls come in the right order with the right structures, that no copy
    leaves the memory it was given, and that nothing is left behind; and
    that the choice between card and CPU, and the fall-back, work."""
    skip = xc_ready("libavcodec", "x264")
    if skip:
        return skip
    if "nvenc" not in {l.split()[0] for l in subprocess.run(
            [BIN, "--backends"], capture_output=True, text=True).stdout.splitlines()}:
        return "SKIPPED: built without NVIDIA support"
    d = nvmock_dir()
    if not d:
        return "SKIPPED: could not build the stand-in libraries"
    binary = ASAN if os.path.exists(ASAN) else BIN
    env = dict(SAN_ENV, LD_LIBRARY_PATH=d, NVMOCK_CHECK_LEAKS="1")

    # the self-test: memory -> card encode -> card decode -> card encode, three formats
    p = subprocess.run([binary, "--gpu-test"], capture_output=True, text=True, env=env,
                       timeout=300)
    need("NVMOCK VIOLATION" not in p.stderr, p.stderr[-400:])
    need("AddressSanitizer" not in p.stderr and "runtime error" not in p.stderr, p.stderr[-1500:])
    need(p.returncode == 0 and "all steps passed" in p.stdout, p.stdout[-600:] + p.stderr[-300:])

    src = v_source("xc_v_h264", "h264")
    an = Analysis(src)
    in_pts = {r["pts"] for r in an.pes[0x100]}
    res = []

    # a whole channel run, card for both by default
    out = w("xc_nv_auto.ts")
    st, log = run_mock(src, out, "--video", "hevc:500k")
    must_pass(out)
    x = xstats(st, 0x100)
    need((x["decoder"], x["encoder"]) == ("nvdec", "nvenc"), f"auto chose {x['decoder']} and "
                                                             f"{x['encoder']} with a card present")
    o = Analysis(out)
    types = {s["pid"]: s["type"] for s in o.pmt["streams"]}
    need(types[0x100] == 0x24, f"PMT type 0x{types[0x100]:02X} for H.265")
    v = o.pes[0x100]
    need(len(v) >= len(an.pes[0x100]) - 3, f"{len(v)} of {len(an.pes[0x100])} video PES")
    need({r["pts"] for r in v} <= in_pts, "video time stamps that the input did not have")
    need(st["output"]["late_units"] == 0 and st["output"]["dropped_units"] == 0, "late or dropped")
    must_match(src, out, only={0x101})
    res.append("card decode + card encode")

    # CPU decoder feeding the card's encoder (the upload path)
    out = w("xc_nv_cpudec.ts")
    st, log = run_mock(src, out, "--video", "h264:500k", "--decoder", "cpu")
    must_pass(out)
    x = xstats(st, 0x100)
    need((x["decoder"], x["encoder"]) == ("libavcodec", "nvenc"), str(x))
    need(x["frames_out"] >= 198, f"{x['frames_out']} pictures out")
    # H.264 on the card is coded with B-frames: pictures come out in decode
    # order, so most have a decode time before their presentation time, decode
    # times only move forward, and every picture is still at its original time
    o = Analysis(out)
    v = o.pes[0x100]
    d = [r["dts"] for r in v]
    need(all(y > x for x, y in zip(d, d[1:])), "decode times do not move forward with B-frames")
    need(all(r["dts"] <= r["pts"] for r in v), "a decode time after its presentation time")
    reordered = sum(1 for r in v if r["dts"] < r["pts"])
    need(reordered > len(v) // 2, f"only {reordered} of {len(v)} pictures show a B-frame delay: "
                                  f"the card was not asked for B-frames")
    need({r["pts"] for r in v} <= in_pts, "video time stamps that the input did not have")
    need(sorted(r["pts"] for r in v) != [r["pts"] for r in v], "pictures came out in display "
         "order: no reordering happened")
    res.append(f"CPU decode + card encode with B-frames ({reordered}/{len(v)} pictures reordered)")

    # 10-bit video that must come out as 8-bit H.264: the card converts
    src10 = v_source("xc_v_hevc10", "hevc", pix="yuv420p10le")
    out = w("xc_nv_10to8.ts")
    st, log = run_mock(src10, out, "--video", "h264:500k",
                       env_extra={"NVMOCK_FORMAT": "320x240:10"})
    must_pass(out)
    need(xstats(st, 0x100)["frames_out"] >= 195, "10-bit to 8-bit on the card produced nothing")
    res.append("10-bit in, 8-bit out")

    # the card has no encode session to give: carry on on the CPU, for real
    out = w("xc_nv_fallback.ts")
    st, log = run_mock(src, out, "--video", "h264:500k", env_extra={"NVMOCK_FAIL": "session"})
    must_pass(out)
    need("falling back to the CPU" in log, "no fall-back announced")
    x = xstats(st, 0x100)
    need((x["decoder"], x["encoder"]) == ("libavcodec", "x264"), str(x))
    decodes_clean(out, "after falling back: ")
    q = psnr(out, src)
    need(q >= 27 and len(video_pts(out)) >= 150, f"after falling back: {q:.1f} dB, "
                                                 f"{len(video_pts(out))} pictures")
    res.append(f"no session on the card -> CPU, {len(video_pts(out))}/200 pictures at {q:.0f} dB")

    # the same with the card insisted on: no silent substitute
    out = w("xc_nv_insist.ts")
    st, log = run_mock(src, out, "--video", "h264:500k", "--encoder", "nvidia",
                       "--decoder", "nvidia", env_extra={"NVMOCK_FAIL": "session"})
    need("stream is silent" in log, "card insisted on and unusable, but nothing said")
    need(xstats(st, 0x100)["failed"], "not reported as failed in the statistics")
    res.append("card insisted on and unusable: reported, not substituted")

    # the card has no decoder to give (or will not take the format): the CPU decodes,
    # from the very first unit, and the card still encodes
    out = w("xc_nv_nodec.ts")
    st, log = run_mock(src, out, "--video", "h264:500k", env_extra={"NVMOCK_FAIL": "decoder"})
    must_pass(out)
    need("decoding on the CPU" in log, "no change of decoder announced")
    x = xstats(st, 0x100)
    need((x["decoder"], x["encoder"]) == ("libavcodec", "nvenc") and not x["failed"], str(x))
    need(x["frames_out"] >= 198, f"card would not decode: only {x['frames_out']} of 200 "
                                 f"pictures came out after changing to the CPU")
    res.append(f"no decoder on the card -> CPU decode, card encode, {x['frames_out']}/200 pictures")

    # ... and with the card insisted on, that is said once and counted, not hidden
    out = w("xc_nv_nodec_insist.ts")
    st, log = run_mock(src, out, "--video", "h264:500k", "--decoder", "nvidia",
                       env_extra={"NVMOCK_FAIL": "decoder"})
    need(log.count("cannot decode this stream") == 1, "the decoder's failure was not "
         "reported exactly once:\n" + log[-500:])
    need(xstats(st, 0x100)["failed"], "decoder failure not in the statistics")

    # the card decodes at first and refuses the stream later (a change of format in
    # mid-run), with its encoder open on the decoder's context: the CPU decoder takes
    # over, the encoder is reopened, and nothing touches a context that is gone
    out = w("xc_nv_refuse_later.ts")
    st, log = run_mock(src, out, "--video", "h264:500k", env_extra={"NVMOCK_BAD_AFTER": "60"})
    must_pass(out)
    x = xstats(st, 0x100)
    need("decoding on the CPU" in log, "no change of decoder announced")
    need((x["decoder"], x["encoder"]) == ("libavcodec", "nvenc") and not x["failed"], str(x))
    need(x["frames_out"] >= 150, f"refused in mid-run: {x['frames_out']} of 200 pictures out")
    res.append(f"card refuses the stream in mid-run -> CPU decode, {x['frames_out']}/200 pictures")

    # three renditions on the card: the decoder scales to each size itself
    out = w("xc_nv_renditions.ts")
    st, log = run_mock(src, out, "--video", "h264:500k", "--video", "160x120:h264:150k",
                       "--video", "240x180:h264:250k")
    must_pass(out)
    need(st["service"]["programs"] == 3, f"{st['service']['programs']} programs on the card")
    sizes = {s_["pid"]: s_.get("size") for s_ in st["service"]["streams"] if s_["mode"] == "transcode"}
    need(sizes == {0x100: "320x240", 0x1001: "160x120", 0x1002: "240x180"}, str(sizes))
    for s_ in st["service"]["streams"]:
        if s_["mode"] == "transcode":
            need((s_["decoder"], s_["encoder"]) == ("nvdec", "nvenc") and s_["frames_out"] >= 195,
                 str(s_))
    res.append("three renditions, each scaled by the card's decoder")

    # interlaced video: the card's encoder is put in field mode ...
    out = w("xc_nv_fields.ts")
    st, log = run_mock(src, out, "--video", "h264:500k",
                       env_extra={"NVMOCK_FORMAT": "320x240:8:i"})
    must_pass(out)
    need("interlaced" in log and xstats(st, 0x100)["frames_out"] >= 195, log[-400:])
    # ... and if it then refuses field mode it still encodes, as frames, on the card
    out = w("xc_nv_fields_refused.ts")
    st, log = run_mock(src, out, "--video", "h264:500k",
                       env_extra={"NVMOCK_FORMAT": "320x240:8:i", "NVMOCK_FAIL": "field"})
    must_pass(out)
    x = xstats(st, 0x100)
    need("interlaced encoding was refused" in log, "refusal of field mode not reported")
    need(x["encoder"] == "nvenc" and x["frames_out"] >= 195, str(x))
    res.append("interlaced: field mode, and frames on the card when field mode is refused")
    return "; ".join(res)


def main():
    args = sys.argv[1:]
    if "--list" in args:
        for t in TESTS:
            print(f"{t.__name__:32s} {t.__doc__.strip().splitlines()[0]}")
        return 0
    if not os.path.exists(BIN):
        print("build first: make && make debug", file=sys.stderr)
        return 2
    os.makedirs(WORK, exist_ok=True)
    if not os.path.exists(ASAN):
        print("note: caricoder3-asan not built; file tests use the normal build")

    chosen = [t for t in TESTS if not args or any(a in t.__name__ for a in args)]
    failed = 0
    t_all = time.time()
    for t in chosen:
        t0 = time.time()
        try:
            info = t() or ""
            status = "SKIP" if info.startswith("SKIPPED") else "PASS"
        except Fail as e:
            info, status = str(e), "FAIL"
            failed += 1
        except Exception as e:                          # a bug in the test itself
            info, status = f"{type(e).__name__}: {e}", "ERROR"
            failed += 1
        print(f"{status:5s} {t.__name__:30s} {time.time() - t0:5.1f}s  {info}", flush=True)
    print(f"\n{len(chosen) - failed} of {len(chosen)} passed "
          f"in {time.time() - t_all:.0f} s")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

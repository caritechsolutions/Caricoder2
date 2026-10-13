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
    """PMT moves onto the old video PID and video onto the old PMT PID."""
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
    need(out.pmt_pid == 1651 and out.pmt["pcr_pid"] == 1650, "new layout not followed")
    need(len(out.pes.get(1650, [])) > 40, "video not carried on its new PID")
    return f"continuity unbroken on both PIDs ({rep['cc_errors']} CC errors)"


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


def live(feed, *args, tail=1.2, capture_name="live.ts", with_proc=False):
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
        stderr=subprocess.PIPE, text=True)
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
        p.play(a, stop_after=3.0)
        p.play(tail_b, rate=4_000_000, stop_after=0.2)

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


def sync_ms(src, out, idx, src_idx=None, check_likeness=True):
    import av_sync
    ms, _, n_out = av_sync.offset_ms(src, idx if src_idx is None else src_idx, out, idx)
    # In place is not enough: it must also be the same audio (not at the wrong
    # speed, not garbled). Lossy coding of the test signal leaves this near 0.8-0.9.
    need(av_sync.last_likeness > 0.5 or not check_likeness,
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
                ms, _ = sync_ms(src, out, 0)
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
    p = subprocess.run(["ffmpeg", "-v", "info", "-copyts", "-i", out, "-i", src, "-lavfi",
                        "[0:v]format=yuv420p[a];[1:v]format=yuv420p[b];"
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


def v_source(name, codec, extra=(), size="320x240", duration=8, pix="yuv420p"):
    args = {"h264": ["-c:v", "libx264", "-preset", "veryfast", "-b:v", "900k", "-g", "25",
                     "-bf", "2"],
            "hevc": ["-c:v", "libx265", "-preset", "ultrafast", "-b:v", "900k",
                     "-x265-params", "log-level=error:keyint=25"]}[codec]
    path = w(name + ".ts")
    if name in _cache and os.path.exists(path):
        return path
    p = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                        "-f", "lavfi", "-i", f"testsrc2=size={size}:rate=25:duration={duration}",
                        "-f", "lavfi", "-i", f"{BURSTS}:d={duration}",
                        "-pix_fmt", pix, *args, *extra, "-c:a", "mp2", "-b:a", "128k",
                        "-f", "mpegts", "-muxrate", "2000k", path],
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
    p = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                        "frame=key_frame", "-of", "csv=p=0", out], capture_output=True, text=True)
    keys = [i for i, x in enumerate(p.stdout.split()) if x.strip(",") == "1"]
    gaps = [b - a for a, b in zip(keys, keys[1:])]
    need(gaps and max(gaps) <= 12, f"--gop 500: key frames {max(gaps, default=0)} pictures apart")
    res.append(f"--vbv 300: at most {max(lead):.0f} ms early and {peak / 1000:.0f} kB held; "
               f"--gop 500 a key frame every {max(gaps)} pictures at most")
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
    st, cap, log = live(lambda p: p.play(src, rate=2_000_000), "--video", "h264:500k",
                        "--delay", "600", capture_name="live_xc_shortdelay.ts", tail=2.0)
    x = xstats(st, 0x100)
    need("--delay is too short" in log, "nothing said about the delay:\n" + log[-600:])
    need("slower than the stream" not in log and x["input_dropped"] == 0,
         f"input was thrown away though the codec keeps up: {x}")
    need(x["frames_out"] >= 150, f"the transcoder stopped working: {x}")
    must_match(src, w("live_xc_shortdelay.ts"), only={0x101}, subset=True)
    return f"reported ({x['slack_min_ms']:.0f} ms to spare), no input discarded, audio untouched"


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
    res.append("CPU decode + card encode")

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

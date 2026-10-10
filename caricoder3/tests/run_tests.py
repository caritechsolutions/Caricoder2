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
                        *args], capture_output=True, text=True, timeout=300)
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
          separate_pcr=False, seed=1, programs=None):
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
           "pcr_base": int(pcr_base * HZ), "programs": programs}
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
    st, cap, _ = live(lambda p: p.play(src), "-r", "4M", capture_name="live_cbr.ts")
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
    need(pace["p99"] < 5.0, f"pacing p99 {pace['p99']:.2f} ms")
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
    need(cap.max_gap_ms() < 100, f"output paused for {cap.max_gap_ms():.0f} ms")
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
    need(cap.max_gap_ms() < 100, f"output paused for {cap.max_gap_ms():.0f} ms")
    n_b = len(Analysis(b).pes[1651])
    out_v = len(Analysis(w("live_restart.ts")).pes[1651])
    need(out_v > n_b + 50, "second source not carried")
    return f"1 discontinuity absorbed, output gap max {cap.max_gap_ms():.1f} ms, " \
           f"PCR continuous (max interval {rep['pcr_interval_max_ms']} ms)"


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
    need(cap.max_gap_ms() < 100, f"output paused for {cap.max_gap_ms():.0f} ms")
    orig = sum(len(v) for v in Analysis(src).pes.values())
    got = sum(len(v) for v in Analysis(w("live_burst.ts")).pes.values())
    need(got >= orig - 3, f"only {got} of {orig} PES delivered")
    need(st["input"]["pcr_missing"] == 0,
         "the wall clock was used to time data that was merely late")
    return f"no time-base step, {got}/{orig} PES delivered, PCR accuracy " \
           f"{rep['pcr_accuracy_ns']} ns"


@test
def live_process_stall():
    """The process itself is frozen for 1.5 s (as in a VM pause)."""
    src = synth("live_src", duration=10.0)
    marks = {}

    def feed(p, proc):
        def freeze():
            proc.send_signal(signal.SIGSTOP)
            marks["stop"] = time.perf_counter()
            time.sleep(1.5)
            marks["cont"] = time.perf_counter()
            proc.send_signal(signal.SIGCONT)
        p.play(src, at={4.0: freeze})

    st, cap, _ = live(feed, "-r", "4M", capture_name="live_stall.ts", with_proc=True)
    # The freeze is a real hole in the output; everything around it must be clean.
    must_pass(w("live_stall.ts"), allow_late=True,
              limits={"pcr_interval_ms": 2500, "pat_interval_ms": 2500,
                      "pmt_interval_ms": 2500, "sdt_interval_ms": 4000})
    must_match(src, w("live_stall.ts"), subset=True)
    need(st["output"]["clock_skips"] >= 1, "stall not noticed")
    burst = sum(len(c) for c, ts in zip(cap.chunks, cap.times)
                if marks["cont"] <= ts < marks["cont"] + 0.2)
    burst_s = burst * 8 / 4e6
    need(burst_s < 0.9, f"{burst_s:.2f} s of stream sent in the 0.2 s after resuming")
    out = Analysis(w("live_stall.ts"))
    need(out.pcr_disc >= 1, "PCR step not flagged in the stream")
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

def main():
    args = sys.argv[1:]
    if "--list" in args:
        for t in TESTS:
            print(f"{t.__name__:32s} {t.__doc__.strip()}")
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

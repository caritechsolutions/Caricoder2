#!/usr/bin/env python3
"""
Checks a transport stream for conformance, and compares the content of two.

    ts_check.py FILE [--program N] [--cbr]
    ts_check.py FILE --against ORIGINAL [--program N]

Exit status 0 means every check passed.
"""
import argparse
import json
import sys

from tslib import Analysis, mod_diff, PTS_MOD

LIMITS = {
    "pcr_interval_ms": 40.0,        # DVB recommendation
    "pcr_accuracy_ns": 500.0,       # ISO 13818-1
    "pat_interval_ms": 500.0,
    "pmt_interval_ms": 500.0,
    "sdt_interval_ms": 2000.0,
}


def check(path, program=0, cbr=False, limits=None):
    """Returns (report dict, list of failure strings)."""
    lim = dict(LIMITS, **(limits or {}))
    a = Analysis(path, program)
    fails = []
    rep = {"file": path, "packets": a.packets, "program": a.program,
           "sync_errors": a.sync_errors, "cc_errors": a.total_cc_errors(),
           "psi_crc_errors": a.psi_crc_errors, "pcr_count": len(a.pcrs)}

    if a.packets == 0:
        return rep, ["empty file"]
    if a.sync_errors:
        fails.append(f"{a.sync_errors} sync errors")
    if a.total_cc_errors():
        fails.append(f"continuity errors: {a.cc_errors}")
    if a.psi_crc_errors:
        fails.append(f"{a.psi_crc_errors} PSI CRC errors")
    if not a.pat or not a.pmt:
        fails.append("PAT or PMT missing")
        return rep, fails

    rep["streams"] = [{"pid": s["pid"], "type": s["type"]} for s in a.pmt["streams"]]
    rep["pes"] = {pid: len(v) for pid, v in a.pes.items()}
    rep["sections"] = {pid: len(v) for pid, v in a.sections.items()}
    bad_pes = sum(1 for v in a.pes.values() for r in v if not r["ok"])
    if bad_pes:
        fails.append(f"{bad_pes} malformed PES packets")

    if len(a.pcrs) < 3:
        fails.append("fewer than 3 PCRs")
        return rep, fails
    if not a.pcr_monotonic():
        fails.append("PCR is not monotonic")

    bitrate, acc_ns, gap_ms = a.pcr_accuracy()
    rep["pcr_interval_max_ms"] = round(gap_ms, 3)
    if gap_ms > lim["pcr_interval_ms"]:
        fails.append(f"PCR interval {gap_ms:.1f} ms > {lim['pcr_interval_ms']} ms")
    if cbr:
        rep["bitrate"] = round(bitrate)
        rep["pcr_accuracy_ns"] = round(acc_ns, 1)
        if acc_ns > lim["pcr_accuracy_ns"]:
            fails.append(f"PCR accuracy {acc_ns:.0f} ns > {lim['pcr_accuracy_ns']} ns")

    for name, idx in (("pat", a.pat_idx), ("pmt", a.pmt_idx), ("sdt", a.sdt_idx)):
        iv = a.max_interval_ms(idx)
        if iv is None:
            if name != "sdt":
                fails.append(f"{name.upper()} seen fewer than twice")
            continue
        rep[f"{name}_interval_max_ms"] = round(iv, 1)
        if iv > lim[f"{name}_interval_ms"]:
            fails.append(f"{name.upper()} interval {iv:.0f} ms")

    late = a.late_pes()
    rep["late_pes"] = late
    if late:
        fails.append(f"{late} PES packets complete after their DTS")
    return rep, fails


def _is_subsequence(small, big):
    it = iter(big)
    return all(any(x == y for y in it) for x in small)


def compare(orig_path, out_path, program=0, subset=False, shifted=False,
            skip_head=0, only=None):
    """
    Checks that `out` carries the same service content as `orig`.

    only     : a set of PIDs. The comparison is limited to those streams
               (for runs in which the others are transcoded).

    subset   : out may be missing whole units (after input loss) but must
               not contain anything that was not in orig, nor reorder.
    shifted  : time stamps may differ (time line re-based); everything
               else in each PES packet must still be identical.
    Returns (report, failures).
    """
    a = Analysis(orig_path, program)
    b = Analysis(out_path, a.program)
    fails = []
    rep = {"program": a.program, "pes": {}, "sections": {}}

    if not a.pmt or not b.pmt:
        return rep, ["PMT missing in one of the files"]
    if a.pmt["pcr_pid"] != b.pmt["pcr_pid"]:
        fails.append("PCR PID differs")
    if a.pmt["info"] != b.pmt["info"]:
        fails.append("program descriptors differ")
    if [s["pid"] for s in a.pmt["streams"]] != [s["pid"] for s in b.pmt["streams"]]:
        fails.append("PMT lists different PIDs")
    if [(s["pid"], s["type"], s["desc"]) for s in a.pmt["streams"]
        if only is None or s["pid"] in only] != \
       [(s["pid"], s["type"], s["desc"]) for s in b.pmt["streams"]
        if only is None or s["pid"] in only]:
        fails.append("PMT stream list differs")

    key = "sha_nots" if shifted else "sha"
    for s in a.pmt["streams"]:
        pid = s["pid"]
        if only is not None and pid not in only:
            continue
        if pid in a.pes or pid in b.pes:
            ia = [(r[key], r["rai"]) + (() if shifted else (r["pts"], r["dts"]))
                  for r in a.pes.get(pid, [])][skip_head:]
            ib = [(r[key], r["rai"]) + (() if shifted else (r["pts"], r["dts"]))
                  for r in b.pes.get(pid, [])]
            rep["pes"][pid] = {"orig": len(ia), "out": len(ib)}
            if subset:
                if not _is_subsequence(ib, ia):
                    fails.append(f"PID 0x{pid:04X}: output has PES packets "
                                 f"that are not in the original, or out of order")
            elif ia != ib:
                n = next((i for i, (x, y) in enumerate(zip(ia, ib)) if x != y),
                         min(len(ia), len(ib)))
                fails.append(f"PID 0x{pid:04X}: PES differ from #{n} "
                             f"(orig {len(ia)}, out {len(ib)})")
            if shifted and not subset:
                d = {mod_diff(y["pts"], x["pts"], PTS_MOD)
                     for x, y in zip(a.pes.get(pid, []), b.pes.get(pid, []))
                     if x["pts"] is not None and y["pts"] is not None}
                rep["pes"][pid]["pts_shifts"] = sorted(d)
        if pid in a.sections or pid in b.sections:
            sa, sb = a.sections.get(pid, []), b.sections.get(pid, [])
            rep["sections"][pid] = {"orig": len(sa), "out": len(sb)}
            if subset:
                if not _is_subsequence(sb, sa):
                    fails.append(f"PID 0x{pid:04X}: sections not a subset")
            elif sa != sb:
                fails.append(f"PID 0x{pid:04X}: sections differ "
                             f"(orig {len(sa)}, out {len(sb)})")
    return rep, fails


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--program", type=int, default=0)
    ap.add_argument("--cbr", action="store_true",
                    help="also check PCR accuracy against byte position")
    ap.add_argument("--against", metavar="ORIGINAL",
                    help="compare the service content with this file")
    ap.add_argument("--subset", action="store_true")
    ap.add_argument("--shifted", action="store_true")
    a = ap.parse_args()

    rep, fails = check(a.file, a.program, a.cbr)
    if a.against:
        crep, cfails = compare(a.against, a.file, a.program, a.subset, a.shifted)
        rep["compare"] = crep
        fails += cfails
    rep["result"] = "FAIL" if fails else "PASS"
    rep["failures"] = fails
    print(json.dumps(rep, indent=2))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()

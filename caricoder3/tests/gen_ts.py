#!/usr/bin/env python3
"""
Synthetic transport stream generator for the caricoder3 tests.

Builds a constant bit rate single- or multi-program stream whose elementary
stream payloads are pseudo-random bytes. Nothing in it can be decoded to
pictures or sound, and it does not need to be: the tests are about the
transport layer, and random payloads make any byte that goes missing or
moves show up in a hash.

It can be imported (build()) or run from the command line.
"""
import argparse
import random
import struct
import sys

from tslib import crc32_mpeg, PCR_MOD, PTS_MOD

TS = 188
HZ = 27_000_000


def section(table_id, body_before_crc, syntax=True):
    """body = everything after section_length and before the CRC."""
    length = len(body_before_crc) + 4
    b1 = (0xB0 if syntax else 0x30) | (length >> 8)
    sec = bytes([table_id, b1, length & 0xFF]) + body_before_crc
    return sec + struct.pack(">I", crc32_mpeg(sec))


def build_pat(ts_id, programs, version=0):
    body = struct.pack(">HBBB", ts_id, 0xC1 | (version << 1), 0, 0)
    for p in programs:
        body += struct.pack(">HH", p["number"], 0xE000 | p["pmt_pid"])
    return section(0x00, body)


def build_pmt(p, version=0):
    info = p.get("info", b"")
    body = struct.pack(">HBBBHH", p["number"], 0xC1 | (version << 1), 0, 0,
                       0xE000 | p["pcr_pid"], 0xF000 | len(info)) + info
    for s in p["streams"]:
        desc = s.get("desc", b"")
        body += struct.pack(">BHH", s["type"], 0xE000 | s["pid"],
                            0xF000 | len(desc)) + desc
    return section(0x02, body)


def build_sdt(ts_id, programs, version=0, onid=0xFF01):
    body = struct.pack(">HBBBHB", ts_id, 0xC1 | (version << 1), 0, 0, onid, 0xFF)
    for p in programs:
        prov = p.get("provider", "Prov").encode("latin-1")
        name = p.get("name", "Service").encode("latin-1")
        d = bytes([0x01, len(prov)]) + prov + bytes([len(name)]) + name
        desc = bytes([0x48, len(d)]) + d + p.get("sdt_extra", b"")
        body += struct.pack(">HBH", p["number"], 0xFC, 0x8000 | len(desc)) + desc
    return section(0x42, body)


def pts_field(prefix, v):
    v %= PTS_MOD
    return bytes([(prefix << 4) | ((v >> 29) & 0x0E) | 1, (v >> 22) & 0xFF,
                  ((v >> 14) & 0xFE) | 1, (v >> 7) & 0xFF, ((v << 1) & 0xFE) | 1])


def pes(stream_id, payload, pts, dts=None, bounded=True):
    if dts is not None and dts != pts:
        hdr = pts_field(3, pts) + pts_field(1, dts)
        flags = 0xC0
    else:
        hdr = pts_field(2, pts)
        flags = 0x80
    body = bytes([0x84, flags, len(hdr)]) + hdr + payload   # data_alignment set
    length = len(body) if bounded and len(body) <= 0xFFFF else 0
    return b"\x00\x00\x01" + bytes([stream_id]) + struct.pack(">H", length) + body


def pcr_bytes(v):
    v %= PCR_MOD
    base, ext = divmod(v, 300)
    return bytes([(base >> 25) & 0xFF, (base >> 17) & 0xFF, (base >> 9) & 0xFF,
                  (base >> 1) & 0xFF, ((base & 1) << 7) | 0x7E | (ext >> 8),
                  ext & 0xFF])


def ts_packet(pid, cc, pusi, chunk, pcr=None, rai=False, pad_payload=False):
    """chunk: the bytes available; returns (packet, bytes consumed)."""
    need_af = pcr is not None or rai
    af_body = (1 + (6 if pcr is not None else 0)) if need_af else 0
    room = TS - 4 - ((1 + af_body) if need_af else 0)
    n = min(room, len(chunk))
    stuff = room - n
    head = bytes([0x47, (0x40 if pusi else 0) | (pid >> 8), pid & 0xFF])
    if pad_payload:                     # sections: 0xFF after the data
        assert not need_af
        return head + bytes([0x10 | cc]) + chunk[:n] + b"\xFF" * stuff, n
    if not need_af and stuff == 0:
        return head + bytes([0x10 | cc]) + chunk[:n], n
    af_len = af_body + stuff if need_af else stuff - 1
    af = bytes([af_len])
    if af_len > 0:
        af += bytes([(0x40 if rai else 0) | (0x10 if pcr is not None else 0)])
        if pcr is not None:
            af += pcr_bytes(pcr)
        af += b"\xFF" * (af_len + 1 - len(af))
    return head + bytes([0x30 | cc]) + af + chunk[:n], n


def pcr_only(pid, cc, pcr):
    return bytes([0x47, pid >> 8, pid & 0xFF, 0x20 | cc, 183, 0x10]) + \
        pcr_bytes(pcr) + b"\xFF" * 176


NULL = bytes([0x47, 0x1F, 0xFF, 0x10]) + b"\xFF" * 184


def make_units(s, clock0, duration, rng):
    """List of (start, end, pid, bytes, rai, is_section) for one stream."""
    out = []
    kind = s["kind"]
    lead = int(0.7 * HZ)                # first DTS this long after the start
    if kind == "video":
        fps = s.get("fps", 25)
        T = HZ // fps
        avg = s["bitrate"] / fps / 8
        gop = s.get("gop", fps)
        bf = s.get("bframes", 2)
        send_rate = s["bitrate"] * 1.08
        vbv = int(s.get("vbv", 0.45) * HZ)
        prev_end = clock0
        step = bf + 1
        i_every = max(1, gop // step)   # every n-th anchor frame is an I
        n = int(duration * fps)
        # Decode order: each anchor (I/P) goes before the B frames that
        # are displayed ahead of it.
        order = [(0, "I")]
        k = 1
        while len(order) < n:
            anchor = k * step
            order.append((anchor, "I" if k % i_every == 0 else "P"))
            order.extend((b, "B") for b in range(anchor - bf, anchor))
            k += 1
        for dec, (d, ftype) in enumerate(order[:n]):
            scale = {"I": s.get("i_scale", 5.0), "P": 1.3, "B": 0.45}[ftype]
            size = max(200, int(avg * scale * rng.uniform(0.8, 1.2) * 0.8))
            dts = clock0 + lead + dec * T
            pts = clock0 + lead + (d + 1) * T
            payload = rng.randbytes(size)
            data = pes(0xE0, payload, pts // 300, dts // 300, bounded=False)
            start = max(prev_end, dts - vbv)
            end = start + int(len(data) * 8 * HZ / send_rate)
            prev_end = end
            if s.get("burst"):          # whole frame at once, like a VBR muxer
                end = start + 1
            out.append((start, end, s["pid"], data, ftype == "I", False))
    elif kind == "audio":
        frame = int(s.get("frame_ms", 24.0) * HZ / 1000)
        size = int(s["bitrate"] * s.get("frame_ms", 24.0) / 8000)
        n = int(duration * HZ / frame)
        for i in range(n):
            pts = clock0 + lead + i * frame
            data = pes(s.get("stream_id", 0xC0), rng.randbytes(size), pts // 300)
            start = pts - int(0.12 * HZ)
            out.append((start, start + frame // 2, s["pid"], data, False, False))
    elif kind == "sections":
        step = int(s.get("interval_ms", 80) * HZ / 1000)
        n = int(duration * HZ / step)
        for i in range(n):
            body = struct.pack(">I", i) + rng.randbytes(s.get("size", 16))
            sec = section(s.get("table_id", 0xBF), body, syntax=False)
            start = clock0 + int(0.1 * HZ) + i * step
            out.append((start, start + 1, s["pid"], sec, False, True))
    return out


def build(cfg):
    rng = random.Random(cfg.get("seed", 1))
    muxrate = cfg["muxrate"]
    duration = cfg["duration"]
    programs = cfg["programs"]
    ts_id = cfg.get("ts_id", 1)
    slot = TS * 8 * HZ / muxrate
    n_slots = int(duration * muxrate / (TS * 8))
    pcr_iv = int(cfg.get("pcr_ms", 35) * HZ / 1000)
    psi_iv = int(cfg.get("psi_ms", 100) * HZ / 1000)
    sdt_iv = int(cfg.get("sdt_ms", 1000) * HZ / 1000)

    # Each program runs on its own clock so that mixing them up shows.
    queues = {}                         # pid -> list of [start, end, data, rai, is_sec]
    for k, p in enumerate(programs):
        p["clock0"] = cfg.get("pcr_base", 0) + k * 1000 * HZ
        for s in p["streams"]:
            units = make_units(s, p["clock0"], duration, rng)
            queues[s["pid"]] = {"units": units, "u": 0, "off": 0, "prog": k,
                                "sec": s["kind"] == "sections"}

    pat = build_pat(ts_id, programs)
    sdt_progs = [p for p in programs
                 if p["number"] in cfg.get("sdt_services", [q["number"] for q in programs])]
    sdt = build_sdt(ts_id, sdt_progs, version=cfg.get("sdt_version", 0)) \
        if cfg.get("sdt", True) else None
    pmts = [build_pmt(p) for p in programs]
    cc = {}
    out = bytearray()
    psi_q = []                          # packets waiting
    next_psi = next_sdt = 0
    next_pcr = [0] * len(programs)

    def cc_next(pid):
        v = cc.get(pid, 0)
        cc[pid] = (v + 1) & 15
        return v

    def psi_packets(pid, sec):
        data = b"\x00" + sec
        first = True
        while data:
            # The continuity counter is filled in when the packet is sent.
            pkt, n = ts_packet(pid, 0, first, data, pad_payload=True)
            psi_q.append((pid, pkt))
            data = data[n:]
            first = False

    draining = False

    def stream_time(pid):
        q = queues[pid]
        if q["u"] >= len(q["units"]):
            return None
        if draining and q["off"] == 0:
            return None                 # finish what is started, begin nothing
        start, end, _, data, _, _ = q["units"][q["u"]]
        total = max(1, len(data))
        return start + (end - start) * q["off"] // total

    def send(pid, pcr=None):
        q = queues[pid]
        start, end, _, data, rai, is_sec = q["units"][q["u"]]
        first = q["off"] == 0
        if is_sec:
            chunk = (b"\x00" + data)[q["off"]:]
            pkt, n = ts_packet(pid, cc_next(pid), first, chunk, pad_payload=True)
            total = len(data) + 1
        else:
            pkt, n = ts_packet(pid, cc_next(pid), first, data[q["off"]:],
                               pcr=pcr, rai=rai and first)
            total = len(data)
        q["off"] += n
        if q["off"] >= total:
            q["u"] += 1
            q["off"] = 0
        return pkt

    i = -1
    while True:
        i += 1
        if i >= n_slots:
            # Do not end the file in the middle of a PES packet or table.
            draining = True
            if not psi_q and all(q["off"] == 0 for q in queues.values()):
                break
        t = int(i * slot)               # elapsed, same for every program
        if t >= next_psi and not draining:
            psi_packets(0, pat)
            for p, sec in zip(programs, pmts):
                psi_packets(p["pmt_pid"], sec)
            next_psi += psi_iv
        if sdt and t >= next_sdt and not draining:
            psi_packets(0x11, sdt)
            next_sdt += sdt_iv

        emitted = None
        for k, p in enumerate(programs):
            if t >= next_pcr[k]:
                now = p["clock0"] + t
                pid = p["pcr_pid"]
                next_pcr[k] = t + pcr_iv
                if pid in queues and not queues[pid]["sec"]:
                    st = stream_time(pid)
                    if st is not None and st <= now:
                        emitted = send(pid, pcr=now)
                        break
                    emitted = pcr_only(pid, (cc.get(pid, 0) - 1) & 15, now)
                else:
                    emitted = pcr_only(pid, (cc.get(pid, 0) - 1) & 15, now)
                break
        if emitted is None and psi_q:
            pid, pkt = psi_q.pop(0)
            emitted = pkt[:3] + bytes([0x10 | cc_next(pid)]) + pkt[4:]
        if emitted is None:
            best = None
            for pid, q in queues.items():
                st = stream_time(pid)
                if st is None:
                    continue
                now = programs[q["prog"]]["clock0"] + t
                if st <= now and (best is None or st - now < best[0]):
                    best = (st - now, pid)
            if best:
                emitted = send(best[1])
        out += emitted if emitted is not None else NULL
    return bytes(out)


def default_program(number=1650, base_pid=None, video_rate=2_500_000,
                    sections=True, separate_pcr=False, name="Vacation"):
    base = base_pid if base_pid is not None else number
    streams = [
        {"pid": base + 1, "type": 0x1B, "kind": "video", "bitrate": video_rate,
         "fps": 25, "gop": 25, "bframes": 2},
        {"pid": base + 2, "type": 0x0F, "kind": "audio", "bitrate": 128_000,
         "desc": bytes([0x0A, 4]) + b"eng\x00"},
        {"pid": base + 3, "type": 0x81, "kind": "audio", "bitrate": 192_000,
         "frame_ms": 32.0, "stream_id": 0xBD,
         "desc": bytes([0x05, 4]) + b"AC-3"},
    ]
    if sections:
        # Private sections declared in the PMT, like the RIST Part 7 markers.
        streams.append({"pid": 0x1FF0 if number == 1650 else base + 9,
                        "type": 0x05, "kind": "sections", "interval_ms": 60,
                        "size": 16, "table_id": 0xBF})
    return {"number": number, "pmt_pid": base,
            "pcr_pid": base + 8 if separate_pcr else base + 1,
            "name": name, "provider": "Coop", "streams": streams,
            "info": bytes([0x05, 4]) + b"TEST" if number == 1650 else b""}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("output")
    ap.add_argument("--duration", type=float, default=10.0)
    ap.add_argument("--muxrate", type=int, default=4_000_000)
    ap.add_argument("--pcr-base", type=float, default=10.0,
                    help="start of the clock in seconds; 'wrap' tests use a "
                         "value just under 95443.7 (2^33 / 90000)")
    ap.add_argument("--mpts", action="store_true", help="add a second service")
    ap.add_argument("--separate-pcr", action="store_true")
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()

    programs = [default_program(separate_pcr=a.separate_pcr)]
    if a.mpts:
        programs.insert(0, default_program(2000, video_rate=1_500_000,
                                           sections=False, name="Other"))
    cfg = {"muxrate": a.muxrate, "duration": a.duration, "seed": a.seed,
           "pcr_base": int(a.pcr_base * HZ), "programs": programs}
    data = build(cfg)
    with open(a.output, "wb") as f:
        f.write(data)
    print(f"{a.output}: {len(data) // TS} packets, {len(data)} bytes", file=sys.stderr)


if __name__ == "__main__":
    main()

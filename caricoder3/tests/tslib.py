"""
Independent MPEG-TS reader used by the caricoder3 tests.

Nothing in here shares code with the C program: it is a second, separate
implementation of the same standard, so that when the two agree about a
stream the agreement means something.
"""
import hashlib
import struct

TS = 188
PCR_MOD = (1 << 33) * 300
PTS_MOD = 1 << 33

_CRC_TABLE = []
for _i in range(256):
    _c = _i << 24
    for _ in range(8):
        _c = ((_c << 1) ^ 0x04C11DB7) if (_c & 0x80000000) else (_c << 1)
    _CRC_TABLE.append(_c & 0xFFFFFFFF)


def crc32_mpeg(data):
    c = 0xFFFFFFFF
    for b in data:
        c = ((c << 8) & 0xFFFFFFFF) ^ _CRC_TABLE[((c >> 24) ^ b) & 0xFF]
    return c


def mod_diff(a, b, mod):
    d = (a - b) % mod
    return d - mod if d > mod // 2 else d


class Packet:
    __slots__ = ("idx", "raw", "pid", "pusi", "tei", "tsc", "afc", "cc",
                 "payload", "pcr", "disc", "rai")

    def __init__(self, idx, raw):
        self.idx = idx
        self.raw = raw
        self.tei = raw[1] >> 7
        self.pusi = (raw[1] >> 6) & 1
        self.pid = ((raw[1] & 0x1F) << 8) | raw[2]
        self.tsc = raw[3] >> 6
        self.afc = (raw[3] >> 4) & 3
        self.cc = raw[3] & 0x0F
        self.pcr = None
        self.disc = False
        self.rai = False
        off = 4
        if self.afc & 2:
            afl = raw[4]
            off = 5 + afl
            if afl >= 1:
                flags = raw[5]
                self.disc = bool(flags & 0x80)
                self.rai = bool(flags & 0x40)
                if (flags & 0x10) and afl >= 7:
                    base = (raw[6] << 25) | (raw[7] << 17) | (raw[8] << 9) | \
                           (raw[9] << 1) | (raw[10] >> 7)
                    ext = ((raw[10] & 1) << 8) | raw[11]
                    self.pcr = base * 300 + ext
        self.payload = raw[off:] if (self.afc & 1) and off <= TS else b""


def read_packets(path):
    """Yields Packet objects. The file must be packet aligned."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) % TS:
        data = data[:len(data) - len(data) % TS]
    for i in range(0, len(data), TS):
        yield Packet(i // TS, data[i:i + TS])


def parse_pts(b):
    return ((b[0] & 0x0E) << 29) | (b[1] << 22) | ((b[2] & 0xFE) << 14) | \
           (b[3] << 7) | (b[4] >> 1)


class SectionAssembler:
    def __init__(self):
        self.buf = None

    def push(self, pkt):
        """Returns the list of complete sections finished by this packet."""
        out = []
        p = pkt.payload
        if not p:
            return out
        if pkt.pusi:
            ptr = p[0]
            tail, p = p[1:1 + ptr], p[1 + ptr:]
            if self.buf is not None:
                self.buf += tail
                self._take(out)
            self.buf = None
            while p and p[0] != 0xFF:
                if len(p) < 3:
                    self.buf = bytearray(p)
                    p = b""
                    break
                need = 3 + (((p[1] & 0x0F) << 8) | p[2])
                if len(p) >= need:
                    out.append(bytes(p[:need]))
                    p = p[need:]
                else:
                    self.buf = bytearray(p)
                    p = b""
        elif self.buf is not None:
            self.buf += p
            self._take(out)
        return out

    def _take(self, out):
        if len(self.buf) >= 3:
            need = 3 + (((self.buf[1] & 0x0F) << 8) | self.buf[2])
            if len(self.buf) >= need:
                out.append(bytes(self.buf[:need]))
                self.buf = None


def parse_pat(sec):
    progs = {}
    for pos in range(8, len(sec) - 4, 4):
        num = (sec[pos] << 8) | sec[pos + 1]
        pid = ((sec[pos + 2] & 0x1F) << 8) | sec[pos + 3]
        if num:
            progs[num] = pid
    return {"ts_id": (sec[3] << 8) | sec[4], "version": (sec[5] >> 1) & 31,
            "programs": progs}


def parse_pmt(sec):
    info_len = ((sec[10] & 0x0F) << 8) | sec[11]
    pos = 12 + info_len
    streams = []
    while pos + 5 <= len(sec) - 4:
        st = sec[pos]
        pid = ((sec[pos + 1] & 0x1F) << 8) | sec[pos + 2]
        dl = ((sec[pos + 3] & 0x0F) << 8) | sec[pos + 4]
        streams.append({"pid": pid, "type": st,
                        "desc": bytes(sec[pos + 5:pos + 5 + dl])})
        pos += 5 + dl
    return {"program": (sec[3] << 8) | sec[4], "version": (sec[5] >> 1) & 31,
            "pcr_pid": ((sec[8] & 0x1F) << 8) | sec[9],
            "info": bytes(sec[12:12 + info_len]), "streams": streams}


def parse_sdt(sec):
    services = {}
    pos = 11
    while pos + 5 <= len(sec) - 4:
        sid = (sec[pos] << 8) | sec[pos + 1]
        dl = ((sec[pos + 3] & 0x0F) << 8) | sec[pos + 4]
        desc = bytes(sec[pos + 5:pos + 5 + dl])
        name = provider = None
        d = 0
        well_formed = len(desc) == dl
        while d + 2 <= len(desc):
            tag, ln = desc[d], desc[d + 1]
            if d + 2 + ln > len(desc):
                well_formed = False
                break
            if tag == 0x48 and ln >= 3:
                body = desc[d + 2:d + 2 + ln]
                pl = body[1]
                if 2 + pl >= len(body) or 3 + pl + body[2 + pl] != len(body):
                    well_formed = False
                    break
                provider = body[2:2 + pl].decode("latin-1")
                nl = body[2 + pl]
                name = body[3 + pl:3 + pl + nl].decode("latin-1")
            d += 2 + ln
        if d != len(desc):
            well_formed = False         # loop does not end on a boundary
        services[sid] = {"desc": desc, "name": name, "provider": provider,
                         "running": sec[pos + 3] >> 5, "well_formed": well_formed}
        pos += 5 + dl
    return {"ts_id": (sec[3] << 8) | sec[4], "version": (sec[5] >> 1) & 31,
            "onid": (sec[8] << 8) | sec[9], "services": services}


SECTION_TYPES = (0x05, 0x0A, 0x0B, 0x0C, 0x0D, 0x86)


class Analysis:
    """
    One pass over a transport stream: structure, continuity, clock and the
    content of one service.
    """

    def __init__(self, path, program=0):
        self.path = path
        self.packets = 0
        self.sync_errors = 0
        self.cc_errors = {}             # pid -> count
        self.pid_packets = {}
        self.psi_crc_errors = 0
        self.pat = None
        self.pmt = None
        self.sdt = None
        self.program = program
        self.pmt_pid = None
        self.pat_idx, self.pmt_idx, self.sdt_idx = [], [], []
        self.pat_versions, self.pmt_versions = [], []
        self.sdt_seen = set()           # every (service id, name) ever listed
        self.sdt_by_version = {}        # version -> set of distinct sections
        self.pcr_disc = 0               # PCR packets flagged as a discontinuity
        self.pcrs = []                  # (packet index, pcr) on the PCR PID
        self.pes = {}                   # pid -> list of dict
        self.sections = {}              # pid -> list of bytes
        self.rai = {}                   # pid -> number of RAI flags on PUSI
        self._run()

    def _run(self):
        last_cc = {}
        last_raw = {}
        dup = {}
        asm = {0: SectionAssembler(), 0x11: SectionAssembler()}
        cur = {}                        # pid -> [first_idx, bytearray, rai]
        es_kind = {}

        def finish(pid):
            c = cur.pop(pid, None)
            if c is None:
                return
            data = bytes(c[1])
            rec = {"first": c[0], "last": c[3], "len": len(data),
                   "sha": hashlib.sha1(data).hexdigest(), "rai": c[2],
                   "sha_nots": None, "pts": None, "dts": None, "ok": True}
            if len(data) < 9 or data[:3] != b"\x00\x00\x01":
                rec["ok"] = False
            else:
                flags = data[7] >> 6
                blank = bytearray(data)
                if flags & 2:
                    rec["pts"] = parse_pts(data[9:14])
                    rec["dts"] = rec["pts"]
                    blank[9:14] = b"\0" * 5
                if flags == 3:
                    rec["dts"] = parse_pts(data[14:19])
                    blank[14:19] = b"\0" * 5
                # Same packet with the time stamps blanked: lets a test say
                # "identical apart from a shift of the time line".
                rec["sha_nots"] = hashlib.sha1(bytes(blank)).hexdigest()
                plen = (data[4] << 8) | data[5]
                if plen and plen + 6 != len(data):
                    rec["ok"] = False
            self.pes.setdefault(pid, []).append(rec)

        for pkt in read_packets(self.path):
            self.packets += 1
            if pkt.raw[0] != 0x47:
                self.sync_errors += 1
                continue
            pid = pkt.pid
            self.pid_packets[pid] = self.pid_packets.get(pid, 0) + 1
            if pid == 0x1FFF:
                continue

            # continuity
            if pid in last_cc and not pkt.disc:
                last = last_cc[pid]
                if pkt.afc & 1:
                    if pkt.cc == ((last + 1) & 15):
                        dup[pid] = False
                    elif pkt.cc == last and not dup.get(pid) and \
                            pkt.raw == last_raw.get(pid):
                        dup[pid] = True         # a true repeat of the packet
                        continue
                    else:
                        self.cc_errors[pid] = self.cc_errors.get(pid, 0) + 1
                elif pkt.cc != last:
                    self.cc_errors[pid] = self.cc_errors.get(pid, 0) + 1
            last_cc[pid] = pkt.cc
            last_raw[pid] = pkt.raw

            if pid == 0:
                for sec in asm[0].push(pkt):
                    if sec[0] != 0:
                        continue
                    if crc32_mpeg(sec):
                        self.psi_crc_errors += 1
                        continue
                    pat = parse_pat(sec)
                    self.pat = pat
                    self.pat_idx.append(pkt.idx)
                    self.pat_versions.append(pat["version"])
                    if not self.program and pat["programs"]:
                        self.program = next(iter(pat["programs"]))  # first listed
                    new_pid = pat["programs"].get(self.program)
                    if new_pid is not None and new_pid != self.pmt_pid:
                        self.pmt_pid = new_pid
                        asm[new_pid] = SectionAssembler()
            elif pid == 0x11:
                for sec in asm[0x11].push(pkt):
                    if sec[0] != 0x42:
                        continue
                    if crc32_mpeg(sec):
                        self.psi_crc_errors += 1
                        continue
                    self.sdt = parse_sdt(sec)
                    self.sdt_idx.append(pkt.idx)
                    self.sdt_by_version.setdefault(self.sdt["version"], set()).add(sec)
                    for sid, svc in self.sdt["services"].items():
                        self.sdt_seen.add((sid, svc["name"]))

            if pid == self.pmt_pid and pid not in (0, 0x11):
                for sec in asm[pid].push(pkt):
                    if sec[0] != 0x02:
                        continue
                    if ((sec[3] << 8) | sec[4]) != self.program:
                        continue
                    if crc32_mpeg(sec):
                        self.psi_crc_errors += 1
                        continue
                    self.pmt = parse_pmt(sec)
                    self.pmt_idx.append(pkt.idx)
                    self.pmt_versions.append(self.pmt["version"])
                    es_kind = {s["pid"]: ("sec" if s["type"] in SECTION_TYPES else "pes")
                               for s in self.pmt["streams"]}
                    for p, k in es_kind.items():
                        if k == "sec" and p not in asm:
                            asm[p] = SectionAssembler()

            if self.pmt and pid == self.pmt["pcr_pid"] and pkt.pcr is not None:
                self.pcrs.append((pkt.idx, pkt.pcr))
                if pkt.disc:
                    self.pcr_disc += 1

            kind = es_kind.get(pid)
            if kind == "pes":
                if pkt.pusi:
                    finish(pid)
                    cur[pid] = [pkt.idx, bytearray(), pkt.rai, pkt.idx]
                    if pkt.rai:
                        self.rai[pid] = self.rai.get(pid, 0) + 1
                if pid in cur and pkt.payload:  # not PCR-only packets
                    cur[pid][1] += pkt.payload
                    cur[pid][3] = pkt.idx
            elif kind == "sec":
                for sec in asm[pid].push(pkt):
                    self.sections.setdefault(pid, []).append(sec)

        for pid in list(cur):
            finish(pid)

    # ---- derived checks ------------------------------------------------

    def total_cc_errors(self):
        return sum(self.cc_errors.values())

    def unwrapped_pcrs(self):
        """PCR list with the 33-bit wrap removed (monotonic if the stream is)."""
        out = []
        acc = None
        prev = None
        for idx, pcr in self.pcrs:
            acc = pcr if acc is None else acc + mod_diff(pcr, prev, PCR_MOD)
            prev = pcr
            out.append((idx, acc))
        return out

    def pcr_accuracy(self):
        """
        For a CBR stream: fit the bitrate from the first and last PCR and
        return (bitrate, worst deviation of any PCR from its byte position
        in nanoseconds, worst PCR interval in ms).
        """
        p = self.unwrapped_pcrs()
        if len(p) < 3:
            return None
        (i0, c0), (i1, c1) = p[0], p[-1]
        ticks_per_pkt = (c1 - c0) / (i1 - i0)
        bitrate = TS * 8 * 27e6 / ticks_per_pkt
        worst = max(abs((c - c0) - (i - i0) * ticks_per_pkt) for i, c in p)
        gap = max(b[1] - a[1] for a, b in zip(p, p[1:]))
        return bitrate, worst / 27e6 * 1e9, gap / 27000.0

    def pcr_monotonic(self):
        p = self.unwrapped_pcrs()
        return all(b[1] > a[1] for a, b in zip(p, p[1:]))

    def stc_at(self, idx):
        """
        Clock value at packet idx, interpolated between PCRs. Past the
        last PCR the clock can only be carried on by counting packets,
        which is exact in a constant-rate stream (one with null packets)
        and meaningless in a variable-rate one: there the answer is None.
        """
        p = self.unwrapped_pcrs()
        if len(p) < 2:
            return None
        if idx > p[-1][0] and not self.pid_packets.get(0x1FFF):
            return None
        import bisect
        k = bisect.bisect_right(p, (idx, float("inf")))
        k = min(max(k, 1), len(p) - 1)
        (i0, c0), (i1, c1) = p[k - 1], p[k]
        return c0 + (c1 - c0) * (idx - i0) / (i1 - i0)

    def late_pes(self):
        """PES packets whose last byte leaves after their own DTS."""
        late = 0
        for pid, lst in self.pes.items():
            for r in lst:
                if r["dts"] is None:
                    continue
                t = self.stc_at(r["last"] + 1)
                if t is None:
                    continue
                dts27 = (t // 300 + mod_diff(r["dts"], int(t // 300) % PTS_MOD, PTS_MOD)) * 300
                if t > dts27:
                    late += 1
        return late

    def lateness_ms(self):
        """For every PES packet that misses its own DTS: by how much, in ms."""
        out = []
        for lst in self.pes.values():
            for r in lst:
                if r["dts"] is None:
                    continue
                t = self.stc_at(r["last"] + 1)
                if t is None:
                    continue
                dts27 = (t // 300 + mod_diff(r["dts"], int(t // 300) % PTS_MOD, PTS_MOD)) * 300
                if t > dts27:
                    out.append((t - dts27) / 27000.0)
        return out

    def lead_ms(self):
        """For every PES packet with a DTS: how long before that DTS the
        packet was complete, in ms (negative = late), on the stream's PCR."""
        out = []
        for lst in self.pes.values():
            for r in lst:
                if r["dts"] is None:
                    continue
                t = self.stc_at(r["last"] + 1)
                if t is None:
                    continue
                dts27 = (t // 300 + mod_diff(r["dts"], int(t // 300) % PTS_MOD, PTS_MOD)) * 300
                out.append((dts27 - t) / 27000.0)
        return out

    def max_interval_ms(self, idx_list):
        """Longest gap between table repetitions, measured on the PCR clock."""
        if len(idx_list) < 2:
            return None
        ts = [t for t in (self.stc_at(i) for i in idx_list) if t is not None]
        if len(ts) < 2:
            return None
        return max(b - a for a, b in zip(ts, ts[1:])) / 27000.0

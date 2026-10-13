"""
UDP helpers for the live tests: play a transport stream file at its own
bit rate, and capture what comes back with arrival times.
"""
import socket
import struct
import threading
import time

from tslib import read_packets, mod_diff, PCR_MOD

TS = 188


def file_bitrate(path):
    """Bit rate of a CBR file from the first PCR PID found."""
    first = last = None
    pid = None
    for pkt in read_packets(path):
        if pkt.pcr is None:
            continue
        if pid is None:
            pid = pkt.pid
        if pkt.pid != pid:
            continue
        if first is None:
            first = (pkt.idx, pkt.pcr)
        last = (pkt.idx, pkt.pcr)
    ticks = mod_diff(last[1], first[1], PCR_MOD)
    return (last[0] - first[0]) * TS * 8 * 27e6 / ticks


class Player:
    """
    Sends files one after another to (host, port), 7 packets per datagram,
    paced by the file's own bit rate.

    gate(elapsed_seconds) -> bool decides whether a datagram is really sent;
    returning False while the clock keeps running imitates a network outage.
    """

    def __init__(self, host, port, rtp=False, mcast_if=None, also=None):
        """also: a second (host, port) that gets a copy of every datagram."""
        self.addr = (host, port)
        self.also = also
        self.rtp = rtp
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        if mcast_if:
            self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                                 socket.inet_aton(mcast_if))
            self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
        self.seq = 0
        self.sent = 0
        self.worst_wake = 0.0           # seconds; see play()

    def play(self, path, gate=None, speed=1.0, stall=None, at=None, rate=None,
             tempo=None, stop_after=None):
        """
        stall=(when, seconds[, speed]): stop sending for that long, then
        send the backlog (data held up, nothing lost): as fast as possible,
        or at `speed` times real time if given.
        at={seconds: callable}: call back once when that point is reached.
        tempo=(from_s, to_s, factor): send that part of the file `factor`
        times faster than real time.
        stop_after=seconds: stop sending at that point in the file.
        """
        with open(path, "rb") as f:
            data = f.read()
        # rate: the file's true bit rate, for files whose PCRs do not span
        # the whole of it evenly (a piece cut out, a PCR that stops).
        rate = (rate or file_bitrate(path)) * speed
        step = 7 * TS * 8 / rate
        t0 = time.perf_counter()
        n = 0
        at = dict(at or {})
        lag = 0.0                       # how far behind schedule we are sending
        drain = 0.0                     # seconds of lag recovered per datagram
        due = t0
        for off in range(0, len(data) - 7 * TS + 1, 7 * TS):
            if stop_after is not None and n * step >= stop_after:
                break
            delay = due + lag - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
                # How late this thread woke: a measure of the test machine,
                # not of anything under test.
                woke_late = time.perf_counter() - (due + lag)
                if woke_late > self.worst_wake:
                    self.worst_wake = woke_late
            fast = tempo and tempo[0] <= n * step < tempo[1]
            due += step / tempo[2] if fast else step
            lag = max(0.0, lag - drain)
            if stall and n * step >= stall[0]:
                time.sleep(stall[1])
                if len(stall) > 2:      # release the backlog at a finite rate
                    lag = stall[1]
                    drain = step * (1 - 1 / stall[2])
                stall = None
            for when in [k for k in at if n * step >= k]:
                at.pop(when)()
            n += 1
            if gate and not gate(n * step):
                continue
            chunk = data[off:off + 7 * TS]
            if self.rtp:
                hdr = struct.pack(">BBHII", 0x80, 33, self.seq & 0xFFFF,
                                  int(n * step * 90000) & 0xFFFFFFFF, 0x1234)
                self.seq += 1
                chunk = hdr + chunk
            self.sock.sendto(chunk, self.addr)
            if self.also:
                self.sock.sendto(bytes(len(chunk)), self.also)
            self.sent += 1

    def close(self):
        self.sock.close()


SO_TIMESTAMPNS = 35                 # Linux, <asm-generic/socket.h>


class Capture:
    """Receives datagrams on a UDP port in a background thread."""

    def __init__(self, port, host="127.0.0.1", join_if=None):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, port))
        if join_if:
            self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                                 socket.inet_aton(host) + socket.inet_aton(join_if))
        self.sock.settimeout(0.2)
        # Arrival times are taken by the kernel when the datagram reaches
        # the socket, not by this thread when it gets round to reading it.
        # Otherwise a busy test machine shows up as jitter in the output.
        self.kernel_times = False
        try:
            self.sock.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPNS, 1)
            self.kernel_times = True
        except OSError:
            pass
        self.chunks = []
        self.times = []
        self._run = True
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    def _loop(self):
        while self._run:
            try:
                d, anc, _, _ = self.sock.recvmsg(65536, 64)
            except socket.timeout:
                continue
            except OSError:
                break
            t = None
            for level, kind, val in anc:
                if level == socket.SOL_SOCKET and kind == SO_TIMESTAMPNS and len(val) >= 16:
                    sec, nsec = struct.unpack("qq", val[:16])
                    t = sec + nsec / 1e9
            self.times.append(t if t is not None else time.time())
            self.chunks.append(d)

    def stop(self, path=None):
        self._run = False
        self.thread.join()
        self.sock.close()
        if path:
            with open(path, "wb") as f:
                for c in self.chunks:
                    f.write(c)
        return self

    def max_gap_ms(self):
        t = self.times
        return max((b - a for a, b in zip(t, t[1:])), default=0) * 1000

    def pacing_error_ms(self, bitrate):
        """
        How far each datagram's arrival is from where a perfectly constant
        `bitrate` would put it. The reference line is placed through the
        median, so one late datagram at either end cannot tilt it and make
        every other arrival look wrong.
        """
        t = self.times
        if len(t) < 10:
            return None
        pos = 0
        resid = []
        for ti, c in zip(t, self.chunks):
            resid.append(ti - pos * 8 / bitrate)
            pos += len(c)
        mid = sorted(resid)[len(resid) // 2]
        errs = sorted(abs(r - mid) * 1000 for r in resid)
        return {"max": errs[-1], "p99": errs[int(len(errs) * 0.99)],
                "median": errs[len(errs) // 2]}

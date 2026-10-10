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

    def play(self, path, gate=None, speed=1.0, stall=None, at=None):
        """
        stall=(when, seconds): stop sending for that long, then send the
        backlog as fast as possible (data held up, nothing lost).
        at={seconds: callable}: call back once when that point is reached.
        """
        with open(path, "rb") as f:
            data = f.read()
        rate = file_bitrate(path) * speed
        step = 7 * TS * 8 / rate
        t0 = time.perf_counter()
        n = 0
        at = dict(at or {})
        for off in range(0, len(data) - 7 * TS + 1, 7 * TS):
            due = t0 + n * step
            delay = due - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
            if stall and n * step >= stall[0]:
                time.sleep(stall[1])
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
        self.chunks = []
        self.times = []
        self._run = True
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    def _loop(self):
        while self._run:
            try:
                d = self.sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            self.times.append(time.perf_counter())
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
        Worst and typical deviation of datagram arrival from an ideal
        constant rate (line fitted through first and last arrival).
        """
        t = self.times
        if len(t) < 10:
            return None
        sizes = [len(c) for c in self.chunks]
        pos = 0
        errs = []
        total = sum(sizes[:-1])
        span = t[-1] - t[0]
        for ti, s in zip(t, sizes):
            ideal = t[0] + span * pos / total if total else t[0]
            errs.append(abs(ti - ideal) * 1000)
            pos += s
        errs.sort()
        return {"max": errs[-1], "p99": errs[int(len(errs) * 0.99)],
                "median": errs[len(errs) // 2]}

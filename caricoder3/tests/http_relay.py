#!/usr/bin/env python3
"""
A test HTTP server: what arrives on a UDP port is sent to whoever GETs
any path (one client at a time), as an HTTP/1.1 body, chunked or plain.

  http_relay.py UDP_PORT HTTP_PORT [--plain] [--hold S] [--drop-after S]

--hold S        answer only once S seconds of stream have come in, and
                send those first, at once (a server's backlog)
--drop-after S  close the first connection S seconds into the answer
/redirect       answers 302 to /live
"""
import socket
import sys
import threading
import time

udp_port, http_port = int(sys.argv[1]), int(sys.argv[2])
plain = "--plain" in sys.argv
hold = float(sys.argv[sys.argv.index("--hold") + 1]) if "--hold" in sys.argv else 0.0
drop_after = float(sys.argv[sys.argv.index("--drop-after") + 1]) if "--drop-after" in sys.argv else 0.0

lock = threading.Condition()
buf = []                  # (time, bytes) since the first datagram
first = [None]


def rx():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", udp_port))
    while True:
        d = s.recv(2048)
        with lock:
            if first[0] is None:
                first[0] = time.time()
            buf.append(d)
            lock.notify_all()


def serve(c, dropped):
    req = b""
    while b"\r\n\r\n" not in req:
        part = c.recv(4096)
        if not part:
            return
        req += part
    path = req.split(b" ")[1]
    if path == b"/redirect":
        c.sendall(b"HTTP/1.1 302 Found\r\nLocation: /live\r\nContent-Length: 0\r\n\r\n")
        c.close()
        return
    with lock:
        while hold and (first[0] is None or time.time() - first[0] < hold):
            lock.wait(0.05)
        pos = len(buf) if not hold else 0
    c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\n" +
              (b"" if plain else b"Transfer-Encoding: chunked\r\n") + b"Connection: close\r\n\r\n")
    t0 = time.time()
    try:
        while True:
            with lock:
                while pos >= len(buf):
                    lock.wait(0.2)
                out = b"".join(buf[pos:])
                pos = len(buf)
            c.sendall(out if plain else b"%x\r\n" % len(out) + out + b"\r\n")
            if drop_after and not dropped[0] and time.time() - t0 > drop_after:
                dropped[0] = True
                break
    except OSError:
        pass
    c.close()


threading.Thread(target=rx, daemon=True).start()
ls = socket.socket()
ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
ls.bind(("127.0.0.1", http_port))
ls.listen(4)
dropped = [False]
while True:
    c, _ = ls.accept()
    threading.Thread(target=serve, args=(c, dropped), daemon=True).start()

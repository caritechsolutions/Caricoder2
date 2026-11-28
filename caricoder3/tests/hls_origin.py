"""A live HLS origin for the tests: five 2 s segments going round for ever (a discontinuity at each lap).
hls_origin.py DIR PORT [tls]: DIR holds s0..s4.ts, e0..e4.ts (AES-128, IV 16 + n), key.bin, cert.pem, key.pem.
/master.m3u8 (two qualities), /hi.m3u8, /lo.m3u8, /enc.m3u8, /fmp4.m3u8, /go (a redirect to the master)"""
import http.server, os, ssl, sys, time
D = sys.argv[1]
T0 = time.time() - 100
N, DUR = 5, 2
def media(prefix, enc=False, fmp4=False):
    n = int((time.time() - T0) / DUR)
    out = ["#EXTM3U", "#EXT-X-VERSION:3", f"#EXT-X-TARGETDURATION:{DUR}", f"#EXT-X-MEDIA-SEQUENCE:{n - 4}"]
    if fmp4:
        out.append('#EXT-X-MAP:URI="init.mp4"')
    for q in range(n - 4, n + 1):
        i = q % N
        if i == 0:
            out.append("#EXT-X-DISCONTINUITY")
        if enc:
            out.append(f'#EXT-X-KEY:METHOD=AES-128,URI="key.bin",IV=0x{i + 16:032x}')
        out.append(f"#EXTINF:{DUR:.3f},")
        out.append(f"{prefix}{i}.ts?n={q}")
    return "\n".join(out) + "\n"
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def send(self, body, ctype, chunked=False):
        self.send_response(200); self.send_header("Content-Type", ctype)
        if chunked:
            self.send_header("Transfer-Encoding", "chunked"); self.send_header("Connection", "close"); self.end_headers()
            for i in range(0, len(body), 50000):
                c = body[i:i + 50000]; self.wfile.write(b"%x\r\n" % len(c) + c + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(body))); self.send_header("Connection", "close"); self.end_headers()
            self.wfile.write(body)
        self.close_connection = True
    def do_GET(self):
        p = self.path.split("?")[0]
        if p == "/go":
            self.send_response(302); self.send_header("Location", "/sub/../master.m3u8"); self.send_header("Content-Length", "0"); self.end_headers(); return
        if p == "/master.m3u8":
            return self.send(b'#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=600000,RESOLUTION=320x180\nlo.m3u8\n'
                             b'#EXT-X-STREAM-INF:BANDWIDTH=1500000,RESOLUTION=640x360\nhi.m3u8\n', "application/vnd.apple.mpegurl")
        if p in ("/hi.m3u8", "/lo.m3u8"): return self.send(media("s").encode(), "application/vnd.apple.mpegurl")
        if p == "/enc.m3u8": return self.send(media("e", enc=True).encode(), "application/vnd.apple.mpegurl")
        if p == "/fmp4.m3u8": return self.send(media("s", fmp4=True).encode(), "application/vnd.apple.mpegurl")
        f = os.path.join(D, os.path.basename(p))
        if os.path.exists(f):
            return self.send(open(f, "rb").read(), "video/MP2T", chunked=p.startswith("/s") and p.endswith("1.ts"))
        self.send_error(404)
srv = http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[2])), H)
if len(sys.argv) > 3:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(os.path.join(D, "cert.pem"), os.path.join(D, "key.pem"))
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
srv.serve_forever()

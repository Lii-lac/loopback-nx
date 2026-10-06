#!/usr/bin/env python3
"""A stand-in for github.com/<owner>/<repo>/releases, for tests/run_update_test.sh.

  update_server.py <mode> <port>

It answers like GitHub does: /releases/latest redirects to /releases/tag/<tag>, and the assets sit under /releases/download/<tag>/ behind a
second redirect. Modes:
  good      tag v1.0.1, valid NRO, matching checksum
  badhash   checksum of other data
  notnro    matching checksum but the file has no NRO header
  nodl      the checksum is there, the NRO is a 404
  nosha     the NRO is there, the checksum is a 404
  private   /releases/latest is a 404 (what a private repository looks like to the public)
  empty     /releases/latest redirects to /releases (no release yet)
  slow      like good, but the NRO trickles out so a cancel has time to land
"""
import hashlib
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MODE, PORT = sys.argv[1], int(sys.argv[2])
REPO = "/Lii-lac/loopback-nx"
TAG = "v1.0.1"

NRO = b"\0" * 0x10 + b"NRO0" + bytes((i * 7 + 3) & 255 for i in range(2 * 1024 * 1024))
BAD = b"\0" * 0x10 + b"ZIP0" + NRO[0x14:]
BODY = BAD if MODE == "notnro" else NRO
SHA_OF = BAD if MODE == "notnro" else NRO
if MODE == "badhash":
    SHA_OF = NRO + b"x"
SHA = (hashlib.sha256(SHA_OF).hexdigest() + " *loopback.nro\n").encode()


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, body=b"", headers=None, head=False):
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if not head:
            if MODE == "slow" and len(body) > 100000:
                for i in range(0, len(body), 65536):
                    self.wfile.write(body[i:i + 65536])
                    self.wfile.flush()
                    time.sleep(0.12)
            else:
                self.wfile.write(body)

    def handle_any(self, head):
        p = self.path
        base = "http://127.0.0.1:%d" % PORT
        if p == REPO + "/releases/latest":
            if MODE == "private":
                return self.send(404, b"Not Found", head=head)
            if MODE == "empty":
                return self.send(302, headers={"Location": base + REPO + "/releases"}, head=head)
            return self.send(302, headers={"Location": base + REPO + "/releases/tag/" + TAG}, head=head)
        pre = REPO + "/releases/download/" + TAG + "/"
        if p == pre + "loopback.nro.sha256":
            if MODE == "nosha":
                return self.send(404, b"Not Found", head=head)
            return self.send(302, headers={"Location": base + "/blob/sha"}, head=head)
        if p == pre + "loopback.nro":
            if MODE == "nodl":
                return self.send(404, b"Not Found", head=head)
            return self.send(302, headers={"Location": base + "/blob/nro"}, head=head)
        if p == "/blob/sha":
            return self.send(200, SHA, head=head)
        if p == "/blob/nro":
            return self.send(200, BODY, head=head)
        return self.send(404, b"Not Found", head=head)

    def do_GET(self):
        self.handle_any(False)

    def do_HEAD(self):
        self.handle_any(True)


ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()

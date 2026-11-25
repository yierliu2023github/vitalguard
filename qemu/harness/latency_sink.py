#!/usr/bin/env python3
"""HTTP sink for the latency-phase tracelogger capture: saves PUT
bodies to latency.kev in this directory (one capture expected)."""
import http.server
import os

OUT = os.path.expanduser("~/workspace/qnx/custom-image/latency.kev")


class H(http.server.BaseHTTPRequestHandler):
    def _save(self):
        n = int(self.headers.get("Content-Length", 0))
        with open(OUT, "wb") as f:
            left = n
            while left > 0:
                chunk = self.rfile.read(min(1 << 20, left))
                if not chunk:
                    break
                f.write(chunk)
                left -= len(chunk)
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"ok\n")
        print(f"saved {n - left} bytes to {OUT}", flush=True)

    do_PUT = _save
    do_POST = _save

    def log_message(self, *a):
        pass


http.server.HTTPServer(("0.0.0.0", 9999), H).serve_forever()

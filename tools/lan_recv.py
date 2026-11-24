"""Dev helper: accept HTTP PUT uploads from the test device into a directory, and serve
files from it for the other direction.
usage: lan_recv.py <out_dir> <port>      (device: curl -T file http://host:port/name,
                                          curl -o file http://host:port/name)"""
import http.server, os, sys
out, port = sys.argv[1], int(sys.argv[2])
os.makedirs(out, exist_ok=True)
class H(http.server.BaseHTTPRequestHandler):
    def do_PUT(self):
        name = os.path.basename(self.path) or 'upload.bin'
        n = int(self.headers.get('Content-Length', 0)); left = n
        with open(os.path.join(out, name), 'wb') as f:
            if self.headers.get('Transfer-Encoding', '').lower() == 'chunked':
                while True:
                    size = int(self.rfile.readline().strip() or b'0', 16)
                    if not size: break
                    f.write(self.rfile.read(size)); self.rfile.readline()
            else:
                while left > 0:
                    b = self.rfile.read(min(left, 1 << 20))
                    if not b: break
                    f.write(b); left -= len(b)
        self.send_response(201); self.end_headers()
    def do_GET(self):
        p = os.path.join(out, os.path.basename(self.path))
        if not os.path.isfile(p):
            self.send_response(404); self.end_headers(); return
        self.send_response(200); self.send_header('Content-Length', str(os.path.getsize(p))); self.end_headers()
        with open(p, 'rb') as f:
            while True:
                b = f.read(1 << 20)
                if not b: break
                self.wfile.write(b)
http.server.ThreadingHTTPServer(('0.0.0.0', port), H).serve_forever()

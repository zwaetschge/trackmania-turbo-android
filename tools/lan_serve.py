"""Read-only dev file server for importing the user's game files to a test
device on the LAN.  usage: lan_serve.py <game_dir> <manifest.txt> <port>
Serves only paths listed in the manifest (plus manifest.txt itself)."""
import http.server, os, sys, urllib.parse
root, manifest, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
allowed = {l.split('\t')[0] for l in open(manifest) if l.strip()}
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        rel = urllib.parse.unquote(self.path.lstrip('/'))
        path = manifest if rel == 'manifest.txt' else (os.path.join(root, rel) if rel in allowed else None)
        if not path or not os.path.isfile(path):
            self.send_error(404); return
        self.send_response(200)
        self.send_header('Content-Length', str(os.path.getsize(path)))
        self.end_headers()
        with open(path, 'rb') as f:
            while True:
                b = f.read(1 << 20)
                if not b: break
                self.wfile.write(b)
http.server.ThreadingHTTPServer(('0.0.0.0', port), H).serve_forever()

"""Run the AI C++ unit binary against a loopback transport fault fixture."""
import http.server
import os
import subprocess
import sys
import threading


class Handler(http.server.BaseHTTPRequestHandler):
    attempts = 0

    def log_message(self, *args):
        pass

    def do_POST(self):
        self.rfile.read(int(self.headers['Content-Length']))
        if self.path == '/timeout':
            self.send_response(503)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        Handler.attempts += 1
        body = b'{"ok":true}'
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        # The first connection closes before the declared body is complete.
        self.wfile.write(body[:5] if Handler.attempts == 1 else body)
        self.wfile.flush()
        self.close_connection = True


if __name__ == '__main__':
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = 'http://127.0.0.1:%d' % server.server_port
    env = dict(os.environ, AI_REVIEW_HTTP_URL=base + '/partial',
               AI_REVIEW_TIMEOUT_URL=base + '/timeout',
               NO_PROXY='127.0.0.1', no_proxy='127.0.0.1')
    try:
        result = subprocess.run(sys.argv[1:], env=env, check=False)
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    raise SystemExit(result.returncode)

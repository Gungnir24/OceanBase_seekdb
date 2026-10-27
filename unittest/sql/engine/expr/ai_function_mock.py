#!/usr/bin/env python3
"""Deterministic AI protocol mock; no model or external service is required."""
import argparse
import hashlib
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def embedding(text, dimension):
    digest = hashlib.sha256(text.encode("utf-8")).digest()
    return [round((digest[i % len(digest)] - 128) / 128, 6) for i in range(dimension)]


def serve(host, port, log_file):
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def reply(self, value, status=200):
            data = json.dumps(value, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            self.reply({"ok": True})

        def do_POST(self):
            try:
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                if log_file:
                    with lock, open(log_file, "a", encoding="utf-8") as log:
                        log.write(json.dumps({"path": self.path, "body": body}, ensure_ascii=False) + "\n")
                if "/http-error/" in self.path:
                    return self.reply({"error": "injected failure"}, 400)
                if "/nonobject/" in self.path:
                    return self.reply([])
                if self.path.endswith("/embeddings") or self.path.endswith("/api/embed"):
                    values = body["input"]
                    if isinstance(values, str):
                        values = [values]
                    if not values or any(not isinstance(value, str) for value in values):
                        return self.reply({"error": "input must contain strings"}, 400)
                    dimension = body.get("dimensions", 768)
                    if not isinstance(dimension, int) or dimension <= 0:
                        return self.reply({"error": "invalid dimensions"}, 400)
                    vectors = [embedding(value, dimension) for value in values]
                    if "/bad-element/" in self.path:
                        vectors[0][0] = "invalid"
                    if "/bad-dimension/" in self.path:
                        vectors[0].append(0)
                    if self.path.endswith("/api/embed"):
                        return self.reply({"embeddings": vectors})
                    entries = [{"index": index, "embedding": vector} for index, vector in enumerate(vectors)]
                    if "/bad-index/" in self.path:
                        entries[0]["index"] = len(entries)
                    if "/short/" in self.path:
                        entries.pop()
                    # Always reverse the wire order to exercise index recovery.
                    return self.reply({"data": list(reversed(entries))})
                if self.path.endswith("/rerank"):
                    query = set(body["query"].casefold().split())
                    entries = [{"index": index,
                                "relevance_score": len(query.intersection(text.casefold().split())) / max(1, len(query))}
                               for index, text in enumerate(body["documents"])]
                    if "/bad-index/" in self.path:
                        entries[0]["index"] = len(entries)
                    return self.reply({"results": list(reversed(entries))})
                text = "mock completion"
                if "/empty/" in self.path:
                    text = ""
                if self.path.endswith("/api/generate"):
                    if body.get("stream") is not False:
                        return self.reply({"error": "stream must be false"}, 400)
                    return self.reply({"response": text, "done": True})
                if self.path.endswith("/chat/completions"):
                    return self.reply({"choices": [{"message": {"content": text}}]})
                return self.reply({"error": "unknown route"}, 404)
            except (ValueError, TypeError, KeyError) as error:
                return self.reply({"error": str(error)}, 400)

    ThreadingHTTPServer((host, port), Handler).serve_forever()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--log-file")
    args = parser.parse_args()
    serve(args.host, args.port, args.log_file)

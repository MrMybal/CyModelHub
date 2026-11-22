# SPDX-FileCopyrightText: 2026 Cyberalien
# SPDX-License-Identifier: GPL-3.0-only

"""When the client library sends a request again, with a stand-in for the service.

    python tests/retry.py <CyModelHubCli>

The stand-in receives a request and closes the connection without answering: for the client
the answer is lost, and it cannot know whether the request was carried out. It may then send
the request again only when that cannot repeat an effect.
"""

import http.server
import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import threading

FAILURES = []
TOKEN = "t" * 40
SEEN = []          # (operation, idempotency key) of every request received
DROP = set()       # operations whose next request gets no answer
REFUSE = set()     # operations whose next request is refused as unauthenticated


def check(condition, label):
    if not condition:
        FAILURES.append(label)
        print("FAILED:", label)


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *arguments):
        pass

    def answer(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self.answer(200 if self.path == "/health" else 404, {"ok": True})

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))))
        operation = request.get("name", request.get("method"))
        SEEN.append((operation, request.get("arguments", {}).get("idempotencyKey")))
        if self.headers.get("Authorization") != f"Bearer {TOKEN}":
            self.answer(401, {})
        elif operation in REFUSE:
            REFUSE.discard(operation)
            self.answer(401, {})
        elif operation in DROP:
            # Received, perhaps carried out, never answered.
            DROP.discard(operation)
            self.close_connection = True
            self.connection.shutdown(socket.SHUT_RDWR)
        else:
            self.answer(200, {"operation": operation, "models": [], "total": 0})


def main():
    cli = sys.argv[1]
    root = pathlib.Path(tempfile.mkdtemp(prefix="cymodelhub-retry-"))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    (root / "data").mkdir()
    (root / "data" / "service.json").write_text(json.dumps(
        {"name": "CyModelHub", "port": server.server_address[1], "pid": os.getpid(), "token": TOKEN, "executable": ""}))
    environment = dict(os.environ, CYMODELHUB_DATA=str(root / "data"), CYMODELHUB_SERVICE=str(root / "no-service"))

    def call(operation, arguments, lose=False, refuse=False):
        SEEN.clear()
        if lose:
            DROP.add(operation)
        if refuse:
            REFUSE.add(operation)
        result = subprocess.run([cli, "call", operation, json.dumps(arguments)], env=environment,
                                capture_output=True, text=True, timeout=60)
        return result.returncode == 0, result.stderr.strip(), list(SEEN)

    try:
        ok, _, seen = call("models_list", {})
        check(ok and len(seen) == 1, "a request that is answered is sent once")

        ok, _, seen = call("models_list", {}, lose=True)
        check(ok and len(seen) == 2, "a read whose answer is lost is sent again")

        ok, error, seen = call("models_remove", {"id": "abc"}, lose=True)
        check(not ok and len(seen) == 1 and "NetworkError" in error,
              "a write whose answer is lost is NOT sent again: the error reaches the caller")

        ok, error, seen = call("install_start", {"url": "https://example.com/m.gguf"}, lose=True)
        check(not ok and len(seen) == 1, "an installation without a key is not sent twice either")

        ok, _, seen = call("install_start", {"url": "https://example.com/m.gguf", "idempotencyKey": "retry-key-0001"},
                           lose=True)
        check(ok and len(seen) == 2 and {key for _, key in seen} == {"retry-key-0001"},
              "with an idempotency key it is sent again, with the same key")

        ok, _, seen = call("models_remove", {"id": "abc"}, refuse=True)
        check(ok and len(seen) == 2, "a request the service refused before acting is sent again after reconnecting")

        ok, _, seen = call("jobs_cancel", {"id": "j1"}, lose=True)
        check(ok and len(seen) == 2, "an operation with the same effect however often it runs is sent again")
    finally:
        server.shutdown()
        shutil.rmtree(root, ignore_errors=True)
    print("FAILED" if FAILURES else "OK")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())

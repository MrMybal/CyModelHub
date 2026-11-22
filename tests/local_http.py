# SPDX-FileCopyrightText: 2026 Cyberalien
# SPDX-License-Identifier: GPL-3.0-only

"""The HTTP exchange the client library uses to reach the service, against a local server.

    python tests/local_http.py <modelhub_http_probe>

The server answers in every way an HTTP/1.1 server may: an announced length, chunks, an
answer that ends with the connection, and several broken or slow answers.
"""

import json
import socket
import socketserver
import subprocess
import sys
import threading
import time

FAILURES = []
BIG = bytes((index * 7) % 26 + 97 for index in range(3_000_000))


def check(condition, label):
    if not condition:
        FAILURES.append(label)
        print("FAILED:", label)


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        data = b""
        while b"\r\n\r\n" not in data:
            block = self.request.recv(65536)
            if not block:
                return
            data += block
        head, _, body = data.partition(b"\r\n\r\n")
        lines = head.decode("latin-1").split("\r\n")
        method, path, _ = lines[0].split(" ")
        headers = {name.strip().lower(): value.strip() for name, value in (line.split(":", 1) for line in lines[1:])}
        while len(body) < int(headers.get("content-length", "0")):
            body += self.request.recv(65536)
        send = self.request.sendall

        def answer(content, status="200 OK"):
            send(f"HTTP/1.1 {status}\r\nContent-Type: application/json\r\ncontent-LENGTH:  {len(content)} \r\n\r\n"
                 .encode() + content)

        if path == "/length":
            answer(b'{"kind":"length"}')
        elif path == "/echo":
            answer(json.dumps({"method": method, "headers": headers, "body": body.decode()}).encode())
        elif path == "/denied":
            answer(b"", "401 Unauthorized")
        elif path == "/big":
            answer(BIG)
        elif path == "/chunked":
            # Pieces of 1, 26 (hexadecimal 1a) and 300 bytes, sent separately; the connection
            # then stays open, so the reader must stop at the last chunk by itself.
            send(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n")
            for piece in (b"[", b"abcdefghijklmnopqrstuvwxyz", b"\r\n" * 150, b"]"):
                send(f"{len(piece):x}\r\n".encode() + piece + b"\r\n")
                time.sleep(0.02)
            send(b"0\r\n\r\n")
            time.sleep(3)
        elif path == "/until-close":
            send(b"HTTP/1.0 200 OK\r\n\r\n")
            for piece in (b"first ", b"second ", b"third"):
                send(piece)
                time.sleep(0.02)
        elif path == "/truncated":
            send(b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly this")
        elif path == "/half-chunk":
            send(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10\r\nshort")
        elif path == "/garbage":
            send(b"SMTP ready\r\n\r\n")
        elif path == "/silent":
            time.sleep(3)
        elif path == "/slow-body":
            send(b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n12345")
            time.sleep(3)


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = False


def main():
    probe = sys.argv[1]
    server = Server(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()

    def ask(path, method="GET", timeout=5000, limit=64 * 1024 * 1024, body=None, bearer=None, to=None):
        command = [probe, str(to or port), method, path, str(timeout), str(limit)]
        if body is not None:
            command += [body] + ([bearer] if bearer else [])
        started = time.monotonic()
        output = subprocess.run(command, capture_output=True, text=True, timeout=60).stdout
        try:
            return json.loads(output), time.monotonic() - started
        except ValueError:
            return {"output": output}, time.monotonic() - started

    reply, _ = ask("/length")
    check(reply == {"status": 200, "size": 17, "body": '{"kind":"length"}'}, f"announced length: {reply}")

    reply, _ = ask("/echo", "POST", body='{"name":"model"}', bearer="secret-token")
    seen = json.loads(reply.get("body", "{}"))
    headers = seen.get("headers", {})
    check(reply.get("status") == 200 and seen.get("method") == "POST", f"POST reaches the server: {reply}")
    check(headers.get("host") == f"127.0.0.1:{port}", f"Host names the loopback address: {headers}")
    check(headers.get("authorization") == "Bearer secret-token", "the token is sent as a bearer credential")
    check(headers.get("content-type") == "application/json" and "origin" not in headers, "JSON body, no Origin")
    check(headers.get("content-length") == str(len(seen.get("body", "").encode())), "the body length is announced")
    check(headers.get("connection") == "close", "one exchange per connection")

    reply, _ = ask("/echo")
    headers = json.loads(reply.get("body", "{}")).get("headers", {})
    check("authorization" not in headers and "content-length" not in headers, "GET carries no body and no token")

    reply, _ = ask("/denied")
    check(reply == {"status": 401, "size": 0, "body": ""}, f"an empty refusal: {reply}")

    reply, _ = ask("/big")
    edges = (BIG[:16] + BIG[-16:]).decode()
    check(reply == {"status": 200, "size": len(BIG), "edges": edges}, "a body of several megabytes arrives whole")
    reply, _ = ask("/big", limit=1_000_000)
    check(reply == {"error": "ResponseTooLarge"}, f"the size limit is enforced: {reply}")

    reply, elapsed = ask("/chunked")
    expected = "[abcdefghijklmnopqrstuvwxyz" + "\r\n" * 150 + "]"
    check(reply.get("status") == 200 and reply.get("body") == expected, f"chunks are joined: {reply.get('size')}")
    check(elapsed < 2, f"reading stops at the last chunk ({elapsed:.1f} s)")

    reply, _ = ask("/until-close")
    check(reply == {"status": 200, "size": 18, "body": "first second third"}, f"answer ended by the close: {reply}")

    for path in ("/truncated", "/half-chunk", "/garbage"):
        reply, _ = ask(path)
        check(reply == {"error": "NetworkError"}, f"{path} is reported as a network error: {reply}")

    for path in ("/silent", "/slow-body"):
        reply, elapsed = ask(path, timeout=400)
        check(reply == {"error": "Timeout"}, f"{path} times out: {reply}")
        check(elapsed < 2, f"{path} gives up at the deadline ({elapsed:.1f} s)")

    # A port nobody listens on.
    closed = socket.socket()
    closed.bind(("127.0.0.1", 0))
    free = closed.getsockname()[1]
    closed.close()
    reply, elapsed = ask("/length", timeout=5000, to=free)
    check(reply.get("error") in ("NetworkError", "Timeout"), f"a closed port is an error: {reply}")
    check(elapsed < 5.5, f"a closed port does not hang ({elapsed:.1f} s)")

    reply, _ = ask("/length\r\nX-Injected: 1")
    check(reply == {"error": "NetworkError"}, f"a path with a line break is refused: {reply}")

    server.shutdown()
    if FAILURES:
        print(f"{len(FAILURES)} check(s) failed")
        return 1
    print("local HTTP: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

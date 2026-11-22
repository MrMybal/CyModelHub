# SPDX-FileCopyrightText: 2026 Cyberalien
# SPDX-License-Identifier: GPL-3.0-only

"""Installation of models, against a local HTTP server: no Internet access is used.

    python tests/download.py <CyModelHubCli> <CyModelHubService>

The server imitates what a model host does: a redirect that announces the SHA-256 of the file
(as Hugging Face does), byte ranges for resuming, and a variant that ignores ranges.
"""

import hashlib
import http.server
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import threading

FAILURES = []


def check(condition, label):
    if not condition:
        FAILURES.append(label)
        print("FAILED:", label)


def gguf(name, size):
    def text(value):
        data = value.encode()
        return struct.pack("<Q", len(data)) + data

    pairs = [text("general.architecture") + struct.pack("<I", 8) + text("llama"),
             text("general.name") + struct.pack("<I", 8) + text(name),
             text("tokenizer.ggml.model") + struct.pack("<I", 8) + text("gpt2")]
    head = b"GGUF" + struct.pack("<IQQ", 3, 0, len(pairs)) + b"".join(pairs)
    return head + bytes((index * 31 + len(name)) % 251 for index in range(size - len(head)))


FILES = {"tiny.gguf": gguf("Tiny", 20_000), "big.gguf": gguf("Big", 600_000), "plain.gguf": gguf("Plain", 50_000)}
RANGES = []


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *arguments):
        pass

    def do_GET(self):
        kind, _, name = self.path.strip("/").partition("/")
        data = FILES.get(name)
        if data is None:
            self.send_error(404)
            return
        if kind == "resolve":
            self.send_response(302)
            self.send_header("Location", f"/files/{name}")
            self.send_header("X-Linked-Etag", '"' + hashlib.sha256(data).hexdigest() + '"')
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        requested = self.headers.get("Range")
        if requested and kind == "files":
            RANGES.append((name, requested))
            first, _, last = requested.removeprefix("bytes=").partition("-")
            start = int(first)
            end = int(last) if last else len(data) - 1
            if start >= len(data):
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{len(data)}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            body = data[start:end + 1]
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{start + len(body) - 1}/{len(data)}")
        else:
            body = data
            self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    cli, service = sys.argv[1], sys.argv[2]
    root = pathlib.Path(tempfile.mkdtemp(prefix="cymodelhub-download-"))
    environment = dict(os.environ, CYMODELHUB_DATA=str(root / "data"), CYMODELHUB_SERVICE=service,
                       HF_HUB_CACHE=str(root / "hf"), OLLAMA_MODELS=str(root / "ollama"),
                       USERPROFILE=str(root / "home"), HOME=str(root / "home"), HF_TOKEN="")
    (root / "home").mkdir()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_address[1]}"

    def call(operation, arguments=None, expect_error=False):
        result = subprocess.run([cli, "call", operation, json.dumps(arguments or {})], env=environment,
                                capture_output=True, text=True, timeout=120)
        if result.returncode != 0:
            if not expect_error:
                raise RuntimeError(f"{operation}: {result.stderr.strip()}")
            return {"error": result.stderr.strip()}
        try:
            return json.loads(result.stdout)
        except json.JSONDecodeError:
            raise RuntimeError(f"{operation}: unexpected output {result.stdout!r} / {result.stderr!r}") from None

    def install(arguments):
        job = call("install_start", arguments)["job"]
        while True:
            state = call("jobs_get", {"id": job})["job"]
            if state["state"] != "running":
                return state

    try:
        store = pathlib.Path(call("hub_status")["managedStore"])

        # A redirect that announces the hash: the file is verified without being told what to expect.
        done = install({"url": f"{base}/resolve/tiny.gguf"})
        check(done["state"] == "done", "install through a redirect")
        check((store / "tiny.gguf").read_bytes() == FILES["tiny.gguf"], "downloaded content is exact")
        model = done.get("result", {}).get("model", {})
        check(model.get("sha256") == hashlib.sha256(FILES["tiny.gguf"]).hexdigest(), "announced SHA-256 verified")
        check(model.get("modality") == "text" and model.get("source") == "managed", "installed model catalogued")
        check(call("models_resolve", {"query": "tiny.gguf"}).get("found") is True, "installed model can be resolved")

        # An interrupted download resumes where it stopped.
        half = len(FILES["big.gguf"]) // 2
        (store / "big.gguf.part").write_bytes(FILES["big.gguf"][:half])
        RANGES.clear()
        done = install({"url": f"{base}/files/big.gguf"})
        check(done["state"] == "done", "resumed install")
        check((store / "big.gguf").read_bytes() == FILES["big.gguf"], "resumed content is exact")
        check(("big.gguf", f"bytes={half}-") in RANGES, "the download resumed from the partial file")
        check(not (store / "big.gguf.part").exists(), "partial file removed")

        # A server without ranges, and a destination sub-folder inside the store.
        done = install({"url": f"{base}/norange/plain.gguf", "destination": str(store / "llm"), "filename": "renamed.gguf"})
        check(done["state"] == "done" and (store / "llm" / "renamed.gguf").read_bytes() == FILES["plain.gguf"],
              "install without range support, renamed, in a sub-folder")

        # Refusals leave nothing behind.
        done = install({"url": f"{base}/files/plain.gguf", "sha256": "0" * 64})
        check(done["state"] == "failed" and not (store / "plain.gguf").exists() and
              not (store / "plain.gguf.part").exists(), "wrong SHA-256 refused and cleaned up")
        check(install({"url": f"{base}/files/tiny.gguf"})["state"] == "failed", "existing file is not overwritten")
        check(install({"url": f"{base}/files/missing.gguf"})["state"] == "failed", "missing remote file")
        check("OutsideLocations" in call("install_start", {"url": f"{base}/files/tiny.gguf",
                                                           "destination": str(root / "elsewhere")},
                                         expect_error=True).get("error", ""), "destination outside the locations")
        check("InsecureTransport" in call("install_start", {"url": "http://example.com/model.gguf"},
                                          expect_error=True).get("error", ""), "remote plain HTTP refused")
    finally:
        subprocess.run([cli, "stop"], env=environment, capture_output=True, timeout=60)
        server.shutdown()
        shutil.rmtree(root, ignore_errors=True)
    print("FAILED" if FAILURES else "OK")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())

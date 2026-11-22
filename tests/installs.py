# SPDX-FileCopyrightText: 2026 Cyberalien
# SPDX-License-Identifier: GPL-3.0-only

"""Shared installations, with the real service and a local HTTP server: no Internet access.

    python tests/installs.py <CyModelHubCli> <CyModelHubService>

Several applications ask for the same model at once, one of them gives up, an answer is
lost, the service is stopped or killed in the middle, and complete models made of several
files are installed, interrupted, damaged and completed.
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
import time

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
             text("general.name") + struct.pack("<I", 8) + text(name)]
    head = b"GGUF" + struct.pack("<IQQ", 3, 0, len(pairs)) + b"".join(pairs)
    return head + bytes((index * 31 + len(name)) % 251 for index in range(size - len(head)))


def digest(data):
    return hashlib.sha256(data).hexdigest()


FILES = {name: gguf(name, size) for name, size in {
    "shared.gguf": 120_000, "leave.gguf": 150_000, "gone.gguf": 150_000, "idem.gguf": 30_000,
    "crash.gguf": 400_000, "secret.gguf": 20_000, "query.gguf": 20_000}.items()}
PACKS = {
    "nova": {"weights/model-00001-of-00002.gguf": gguf("Nova Pack", 300_000),
             "weights/model-00002-of-00002.gguf": bytes(range(256)) * 800,
             "config.json": b'{"context": 8192}',
             "tokenizer/tokenizer.json": b'{"tokens": ["a", "b", "c"]}'},
    "holes": {"model.gguf": gguf("Holes", 40_000), "config.json": b'{"holes": true}'},
    "badpack": {"model.gguf": gguf("Bad", 40_000), "config.json": b'{"bad": true}'},
    "resume": {"model.gguf": gguf("Resume", 200_000), "config.json": b'{"resume": true}'},
}
LOCK = threading.Lock()
DATA = {}      # path -> number of requests that fetched content (not the one-byte probe)
RANGES = []    # (path, Range header) of content requests
SEEN_AUTH = set()
GOOD_TOKEN = "GOOD-TOKEN-0123456789"


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *arguments):
        pass

    def do_GET(self):
        path = self.path.split("?")[0]
        kind, _, name = path.strip("/").partition("/")
        if self.headers.get("Authorization"):
            SEEN_AUTH.add(self.headers["Authorization"])
        if kind == "pkg":
            pack, _, member = name.partition("/")
            data = PACKS.get(pack, {}).get(member)
        else:
            data = FILES.get(name)
        if kind == "gated" and self.headers.get("Authorization") != f"Bearer {GOOD_TOKEN}":
            self.send_response(401)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if data is None:
            self.send_error(404)
            return
        requested = self.headers.get("Range")
        start, end = 0, len(data) - 1
        if requested:
            first, _, last = requested.removeprefix("bytes=").partition("-")
            start = int(first)
            end = int(last) if last else len(data) - 1
            if start >= len(data):
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{len(data)}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
        probe = requested == "bytes=0-0"
        if not probe:
            with LOCK:
                DATA[path] = DATA.get(path, 0) + 1
                RANGES.append((path, requested))
        body = data[start:end + 1]
        self.send_response(206 if requested else 200)
        if requested:
            self.send_header("Content-Range", f"bytes {start}-{start + len(body) - 1}/{len(data)}")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if kind != "slow" or probe:
            self.wfile.write(body)
            return
        # A slow source: the download lasts long enough for others to ask for the same model.
        pieces = 30
        step = max(1, len(body) // pieces)
        try:
            for offset in range(0, len(body), step):
                self.wfile.write(body[offset:offset + step])
                self.wfile.flush()
                time.sleep(0.1)
        except OSError:
            pass


def main():
    cli, service = sys.argv[1], sys.argv[2]
    root = pathlib.Path(tempfile.mkdtemp(prefix="cymodelhub-installs-"))
    environment = dict(os.environ, CYMODELHUB_DATA=str(root / "data"), CYMODELHUB_SERVICE=service,
                       HF_HUB_CACHE=str(root / "hf"), OLLAMA_MODELS=str(root / "ollama"),
                       USERPROFILE=str(root / "home"), HOME=str(root / "home"), HF_TOKEN="")
    (root / "home").mkdir()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_address[1]}"
    outputs = []

    def call(operation, arguments=None, expect_error=False):
        result = subprocess.run([cli, "call", operation, json.dumps(arguments or {})], env=environment,
                                capture_output=True, text=True, timeout=120)
        outputs.append(result.stdout + result.stderr)
        if result.returncode != 0:
            if not expect_error:
                raise RuntimeError(f"{operation}: {result.stderr.strip()}")
            return {"error": result.stderr.strip()}
        return json.loads(result.stdout)

    def job_state(job):
        return call("jobs_get", {"id": job})["job"]

    def wait(job, limit=60):
        deadline = time.monotonic() + limit
        while time.monotonic() < deadline:
            state = job_state(job)
            if state["state"] != "running":
                return state
            time.sleep(0.1)
        return {"state": "timeout"}

    def together(count, make):
        answers = [None] * count

        def run(index):
            answers[index] = call("install_start", make(index))

        threads = [threading.Thread(target=run, args=(index,)) for index in range(count)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        return answers

    try:
        store = pathlib.Path(call("hub_status")["managedStore"])

        # --- Several applications ask for the same model at the same moment ---------------------
        answers = together(6, lambda index: {"url": f"{base}/slow/shared.gguf",
                                             **({"destination": str(store / "other")} if index == 0 else {})})
        check(len({answer["job"] for answer in answers}) == 1, "six simultaneous requests share one job")
        check(sum(not answer["joined"] for answer in answers) == 1, "one of them started it, five joined")
        check(len({answer["ticket"] for answer in answers}) == 6 and all(answer["ticket"] for answer in answers),
              "each requester has a ticket of its own")
        check(len({answer["artifact"] for answer in answers}) == 1, "they name the same artifact")
        check(len({answer["destination"] for answer in answers}) == 1 and
              sum(answer["destinationHonored"] for answer in answers) in (1, 5),
              "another destination does not make a second copy: everyone is told the one place")
        done = wait(answers[0]["job"])
        check(done["state"] == "done", "the shared installation completes")
        check(DATA.get("/slow/shared.gguf") == 1, "the model was downloaded once")
        copies = [path for path in store.rglob("shared.gguf*")]
        check(len(copies) == 1 and copies[0].read_bytes() == FILES["shared.gguf"], "one exact copy is on disk")
        check(call("models_list", {"query": "shared"})["total"] == 1, "one catalogue entry")
        again = call("install_start", {"url": f"{base}/slow/shared.gguf", "destination": str(store / "third")})
        check(again["installed"] and again["model"]["path"] == str(copies[0]) and not again["destinationHonored"],
              "asked again for another folder: the installed model is returned, not a copy")
        finished = wait(again["job"])
        check(finished["state"] == "done" and finished["result"].get("reused") is True and
              DATA.get("/slow/shared.gguf") == 1, "its job is already done, and nothing was downloaded")

        # --- One requester gives up: the others are not interrupted ---------------------------
        first = call("install_start", {"url": f"{base}/slow/leave.gguf"})
        second = call("install_start", {"url": f"{base}/slow/leave.gguf"})
        check(second["joined"] and second["job"] == first["job"], "a second requester joins")
        shared = call("jobs_cancel", {"id": first["job"]})
        check(shared == {"cancelled": False, "shared": True, "requesters": 2},
              "a job others wait for is not cancelled by one of them")
        left = call("install_leave", {"job": first["job"], "ticket": first["ticket"]})
        check(left["left"] and not left["cancelled"] and left["requesters"] == 1, "one requester leaves")
        check(job_state(first["job"])["state"] == "running", "the installation goes on for the other")
        check(wait(first["job"])["state"] == "done" and (store / "leave.gguf").read_bytes() == FILES["leave.gguf"],
              "and completes")
        first = call("install_start", {"url": f"{base}/slow/gone.gguf"})
        second = call("install_start", {"url": f"{base}/slow/gone.gguf"})
        call("install_leave", {"job": first["job"], "ticket": first["ticket"]})
        left = call("install_leave", {"job": first["job"], "ticket": second["ticket"]})
        check(left["cancelled"], "when the last requester leaves, the work is cancelled")
        cancelled = wait(first["job"])
        check(cancelled["state"] == "cancelled" and cancelled.get("code") == "Cancelled", "the job ends cancelled")
        check(not (store / "gone.gguf").exists() and call("models_list", {"query": "gone"})["total"] == 0,
              "nothing was published")
        check(call("installs_list")["installs"] == [], "the reservation ends with the job")

        # --- A lost answer: the same request is sent again ---------------------------------------
        request = {"url": f"{base}/files/idem.gguf", "idempotencyKey": "installs-test-key-1"}
        one = call("install_start", request)
        two = call("install_start", request)
        check(not one.get("replayed") and two.get("replayed") and two["job"] == one["job"],
              "sent twice with one key: one job")
        check(wait(one["job"])["state"] == "done", "it completes")
        three = call("install_start", request)
        check(three.get("replayed") and three["outcome"]["state"] == "done" and DATA.get("/files/idem.gguf") == 1,
              "sent again after the end: the outcome, and no second download")
        conflict = call("install_start", {"url": f"{base}/files/shared.gguf", "idempotencyKey": "installs-test-key-1"},
                        expect_error=True)
        check("IdempotencyConflict" in conflict.get("error", ""), "the same key with other arguments is a conflict")
        subprocess.run([cli, "stop"], env=environment, capture_output=True, timeout=60)
        time.sleep(0.5)
        four = call("install_start", request)
        check(four.get("replayed") and four["outcome"]["state"] == "done" and DATA.get("/files/idem.gguf") == 1,
              "after a restart of the service the key is still known")
        check(job_state(one["job"])["state"] == "done", "and its job can still be asked about")

        # --- The service is killed in the middle of a download ----------------------------------
        request = {"url": f"{base}/slow/crash.gguf", "idempotencyKey": "installs-test-key-2"}
        started = call("install_start", request)
        partial = store / "crash.gguf.part"
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and not (partial.exists() and partial.stat().st_size > 50_000):
            time.sleep(0.05)
        check(partial.exists(), "the download is under way")
        pid = json.loads((root / "data" / "service.json").read_text())["pid"]
        if os.name == "nt":
            subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
        else:
            os.kill(pid, 9)
        time.sleep(0.5)
        size = partial.stat().st_size
        check(0 < size < len(FILES["crash.gguf"]), "killed with a partial file on disk")
        pending = call("installs_list")["installs"]
        check(len(pending) == 1 and pending[0]["state"] == "interrupted" and
              pending[0]["target"] == str(store / "crash.gguf"), "the next service finds the unfinished installation")
        check(job_state(started["job"])["state"] == "interrupted", "its job is reported as interrupted")
        RANGES.clear()
        resumed = call("install_start", request)
        check(resumed.get("resumed") and resumed["job"] != started["job"] and
              resumed["destination"] == str(store / "crash.gguf"), "the same request takes the work up again")
        other = call("install_start", {"url": f"{base}/slow/crash.gguf", "destination": str(store / "elsewhere")})
        check(other["joined"] and other["job"] == resumed["job"] and not other["destinationHonored"] and
              other["destination"] == str(store / "crash.gguf"),
              "another application joins it where its partial file is, whatever destination it asks")
        check(wait(resumed["job"])["state"] == "done" and
              (store / "crash.gguf").read_bytes() == FILES["crash.gguf"], "the resumed installation is exact")
        check(("/slow/crash.gguf", f"bytes={size}-") in RANGES, "it continued from the partial file")
        check(call("installs_list")["installs"] == [] and not partial.exists(), "nothing is left behind")

        # --- A complete model made of several files --------------------------------------------
        def member(pack, path, role, hashed=True, sized=True):
            data = PACKS[pack][path]
            entry = {"path": path, "role": role}
            if sized:
                entry["size"] = len(data)
            if hashed:
                entry["sha256"] = digest(data)
            return entry

        commit = "0123456789abcdef0123456789abcdef01234567"
        nova = {"identity": {"name": "Nova Pack", "repository": "acme/nova-pack", "revision": commit, "variant": "q4"},
                "classification": {"format": "gguf", "modality": "text", "architecture": "llama",
                                   "quantization": "Q4_K_M"},
                "entry": "weights/model-00001-of-00002.gguf",
                "files": [member("nova", "weights/model-00001-of-00002.gguf", "weights"),
                          member("nova", "weights/model-00002-of-00002.gguf", "shard"),
                          member("nova", "config.json", "config", hashed=False, sized=False),
                          member("nova", "tokenizer/tokenizer.json", "tokenizer")]}
        request = {"package": nova, "baseUrl": f"{base}/pkg/nova"}
        installed = call("install_start", request)
        done = wait(installed["job"])
        folder = store / "Nova Pack-q4"
        model = done.get("result", {}).get("model", {})
        check(done["state"] == "done" and model.get("state") == "ready" and model.get("complete") is True,
              "complete model installed and announced ready")
        check(all((folder / path).read_bytes() == data for path, data in PACKS["nova"].items()),
              "every file of the model is exact")
        check(not (store / "Nova Pack-q4.incomplete").exists(), "the staging folder became the model")
        check(model.get("integrity") == "recorded" and model.get("revision") == commit and
              model.get("architecture") == "llama", "hashes checked or recorded, identity kept")
        need = {"repository": "acme/nova-pack", "revision": commit, "variant": "q4", "integrity": "hashes",
                "files": [{key: value for key, value in entry.items() if key != "role"} for entry in nova["files"]]}
        found = call("models_check", need)
        check(found["status"] == "ready" and found["root"] == str(folder) and
              found["entry"] == str(folder / "weights" / "model-00001-of-00002.gguf"),
              "the strict check finds it, with its folder and its entry file")
        check(call("install_start", request)["installed"], "asked again: already installed")

        # A member the source does not have: nothing is published, and a later attempt resumes.
        holes = {"identity": {"name": "Holes", "repository": "acme/holes"},
                 "files": [member("holes", "model.gguf", "weights"), member("holes", "config.json", "config"),
                           {"path": "late.bin", "role": "shard"}]}
        failed = wait(call("install_start", {"package": holes, "baseUrl": f"{base}/pkg/holes"})["job"])
        check(failed["state"] == "failed" and failed.get("code") == "DownloadFailed", "a missing member fails the job")
        check((store / "Holes.incomplete" / "model.gguf").exists() and not (store / "Holes").exists() and
              call("models_check", {"repository": "acme/holes"})["status"] == "absent",
              "the incomplete model stays in its staging folder and is not announced")
        PACKS["holes"]["late.bin"] = b"late" * 500
        done = wait(call("install_start", {"package": holes, "baseUrl": f"{base}/pkg/holes"})["job"])
        check(done["state"] == "done" and (store / "Holes" / "late.bin").exists() and
              DATA.get("/pkg/holes/model.gguf") == 1, "the next attempt only fetches what was missing")

        # A member that is not what the contract says.
        bad = {"identity": {"name": "Bad", "repository": "acme/bad"},
               "files": [member("badpack", "model.gguf", "weights"),
                         dict(member("badpack", "config.json", "config", sized=False), sha256="0" * 64)]}
        failed = wait(call("install_start", {"package": bad, "baseUrl": f"{base}/pkg/badpack"})["job"])
        check(failed["state"] == "failed" and failed.get("code") == "HashMismatch" and not (store / "Bad").exists() and
              not list((store / "Bad.incomplete").glob("config.json*")), "a wrong hash is refused and not kept")

        # An installation that was interrupted between two files.
        staging = store / "Resume.incomplete"
        staging.mkdir(parents=True)
        (staging / "config.json").write_bytes(PACKS["resume"]["config.json"])
        half = len(PACKS["resume"]["model.gguf"]) // 2
        (staging / "model.gguf.part").write_bytes(PACKS["resume"]["model.gguf"][:half])
        RANGES.clear()
        resume = {"identity": {"name": "Resume", "repository": "acme/resume"},
                  "files": [member("resume", "model.gguf", "weights"), member("resume", "config.json", "config")]}
        done = wait(call("install_start", {"package": resume, "baseUrl": f"{base}/pkg/resume"})["job"])
        check(done["state"] == "done" and (store / "Resume" / "model.gguf").read_bytes() == PACKS["resume"]["model.gguf"],
              "interrupted complete model: resumed and exact")
        check(("/pkg/resume/model.gguf", f"bytes={half}-") in RANGES and "/pkg/resume/config.json" not in DATA,
              "the partial file continued, the complete one was not fetched again")

        # A model that lost a file is completed where it is; a damaged file is never overwritten.
        (folder / "config.json").unlink()
        check(call("models_check", need)["status"] == "incomplete", "a missing file is seen at once")
        before = DATA.get("/pkg/nova/weights/model-00001-of-00002.gguf")
        done = wait(call("install_start", request)["job"])
        check(done["state"] == "done" and (folder / "config.json").read_bytes() == PACKS["nova"]["config.json"] and
              DATA.get("/pkg/nova/weights/model-00001-of-00002.gguf") == before,
              "the missing file is fetched, the weights are not downloaded again")
        check(call("models_check", need)["status"] == "ready", "ready again")
        tokenizer = folder / "tokenizer" / "tokenizer.json"
        damaged = bytearray(PACKS["nova"]["tokenizer/tokenizer.json"])
        damaged[5] ^= 0xFF
        tokenizer.write_bytes(damaged)
        verified = wait(call("models_verify", {"id": model["id"]})["job"])
        check(verified["result"]["model"]["state"] == "invalid", "verification finds the damaged file")
        failed = wait(call("install_start", request)["job"])
        check(failed["state"] == "failed" and failed.get("code") == "ValidationFailed" and
              tokenizer.read_bytes() == bytes(damaged), "a damaged file is reported, never overwritten")
        check(call("models_check", need)["status"] == "invalid", "and the model stays invalid")

        # Paths that would leave the model or the registered locations.
        for path in ("../evil.bin", "/etc/passwd", "a/../../b.bin"):
            escaping = {"identity": {"name": "Evil", "repository": "acme/evil"}, "files": [{"path": path}]}
            refused = call("install_start", {"package": escaping, "baseUrl": f"{base}/pkg/nova"}, expect_error=True)
            check("InvalidManifest" in refused.get("error", ""), f"package path refused: {path}")
        refused = call("install_start", dict(request, destination=str(root / "outside")), expect_error=True)
        check("OutsideLocations" in refused.get("error", ""), "a package is not installed outside the locations")
        refused = call("install_start", dict(request, filename="../up"), expect_error=True)
        check("InvalidName" in refused.get("error", ""), "a folder name cannot leave the destination")
        check(not (root / "outside").exists() and not (store.parent / "up").exists(), "nothing was written elsewhere")

        # --- Provider access, and credentials that must not leak --------------------------------
        denied = wait(call("install_start", {"url": f"{base}/gated/secret.gguf", "token": "WRONG-TOKEN-987654"})["job"])
        check(denied["state"] == "failed" and denied.get("code") == "ProviderAccessRequired",
              "a refused download says that the provider wants access")
        granted = wait(call("install_start", {"url": f"{base}/gated/secret.gguf", "token": GOOD_TOKEN,
                                              "idempotencyKey": "installs-test-key-3"})["job"])
        check(granted["state"] == "done", "with the right token it installs")
        queried = wait(call("install_start", {"url": f"{base}/files/query.gguf?token=QUERY-SECRET-42&v=1"})["job"])
        check(queried["state"] == "done", "an address with a query installs")
        record = call("models_get", {"id": queried["result"]["model"]["id"]})["model"]
        check(record["url"] == f"{base}/files/query.gguf", "the stored address has no query")
        call("jobs_list")
        call("installs_list")
        everything = "".join(outputs)
        for name in ("catalog.json", "requests.json", "installs.json"):
            path = root / "data" / name
            everything += path.read_text(encoding="utf-8") if path.exists() else ""
        for secret in (GOOD_TOKEN, "WRONG-TOKEN-987654", "QUERY-SECRET-42", "installs-test-key-3"):
            check(secret not in everything, f"{secret[:6]}... appears in no answer and no file of the service")
        check(f"Bearer {GOOD_TOKEN}" in SEEN_AUTH, "the token was sent to its provider")
    finally:
        subprocess.run([cli, "stop"], env=environment, capture_output=True, timeout=60)
        server.shutdown()
        time.sleep(0.3)
        shutil.rmtree(root, ignore_errors=True)
    print("FAILED" if FAILURES else "OK")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())

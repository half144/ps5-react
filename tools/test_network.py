# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""Local native networking integration tests; never access a console or catalog."""
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
DATA = bytes(range(256)) * (16 * 1024 * 1024 // 256)
PIECES = [bytes([19+i]) * size for i, size in enumerate((3*1024*1024+17, 5*1024*1024+31, 7*1024*1024+9))]


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    requests = []
    guard = threading.Lock()
    retried = set()

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            pass  # Expected when the client cancels or rejects a response.

    def log_message(self, *_):
        pass

    def do_HEAD(self):
        self.respond(head=True)

    def do_GET(self):
        self.respond()

    def respond(self, head=False):
        path = self.path
        selected = self.headers.get("Range")
        with self.guard:
            self.requests.append((path, selected, self.headers.get("If-Range")))
        if path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/file")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if path in ("/json", "/not-found", "/large-json"):
            body = b'{"value":42}' if path != "/large-json" else b"x" * 4096
            self.send_response(404 if path == "/not-found" else 200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if not head:
                self.wfile.write(body)
            return
        piece = re.search(r"/piece([0-2])$", path)
        payload = PIECES[int(piece[1])] if piece else DATA
        total = 4*1024**3 + 1024**2 if path == "/large-offset" else len(payload)
        start, end, status = 0, total-1, 200
        if selected and path not in ("/no-range", "/slow-no-range", "/multipart-no-range/piece1"):
            match = re.fullmatch(r"bytes=(\d+)-(\d+)", selected)
            if not match:
                self.send_error(416)
                return
            start, end = map(int, match.groups())
            status = 206
        if (path == "/flaky" and selected != "bytes=0-0") or path == "/flaky-probe":
            with self.guard:
                retry_key = (path, selected)
                first = retry_key not in self.retried
                self.retried.add(retry_key)
            if first:
                self.send_response(503)
                self.send_header("Retry-After", "0")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
        if path == "/changed" and selected != "bytes=0-0":
            status = 200
        self.send_response(status)
        self.send_header("Content-Length", str(end-start+1))
        if path not in ("/no-validator", "/slow-no-validator") and not path.startswith("/multipart-no-validator"):
            self.send_header("ETag", '"fixture-v1"' if path != "/changed" else '"fixture-v2"')
        if status == 206:
            reported = start+1 if path == "/bad-range" and selected != "bytes=0-0" else start
            self.send_header("Content-Range", f"bytes {reported}-{end}/{total}")
        self.end_headers()
        if head:
            return
        try:
            for offset in range(start, end+1, 65536):
                index = offset % len(payload)
                self.wfile.write(payload[index:index+min(65536, end-offset+1)])
                if path.startswith("/slow") or path.startswith("/multipart-slow"):
                    time.sleep(0.006)
                if path == "/truncated" and selected != "bytes=0-0":
                    self.close_connection = True
                    self.connection.shutdown(socket.SHUT_RDWR)
                    return
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass


def compile_client(directory):
    binary = directory / "client"
    subprocess.run(["clang++", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-I", str(ROOT / "native/shared"), str(ROOT / "tools/tests/network_client.cpp"),
                    str(ROOT / "native/shared/network.cpp"), str(ROOT / "native/desktop/network_platform.cpp"),
                    "-lcurl", "-o", str(binary)], check=True)
    return binary


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    origin = f"http://127.0.0.1:{server.server_port}"
    with tempfile.TemporaryDirectory(prefix="ps5-react-network-") as temporary:
        directory = Path(temporary)
        binary = compile_client(directory)

        def run(endpoint, path="-", connections=4, max_bytes=1024*1024, cancel=0, digest="", method="GET", file_limit=0, stop=0, pieces=False, hashes=None, recover=False):
            environment = dict(os.environ)
            environment.pop("NETWORK_TEST_RECOVER", None)
            if recover:
                environment["NETWORK_TEST_RECOVER"] = "1"
            result = subprocess.run([str(binary), (endpoint if endpoint.startswith("https:") else origin+endpoint), str(path), str(connections), str(max_bytes),
                                     str(cancel), digest, method, str(file_limit), str(stop),
                                     "pieces" if pieces else "single", *(hashes or [])], capture_output=True, text=True, timeout=20, env=environment)
            if not result.stdout.strip():
                raise AssertionError(result.stderr or f"client exited {result.returncode}")
            out = json.loads(result.stdout)
            assert out["peakBuffer"] <= 8*1024*1024, out
            assert out["peakConnections"] <= connections, out
            return out

        assert json.loads(run("/json")["body"]) == {"value": 42}
        assert run("/not-found")["status"] == 404
        assert run("/json", method="HEAD")["body"] == ""
        # A published file can be recovered after queue-state loss without contacting its provider.
        path = directory / "published-recovery"
        assert run("/file", path, recover=True)["state"] == "completed"
        before = len(Handler.requests)
        recovered = run("/file", path, recover=True)
        assert recovered["state"] == "completed" and recovered["written"] == len(DATA), recovered
        assert len(Handler.requests) == before
        assert run("/no-range", path, recover=True)["state"] == "failed"  # Different request identity.
        with path.open("r+b") as completed:
            completed.write(b"corrupt")  # Same inode and size still require content verification.
        assert "SHA-256 mismatch" in run("/file", path, recover=True)["error"]
        path = directory / "unowned-final"
        path.write_bytes(DATA)
        assert run("/file", path, recover=True)["state"] == "failed"
        assert path.read_bytes() == DATA
        path = directory / "sequential-recovery"
        assert run("/no-range", path, recover=True)["state"] == "completed"
        assert run("/no-range", path, recover=True)["state"] == "completed"
        for endpoint in ("/slow-no-range", "/slow-no-validator"):
            path = directory / endpoint.removeprefix("/")
            assert run(endpoint, path, cancel=180, recover=True)["state"] == "cancelled"
            assert run(endpoint, path, recover=True)["state"] == "failed"
            # Explicit app restart discards its owned partial, preserving final-file exclusivity.
            Path(str(path)+".part").unlink()
            assert run(endpoint, path, recover=True)["state"] == "completed"
        assert run("/large-json", max_bytes=256)["state"] == "failed"
        assembled = b"".join(PIECES)
        piece_hashes = [hashlib.sha1(piece).hexdigest() for piece in PIECES]
        for endpoint in ("/multipart", "/multipart-no-range", "/multipart-no-validator"):
            path = directory / endpoint.removeprefix("/")
            out = run(endpoint, path, pieces=True, hashes=piece_hashes, digest=hashlib.sha256(assembled).hexdigest(), recover=True)
            assert out["state"] == "completed", out
            assert path.read_bytes() == assembled
            assert out["written"] == len(assembled), out
            before = len(Handler.requests)
            assert run(endpoint, path, pieces=True, hashes=piece_hashes, digest=hashlib.sha256(assembled).hexdigest(), recover=True)["state"] == "completed"
            assert len(Handler.requests) == before
        path = directory / "multipart-sha1-error"
        out = run("/multipart", path, pieces=True, hashes=["0"*40, *piece_hashes[1:]])
        assert out["state"] == "failed" and "SHA-1 mismatch" in out["error"], out
        assert not path.exists()
        path = directory / "multipart-resume"
        assert run("/multipart-slow", path, pieces=True, hashes=piece_hashes, cancel=180)["state"] == "cancelled"
        before = len(Handler.requests)
        out = run("/multipart-slow", path, pieces=True, hashes=piece_hashes)
        assert out["state"] == "completed", out
        assert path.read_bytes() == assembled
        assert len([r for r in Handler.requests[before:] if r[1] != "bytes=0-0"]) < 18
        for endpoint in ("/file", "/redirect", "/no-range", "/no-validator"):
            path = directory / endpoint.removeprefix("/")
            out = run(endpoint, path)
            assert out["state"] == "completed", out
            assert hashlib.sha256(path.read_bytes()).digest() == hashlib.sha256(DATA).digest()
            assert out["written"] == len(DATA), out
            assert not Path(str(path)+".part").exists()
            path.unlink()
        for endpoint in ("/bad-range", "/changed", "/truncated"):
            path = directory / endpoint.removeprefix("/")
            out = run(endpoint, path)
            assert out["state"] == "failed", out
            assert not path.exists()
        path = directory / "large-offset"
        size = 4*1024**3 + 1024**2
        # Seed completed sparse ranges to exercise the actual downloader beyond 4 GiB
        # without transferring or allocating gigabytes on the development machine.
        with Path(str(path)+".part").open("wb") as partial:
            partial.truncate(4*1024**3)
        identity = hashlib.sha256((origin+"/large-offset"+'\0"fixture-v1"\0\0').encode()).hexdigest().encode()+b"\0"
        meta = struct.pack("<8sQQI65s3x", b"P5RDL001", size, 1024**2, 4097, identity)
        Path(str(path)+".resume").write_bytes(meta + b"\1"*4096 + b"\0")
        assert run("/large-offset", path)["state"] == "completed"
        assert path.stat().st_size == size
        with path.open("rb") as file:
            file.seek(4*1024**3)
            assert file.read() == DATA[:1024**2]
        path.unlink()
        path = directory / "flaky"
        before = len(Handler.requests)
        flaky = run("/flaky", path)
        assert flaky["state"] == "completed", flaky
        assert flaky["retries"] == 16, flaky
        assert path.read_bytes() == DATA
        assert len(Handler.requests[before:]) == 33  # Probe plus two attempts per range.
        path.unlink()
        path = directory / "flaky-probe"
        probe_retry = run("/flaky-probe", path)
        assert probe_retry["state"] == "completed" and probe_retry["retries"] == 17, probe_retry
        path.unlink()
        path = directory / "disk-error"
        failed = run("/file", path, file_limit=512*1024)
        assert failed["state"] == "failed" and "write partial file" in failed["error"], failed
        assert not path.exists()
        path = directory / "shutdown"
        assert run("/slow", path, stop=180)["state"] == "stopped"
        assert run("/slow", path)["state"] == "completed"
        assert path.read_bytes() == DATA
        path.unlink()
        path = directory / "resume"
        cancelled = run("/slow", path, cancel=180)
        assert cancelled["state"] == "cancelled", cancelled
        assert Path(str(path)+".resume").exists()
        before = len(Handler.requests)
        resumed = run("/slow", path)
        assert resumed["state"] == "completed", resumed
        assert path.read_bytes() == DATA
        requests = Handler.requests[before:]
        assert len([item for item in requests if item[1] != "bytes=0-0"]) < 16, requests
        assert all(item[2] == '"fixture-v1"' for item in requests if item[1] != "bytes=0-0")
        path.unlink()
        path = directory / "hash"
        assert run("/file", path, digest="0"*64)["state"] == "failed"
        assert not path.exists()
        # A complete but hash-mismatched partial can be retried with the same expected identity;
        # it must never be silently promoted to a destination file.
        assert run("/file", path, digest="0"*64)["state"] == "failed"
        path = directory / "correct-hash"
        assert run("/file", path, digest=hashlib.sha256(DATA).hexdigest())["state"] == "completed"
        path.unlink()
        certificate, key = directory / "certificate.pem", directory / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(key), "-out", str(certificate), "-days", "1",
                        "-subj", "/CN=localhost"], check=True, capture_output=True)
        tls = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)
        tls.socket = context.wrap_socket(tls.socket, server_side=True)
        threading.Thread(target=tls.serve_forever, daemon=True).start()
        assert run(f"https://127.0.0.1:{tls.server_port}/json")["state"] == "failed"
        tls.shutdown()
        existing = directory / "existing"
        existing.write_bytes(b"preserve")
        assert run("/file", existing)["state"] == "failed"
        assert existing.read_bytes() == b"preserve"
        print("Native networking: HTTP methods/status/limits, exact ranges, bounded buffering, redirects, "
              "sequential fallbacks, truncation, 64-bit offsets, retries, cancellation/resume, hashes, TLS verification "
              "and destination preservation passed.")
    server.shutdown()


if __name__ == "__main__":
    main()

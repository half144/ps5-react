# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""Drive a running desktop preview, as a browser automation tool drives a page.

  ps5_drive.py start --app-dir <dir>    build and launch the preview with its control channel
  ps5_drive.py press down               up/down/left/right/confirm/back/triangle/square/l1/r1/l2/r2
  ps5_drive.py focus <focusKey>         move focus to an element by key
  ps5_drive.py snapshot                 focusable elements as JSON: focusKey, text, rect, focused
  ps5_drive.py shot <path.bmp>          the frame on screen, 960×540
  ps5_drive.py logs                     output printed since the last call
  ps5_drive.py wait <ms>                return after that many milliseconds of frames
  ps5_drive.py stop
"""
import fcntl
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cli import desktop, preview_env  # noqa: E402
from common import ROOT, app_config  # noqa: E402

SOCKET = os.environ.get("PS5_DRIVE_SOCKET", "/tmp/ps5-drive.sock")
PID = Path(SOCKET + ".pid")
LOG = Path(SOCKET + ".log")


def send(line, timeout=30):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout)
        client.connect(SOCKET)
        client.sendall((line + "\n").encode())
        chunks = []
        while chunk := client.recv(65536):
            chunks.append(chunk)
    return b"".join(chunks).decode(errors="replace").rstrip("\n")


def start(app):
    stop()
    app_config(app)
    with open(ROOT / ".build/build.lock", "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        command, directory = desktop(app)
    env = {**preview_env(app), "PS5_REACT_CONTROL": SOCKET}
    with open(LOG, "w") as log:
        process = subprocess.Popen([str(part) for part in command], cwd=directory, env=env, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)
    PID.write_text(str(process.pid))
    # The app's bundle loads after the window opens; the channel answers once frames run.
    for _ in range(600):
        if process.poll() is not None:
            raise SystemExit(f"Preview exited ({process.returncode}); see {LOG}")
        try:
            send("wait 0", timeout=2)
            return print(f"ready: {SOCKET} (pid {process.pid}, output in {LOG})")
        except OSError:
            time.sleep(0.2)
    raise SystemExit("Preview did not answer within 120 s")


def stop():
    if PID.exists():
        try:
            os.kill(int(PID.read_text()), signal.SIGTERM)
        except (ProcessLookupError, ValueError):
            pass
        PID.unlink()


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        return print(__doc__)
    if args[0] == "start":
        if len(args) != 3 or args[1] != "--app-dir":
            raise SystemExit("Usage: ps5_drive.py start --app-dir <dir>")
        return start(Path(args[2]).expanduser().resolve())
    if args[0] == "stop":
        return stop()
    try:
        print(send(" ".join(args)))
    except OSError as error:
        raise SystemExit(f"No preview on {SOCKET} ({error}); run: ps5_drive.py start --app-dir <dir>")


if __name__ == "__main__":
    main()

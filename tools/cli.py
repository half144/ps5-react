# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""Developer entry point. Build and preview only; no console communication."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

from common import ROOT, app_config, bundle, dependency, run


def desktop(name, test=False):
    er = dependency("embeddedReact")
    _, _, generated = bundle(name, er)
    build = ROOT / ".build" / name / "desktop"
    quickjs = dependency("quickjsSource")
    run(["cmake", "-S", ROOT / "native/desktop", "-B", build,
         f"-DER_ROOT={er}", f"-DFETCHCONTENT_SOURCE_DIR_QUICKJS={quickjs}", f"-DAPP_GENERATED={generated}", "-DCMAKE_BUILD_TYPE=Release"],
        log=build / "configure.log")
    run(["cmake", "--build", build, "--target", "ps5-react-preview", "-j", "6"], log=build / "build.log")
    command = [build / "ps5-react-preview", generated / "app.bundle.js"]
    if test:
        run(["node", "--test", *sorted((ROOT / "tools/tailwind").glob("*.test.mjs"))], env={**os.environ, "PS5_REACT_ER": str(er)},
            log=ROOT / ".build/tailwind-test.log")
        if name != "starter":
            raise ValueError("The scripted UI test belongs to starter; use preview for other apps")
        command.append("--self-test")
    return command, build


def doctor():
    print("PS5 React 0.1 — macOS developer environment")
    missing = []
    for command in ("python3", "node", "npm", "cmake", "git", "clang++"):
        path = shutil.which(command)
        print(f"{'OK' if path else 'MISSING'} {command}: {path or 'install required'}")
        if not path: missing.append(command)
    print("PS5 toolchain: LLVM 18 and the pinned SDK are needed only for npm run build")
    print("Preview: SDL2/OpenGL; install with brew install cmake sdl2 llvm@18")
    print("Assets: python3 -m pip install -r requirements.txt")
    if sys.version_info < (3, 12): missing.append("Python 3.12+")
    try:
        import PIL
        print(f"OK Pillow {PIL.__version__}")
    except ImportError:
        missing.append("Pillow")
    if missing: raise RuntimeError("Missing: " + ", ".join(missing))


def source_stamp(name):
    entries = []
    for base in (ROOT / "apps" / name, ROOT / "runtime", ROOT / "native"):
        for path in sorted(base.rglob("*")):
            if path.name.startswith(".") or not path.is_file(): continue
            try:
                stat = path.stat()
                entries.append((str(path), stat.st_mtime_ns, stat.st_size))
            except FileNotFoundError:
                pass  # Editors may atomically rename files during a save.
    return tuple(entries)


def stop_preview(process):
    if process and process.poll() is None:
        process.terminate()  # SDL handles SIGTERM as a quit event.
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def watch(name, command, directory):
    process = subprocess.Popen([str(x) for x in command], cwd=directory)
    stamp = source_stamp(name)
    print("Watching JSX/assets/config. Save to rebuild; Esc closes; Ctrl+C stops.", flush=True)
    try:
        while True:
            if process and process.poll() is not None:
                if process.returncode == 0: return
                print("Preview exited with an error; edit and save to retry.", flush=True)
                process = None
            time.sleep(0.25)
            changed = source_stamp(name)
            if changed == stamp: continue
            # Debounce until the editor has finished its save/rename sequence.
            time.sleep(0.25)
            stamp = source_stamp(name)
            stop_preview(process)
            process = None
            print("Source changed — rebuilding…", flush=True)
            try:
                with open(ROOT / ".build/build.lock", "w") as lock:
                    fcntl.flock(lock, fcntl.LOCK_EX)
                    command, directory = desktop(name)
                process = subprocess.Popen([str(x) for x in command], cwd=directory)
                print("Preview restarted.", flush=True)
            except (ValueError, RuntimeError, subprocess.CalledProcessError) as error:
                print(f"{error}\nFix the source and save to retry.", flush=True)
    finally:
        stop_preview(process)


def create(name, title):
    destination = ROOT / "apps" / name
    if destination.exists(): raise ValueError(f"App already exists: {destination}")
    # Validate identity before touching the new app directory.
    config = json.loads((ROOT / "apps/starter/app.json").read_text())
    config.update(titleId=title, contentId=f"UP9000-{title}_00-PS5REACTSTARTER1", name=name)
    app_config(name, config)
    stage = ROOT / ".build" / ("create-" + name)
    if stage.exists(): raise ValueError(f"Staging directory exists: {stage}")
    shutil.copytree(ROOT / "apps/starter", stage)
    (stage / "app.json").write_text(json.dumps(config, indent=2) + "\n")
    destination.parent.mkdir(exist_ok=True)
    stage.rename(destination)
    print(f"Created {destination}\nNext: npm run preview -- --app {name}")


def main():
    parser = argparse.ArgumentParser(description="React JSX → desktop preview or independent PS5 title")
    parser.add_argument("command", choices=("doctor", "preview", "build", "test", "create"))
    parser.add_argument("--app", default="starter")
    parser.add_argument("--name", help="new app folder name")
    parser.add_argument("--title-id", help="unique PPSAxxxxx ID for a new app")
    parser.add_argument("--sdk", type=Path, help="use an existing public PS5 payload SDK")
    parser.add_argument("--watch", action="store_true", help="rebuild/restart desktop preview when sources change")
    args = parser.parse_args()
    if args.command == "doctor": return doctor()
    if args.command == "create":
        if not args.name or not args.title_id: raise ValueError("Use npm run create -- --name my-app --title-id PPSA99054")
        # Prevent traversals before creating directories.
        import re
        if not re.fullmatch(r"[a-z][a-z0-9-]*", args.name): raise ValueError("name must use lowercase letters/numbers/hyphens")
    else:
        app_config(args.app)  # Fast actionable config errors before downloads/builds.
    (ROOT / ".build").mkdir(exist_ok=True)
    # The asset tooling and caches are shared; serialize build mutations.
    command = None
    with open(ROOT / ".build/build.lock", "w") as lock:
        print("Waiting for build lock…", flush=True)
        fcntl.flock(lock, fcntl.LOCK_EX)
        if args.command == "create":
            return create(args.name, args.title_id)
        if args.command == "build":
            cmd = ["python3", ROOT / "tools/build_ps5.py", "--app", args.app]
            sdk = args.sdk or os.environ.get("PS5_PAYLOAD_SDK")
            if sdk: cmd.extend(["--payload-sdk", sdk])
            run(cmd)
        else:
            command, directory = desktop(args.app, args.command == "test")
    if command:
        if args.watch and args.command == "preview":
            watch(args.app, command, directory)
        else:
            run(command, cwd=directory)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("Preview stopped.")
    except (ValueError, RuntimeError, FileNotFoundError, subprocess.CalledProcessError) as error:
        print(f"PS5 React: {error}", file=sys.stderr)
        sys.exit(1)

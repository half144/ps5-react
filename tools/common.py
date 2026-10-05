# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""Shared configuration and pinned dependency handling. No console transport."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
DEPS = ROOT / ".deps"
LOCK = json.loads((ROOT / "dependencies.lock.json").read_text())


def run(command, env=None, log=None, cwd=None):
    print("→ " + " ".join(str(x) for x in command[:3]), flush=True)
    if log:
        Path(log).parent.mkdir(parents=True, exist_ok=True)
        with open(log, "w") as output:
            result = subprocess.run([str(x) for x in command], env=env, cwd=cwd,
                                    stdout=output, stderr=subprocess.STDOUT)
        if result.returncode:
            print(Path(log).read_text()[-6000:])
            raise RuntimeError(f"Failed. Full log: {log}")
    else:
        subprocess.run([str(x) for x in command], env=env, cwd=cwd, check=True)


def digest(path):
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify(path, expected):
    if digest(path) != expected:
        raise RuntimeError(f"Checksum mismatch: {path}")


def fetch(url, path, expected):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        print(f"Downloading {path.name}", flush=True)
        temporary = path.with_suffix(path.suffix + ".download")
        with urllib.request.urlopen(url, timeout=120) as source, open(temporary, "wb") as output:
            while chunk := source.read(1024 * 1024): output.write(chunk)
        verify(temporary, expected)
        temporary.replace(path)
    verify(path, expected)
    return path


def checkout(path, url, revision):
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", url, path])
        run(["git", "checkout", "--detach", revision], cwd=path)
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=path, text=True).strip()
    if actual != revision:
        raise RuntimeError(f"Dependency at {actual}, expected {revision}: {path}")
    run(["git", "diff", "--exit-code", "HEAD", "--"], cwd=path,
        log=ROOT / ".build/dependency-check.log")


def dependency(name, override=None):
    item = LOCK[name]
    path = Path(override).resolve() if override else DEPS / name
    checkout(path, item["url"], item["revision"])
    return path


def app_config(name, config=None):
    if not re.fullmatch(r"[a-z][a-z0-9-]*", name):
        raise ValueError("App name must use lowercase letters, numbers and hyphens")
    app = ROOT / "apps" / name
    if config is None:
        config = json.loads((app / "app.json").read_text())
    title = config["titleId"]
    if not re.fullmatch(r"PPSA\d{5}", title):
        raise ValueError("titleId must look like PPSA99053")
    if not re.fullmatch(r"[A-Z]{2}\d{4}-" + title + r"_00-[A-Z0-9]{16}", config["contentId"]):
        raise ValueError("contentId needs the same titleId and exactly 16 suffix characters")
    if not re.fullmatch(r"\d{2}\.\d{3}\.\d{3}", config["version"]):
        raise ValueError("version must look like 01.000.000")
    for field in ("render", "surface"):
        for axis in ("width", "height"):
            value = config[field][axis]
            if type(value) is not int or not 1 <= value <= 3840:
                raise ValueError(f"{field}.{axis} must be an integer between 1 and 3840")
    if type(config["timeoutSeconds"]) is not int or not 0 <= config["timeoutSeconds"] <= 3600:
        raise ValueError("timeoutSeconds must be 0 (disabled) or 1..3600")
    if not config["name"].strip() or len(config["name"]) > 80:
        raise ValueError("name must contain 1..80 characters")
    for other in (ROOT / "apps").glob("*/app.json"):
        if other.parent != app and json.loads(other.read_text())["titleId"] == title:
            raise ValueError(f"titleId already belongs to {other.parent.name}")
    return app, config


def generated_config(config, directory):
    directory.mkdir(parents=True, exist_ok=True)
    definitions = {"WIDTH": config["render"]["width"], "HEIGHT": config["render"]["height"],
                   "SURFACE_WIDTH": config["surface"]["width"], "SURFACE_HEIGHT": config["surface"]["height"],
                   "TIMEOUT": config["timeoutSeconds"], "NAME": config["name"], "TITLE": config["titleId"]}
    (directory / "app_config.hpp").write_text("#pragma once\n" + "".join(
        f"#define PS5_REACT_{key} {json.dumps(value)}\n" for key, value in definitions.items()))


def bundle(name, er):
    app, config = app_config(name)
    output = ROOT / ".build" / name / "generated"
    generated_config(config, output)
    package = er / "bridges/quickjs/js"
    if not (package / "node_modules").exists():
        run(["npm", "ci", "--omit=optional"], cwd=package, log=ROOT / ".build/npm-upstream.log")
    run(["node", ROOT / "tools/bundle.mjs", er, app, output], log=ROOT / ".build" / name / "bundle.log")
    return app, config, output

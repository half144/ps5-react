# Copyright (C) 2026 half144 and PS5 React contributors
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional attribution term: see LICENSE-ATTRIBUTION.
"""rar-extract (native/rar_worker): RarLab's UnRAR library and a small MIT-licensed driver, built as a
program of its own. UnRAR's license allows its use to extract RAR archives but is not GPL-compatible,
so it is never linked into the engine; the engine starts this program and reads its reports.
"""
import subprocess
import tarfile
from concurrent.futures import ThreadPoolExecutor

from common import DEPS, LOCK, ROOT, digest, fetch

# The library objects of UnRAR's makefile, without its command line front end and unrar.dll layer.
SOURCES = ("strlist strfn pathfn smallfn global file filefn filcreat archive arcread unicode system crypt "
           "crc rawread encname resource match timefn rdwrfn consio options errhnd rarvm secpassword "
           "rijndael getbits sha1 sha256 blake2s hash extinfo extract volume list find unpack headers "
           "threadpool rs16 cmddata ui largepage filestr scantree qopen").split()
DEFINES = ["-DRARDLL", "-DSILENT", "-DRAR_SMP", "-DUNRAR_UTF8_NAMES", "-D_FILE_OFFSET_BITS=64", "-D_LARGEFILE_SOURCE"]


def unrar_source():
    """The pinned UnRAR source tree with the repository's patches applied."""
    pinned = LOCK["unrarSource"]
    root = DEPS / f"unrar-{pinned['version']}"
    marker = root / ".patched"
    patches = [ROOT / p for p in pinned["patches"]]
    expected = "\n".join(digest(p) for p in patches) + "\n"
    if not marker.exists() or marker.read_text() != expected:
        archive = fetch(pinned["url"], DEPS / f"unrarsrc-{pinned['version']}.tar.gz", pinned["sha256"])
        root.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive) as package:
            for member in package.getmembers():
                member.name = member.name.removeprefix("unrar/")
                if member.isfile() and "/" not in member.name:
                    package.extract(member, root, filter="data")
        for patch in patches:
            subprocess.run(["patch", "-p1", "--forward", "--quiet", "-i", str(patch)], cwd=root, check=True)
        marker.write_text(expected)
    return root


def build(output, compiler, flags=(), libraries=(), env=None):
    """Compiles UnRAR and the driver with `compiler` (a command list) and links `output`."""
    source = unrar_source()
    objects = output.parent / (output.name + ".obj")
    objects.mkdir(parents=True, exist_ok=True)
    common = [*compiler, "-O2", "-fexceptions", *DEFINES, *flags]

    def compile_one(item):
        path, extra = item
        obj = objects / (path.stem + ".o")
        # UnRAR's headers are a system include for the driver, which builds with the engine's warnings.
        result = subprocess.run([*map(str, common), *extra, "-isystem", str(source), "-c", str(path), "-o", str(obj)],
                                capture_output=True, text=True, env=env)
        if result.returncode:
            raise RuntimeError(f"{path.name}: {result.stderr[-4000:]}")
        return obj

    library = ["-std=c++17", "-w"]
    driver = ["-std=c++20", "-Wall", "-Wextra", "-Werror"]
    items = [(source / f"{name}.cpp", library) for name in SOURCES] + [(ROOT / "native/rar_worker/rar_worker.cpp", driver)]
    with ThreadPoolExecutor(8) as pool:
        built = list(pool.map(compile_one, items))
    result = subprocess.run([*map(str, compiler), *map(str, flags), *map(str, built), *map(str, libraries), "-o", str(output)],
                            capture_output=True, text=True, env=env)
    if result.returncode:
        raise RuntimeError(f"rar-extract link: {result.stderr[-4000:]}")
    return output


def build_host(output):
    """rar-extract for the desktop preview and the archive tests."""
    return build(output, ["clang++"], ["-pthread"])

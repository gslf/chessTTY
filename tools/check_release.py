#!/usr/bin/env python3
"""Test the extracted release with no build-tool PATH or library overrides."""
import argparse
import os
from pathlib import Path
import platform
import re
import subprocess
import tarfile
import tempfile
import zipfile


def run(command, cwd, env, timeout=180):
    result = subprocess.run([str(x) for x in command], cwd=cwd, env=env,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=timeout)
    print(result.stdout, end="", flush=True)
    if result.returncode:
        raise RuntimeError(f"{command[0]} exited with {result.returncode}")
    return result.stdout


def check(folder, version):
    windows = platform.system() == "Windows"
    suffix = ".exe" if windows else ""
    binary = folder / ("chesstty" + suffix)
    engine = folder / "engine" / ("stockfish" + suffix)
    for path in (binary, engine, folder / "LICENSE", folder / "README.md",
                 folder / "engine/STOCKFISH-COPYING.txt", folder / "engine/STOCKFISH-AUTHORS.txt",
                 folder / "data/openings/COPYING.txt", folder / "examples/immortal-games.pgn"):
        if not path.is_file() or not path.stat().st_size:
            raise RuntimeError(f"Missing/empty package file: {path}")
    if windows and not (folder / "third-party-licenses").is_dir():
        raise RuntimeError("Missing static-library license notices")
    if platform.system() == "Darwin":
        for path in (binary, engine):
            # -verify_arch consumes all following arguments as architecture names.
            subprocess.run(["lipo", str(path), "-verify_arch", "arm64", "x86_64"], check=True)
            subprocess.run(["codesign", "--verify", "--strict", "--all-architectures", str(path)], check=True)
            deps = subprocess.check_output(["otool", "-L", str(path)], text=True)
            print(deps)
            for line in deps.splitlines()[1:]:
                if line[:1].isspace() and line.strip() and not line.lstrip().startswith(("/usr/lib/", "/System/Library/")):
                    raise RuntimeError(f"Non-system macOS dependency: {line}")
    env = os.environ.copy()
    if windows:
        # Unlike os.environ on Windows, its plain dict copy is case-sensitive.
        env = {key.upper(): value for key, value in env.items()}
    for key in list(env):
        if key.upper().startswith(("LD_", "DYLD_", "CHESSTTY_")) or key.upper() in (
                "CURL_CA_BUNDLE", "SSL_CERT_FILE", "SSL_CERT_DIR", "OPENSSL_CONF", "OPENSSL_MODULES"):
            del env[key]
    if windows:
        if not env.get("SYSTEMROOT"):
            raise RuntimeError("Windows environment is missing SYSTEMROOT")
        env["PATH"] = str(Path(env["SYSTEMROOT"]) / "System32")
    else:
        env["PATH"] = "/usr/bin:/bin"
    # A different directory catches accidental engine/data lookup in the checkout.
    with tempfile.TemporaryDirectory(prefix="chesstty unrelated cwd ") as work:
        out = run([binary, "--version"], work, env)
        if not out.splitlines() or out.splitlines()[0] != f"ChessTTY {version}":
            raise RuntimeError("Packaged version does not match the release")
    print(f"Package files and application startup verified (engine not executed): {folder.name} on {platform.system()} {platform.machine()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="chesstty extracted release ") as work:
        target = Path(work)
        # Packages produced by our build must contain one ordinary top-level directory.
        opener = zipfile.ZipFile if args.archive.suffix == ".zip" else tarfile.open
        with opener(args.archive) as archive:
            names = archive.namelist() if isinstance(archive, zipfile.ZipFile) else archive.getnames()
            for name in names:
                if name.startswith(("/", "\\")) or re.match(r"^[A-Za-z]:", name) or ".." in name.replace("\\", "/").split("/"):
                    raise RuntimeError(f"Unsafe archive member: {name}")
            if isinstance(archive, tarfile.TarFile):
                if any(not (member.isfile() or member.isdir()) for member in archive.getmembers()):
                    raise RuntimeError("Release archive contains special files or links")
            archive.extractall(target)
        roots = list(target.iterdir())
        if len(roots) != 1 or not roots[0].is_dir():
            raise RuntimeError("Expected one top-level release directory")
        check(roots[0], args.version)


if __name__ == "__main__":
    main()

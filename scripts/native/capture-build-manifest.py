#!/usr/bin/env python3
"""Create and validate W00 native build state, manifests, and test summaries."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Dict, List, Optional


def run(command: List[str], cwd: Optional[pathlib.Path] = None) -> str:
    completed = subprocess.run(
        command,
        cwd=str(cwd) if cwd else None,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return completed.stdout.strip()


def first_line(command: List[str]) -> str:
    try:
        return run(command).splitlines()[0]
    except (OSError, subprocess.CalledProcessError, IndexError):
        return "unavailable"


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def repository_state(repo: pathlib.Path) -> Dict[str, Any]:
    commit = run(["git", "rev-parse", "HEAD"], repo)
    status_output = run(
        ["git", "status", "--porcelain=v1", "--untracked-files=all"], repo
    )
    return {
        "commit": commit,
        "dirty": bool(status_output),
        "status": status_output.splitlines(),
    }


def atomic_json(path: pathlib.Path, data: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=str(path.parent), delete=False
    ) as stream:
        json.dump(data, stream, indent=2, sort_keys=True)
        stream.write("\n")
        temporary = pathlib.Path(stream.name)
    os.replace(str(temporary), str(path))


def command_fingerprint(args: argparse.Namespace) -> str:
    repo = pathlib.Path(args.repo).resolve()
    profile_file = pathlib.Path(args.profile_file).resolve()
    digest = hashlib.sha256()

    def add(label: str, value: bytes) -> None:
        digest.update(label.encode("utf-8") + b"\0" + value + b"\0")

    add("commit", run(["git", "rev-parse", "HEAD"], repo).encode())
    add(
        "status",
        subprocess.run(
            ["git", "status", "--porcelain=v1", "-z", "--untracked-files=all"],
            cwd=str(repo),
            check=True,
            stdout=subprocess.PIPE,
        ).stdout,
    )
    add(
        "tracked-diff",
        subprocess.run(
            ["git", "diff", "--binary", "HEAD"],
            cwd=str(repo),
            check=True,
            stdout=subprocess.PIPE,
        ).stdout,
    )
    untracked = run(
        ["git", "ls-files", "--others", "--exclude-standard"], repo
    ).splitlines()
    for relative in sorted(untracked):
        path = repo / relative
        add("untracked-name", relative.encode())
        if path.is_file():
            add("untracked-content", path.read_bytes())
    add("profile", profile_file.read_bytes())
    add("compiler", args.compiler.encode())
    add("compiler-version", first_line([args.compiler, "--version"]).encode())
    for key in ("env_cc", "env_cflags", "env_cppflags", "env_ldflags"):
        add(key, getattr(args, key).encode())
    for configure_arg in args.configure_arg:
        add("configure-arg", configure_arg.encode())
    return digest.hexdigest()


def command_write_state(args: argparse.Namespace) -> int:
    atomic_json(
        pathlib.Path(args.path),
        {
            "schema_version": 1,
            "kind": "php-native-w00-configure-state",
            "fingerprint": args.fingerprint,
            "profile": args.profile,
            "repository_commit": args.commit,
            "source_fingerprint": args.source_fingerprint,
            "configured_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        },
    )
    return 0


def command_state_matches(args: argparse.Namespace) -> int:
    try:
        with pathlib.Path(args.path).open(encoding="utf-8") as stream:
            state = json.load(stream)
    except (OSError, ValueError, TypeError):
        return 1
    return 0 if state.get("fingerprint") == args.fingerprint else 1


def make_variable(makefile: pathlib.Path, name: str) -> str:
    pattern = re.compile(r"^" + re.escape(name) + r"\s*=\s*(.*)$")
    try:
        for line in makefile.read_text(encoding="utf-8", errors="replace").splitlines():
            match = pattern.match(line)
            if match:
                return match.group(1).strip()
    except OSError:
        pass
    return "unavailable"


def command_write_manifest(args: argparse.Namespace) -> int:
    repo = pathlib.Path(args.repo).resolve()
    binary = pathlib.Path(args.binary).resolve()
    makefile = pathlib.Path(args.build_dir) / "Makefile"
    compiler_path = shutil.which(args.compiler) or args.compiler
    manifest = {
        "schema_version": 1,
        "kind": "php-native-w00-build-manifest",
        "reproducibility": "command-and-environment-record; not a bit-identical-build claim",
        "repository": repository_state(repo),
        "worktree": {
            "canonical_path": str(repo),
            "base_commit": args.base_commit,
            "id": args.worktree_id,
        },
        "profile": {
            "name": args.profile,
            "build_type": args.build_type,
            "thread_safety": args.thread_safety,
            "sanitizer": args.sanitizer,
        },
        "configure": {
            "command": [args.configure_program] + args.configure_arg,
            "flags": args.configure_arg,
            "fingerprint": args.fingerprint,
            "compiler_flags": {
                "cflags": make_variable(makefile, "CFLAGS"),
                "cflags_clean": make_variable(makefile, "CFLAGS_CLEAN"),
                "extra_cflags": make_variable(makefile, "EXTRA_CFLAGS"),
                "cppflags": make_variable(makefile, "CPPFLAGS"),
                "ldflags": make_variable(makefile, "LDFLAGS"),
                "extra_ldflags": make_variable(makefile, "EXTRA_LDFLAGS"),
            },
            "environment_overrides": {
                "CC": os.environ.get("CC", ""),
                "CFLAGS": os.environ.get("CFLAGS", ""),
                "CPPFLAGS": os.environ.get("CPPFLAGS", ""),
                "LDFLAGS": os.environ.get("LDFLAGS", ""),
            },
        },
        "toolchain": {
            "compiler_path": str(pathlib.Path(compiler_path).resolve()),
            "compiler_version": first_line([args.compiler, "--version"]),
            "autoconf_version": first_line(["autoconf", "--version"]),
            "make_version": first_line(["make", "--version"]),
        },
        "host": {
            "system": platform.system(),
            "release": platform.release(),
            "architecture": platform.machine(),
            "platform": platform.platform(),
        },
        "runtime": {
            "php_debug": args.runtime_debug == "1",
            "php_zts": args.runtime_zts == "1",
            "opcache_loaded": args.opcache_loaded == "1",
        },
        "binary": {
            "path": str(binary),
            "sha256": sha256_file(binary),
        },
        "build": {
            "jobs": args.jobs,
            "source_date_epoch": os.environ.get("SOURCE_DATE_EPOCH", ""),
            "completed_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        },
    }
    atomic_json(pathlib.Path(args.path), manifest)
    return 0


def command_write_summary(args: argparse.Namespace) -> int:
    results: List[Dict[str, str]] = []
    counts: Dict[str, int] = {}
    result_path = pathlib.Path(args.results)
    if result_path.is_file():
        for line in result_path.read_text(encoding="utf-8", errors="replace").splitlines():
            status, separator, test = line.partition("\t")
            if not separator:
                continue
            results.append({"status": status, "test": test})
            counts[status] = counts.get(status, 0) + 1
    failing_statuses = {"BORKED", "FAILED", "LEAKED", "WARNED", "XLEAKED"}
    phpt_failed = any(item["status"] in failing_statuses for item in results)
    success = (
        args.version_exit == 0
        and args.modules_exit == 0
        and args.phpt_exit == 0
        and not phpt_failed
        and not args.sanitizer_diagnostics
    )
    atomic_json(
        pathlib.Path(args.path),
        {
            "schema_version": 1,
            "kind": "php-native-w00-smoke-summary",
            "profile": args.profile,
            "binary": str(pathlib.Path(args.binary).resolve()),
            "sanitizer": args.sanitizer,
            "commands": {
                "php_version_exit": args.version_exit,
                "php_modules_exit": args.modules_exit,
                "phpt_exit": args.phpt_exit,
            },
            "counts": counts,
            "tests": results,
            "sanitizer_diagnostics": args.sanitizer_diagnostics,
            "success": success,
            "completed_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        },
    )
    return 0 if success else 1


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)

    fingerprint = commands.add_parser("fingerprint")
    fingerprint.add_argument("--repo", required=True)
    fingerprint.add_argument("--profile-file", required=True)
    fingerprint.add_argument("--compiler", required=True)
    fingerprint.add_argument("--env-cc", default="")
    fingerprint.add_argument("--env-cflags", default="")
    fingerprint.add_argument("--env-cppflags", default="")
    fingerprint.add_argument("--env-ldflags", default="")
    fingerprint.add_argument("--configure-arg", action="append", default=[])
    fingerprint.set_defaults(handler=lambda args: print(command_fingerprint(args)) or 0)

    write_state = commands.add_parser("write-state")
    write_state.add_argument("--path", required=True)
    write_state.add_argument("--fingerprint", required=True)
    write_state.add_argument("--profile", required=True)
    write_state.add_argument("--commit", required=True)
    write_state.add_argument("--source-fingerprint", required=True)
    write_state.set_defaults(handler=command_write_state)

    state_matches = commands.add_parser("state-matches")
    state_matches.add_argument("--path", required=True)
    state_matches.add_argument("--fingerprint", required=True)
    state_matches.set_defaults(handler=command_state_matches)

    manifest = commands.add_parser("write-manifest")
    manifest.add_argument("--path", required=True)
    manifest.add_argument("--repo", required=True)
    manifest.add_argument("--base-commit", required=True)
    manifest.add_argument("--worktree-id", required=True)
    manifest.add_argument("--profile", required=True)
    manifest.add_argument("--build-type", required=True)
    manifest.add_argument("--thread-safety", required=True)
    manifest.add_argument("--sanitizer", required=True)
    manifest.add_argument("--configure-program", required=True)
    manifest.add_argument("--configure-arg", action="append", default=[])
    manifest.add_argument("--fingerprint", required=True)
    manifest.add_argument("--compiler", required=True)
    manifest.add_argument("--build-dir", required=True)
    manifest.add_argument("--binary", required=True)
    manifest.add_argument("--runtime-debug", choices=("0", "1"), required=True)
    manifest.add_argument("--runtime-zts", choices=("0", "1"), required=True)
    manifest.add_argument("--opcache-loaded", choices=("0", "1"), required=True)
    manifest.add_argument("--jobs", type=int, required=True)
    manifest.set_defaults(handler=command_write_manifest)

    summary = commands.add_parser("write-summary")
    summary.add_argument("--path", required=True)
    summary.add_argument("--profile", required=True)
    summary.add_argument("--binary", required=True)
    summary.add_argument("--sanitizer", required=True)
    summary.add_argument("--results", required=True)
    summary.add_argument("--version-exit", type=int, required=True)
    summary.add_argument("--modules-exit", type=int, required=True)
    summary.add_argument("--phpt-exit", type=int, required=True)
    summary.add_argument("--sanitizer-diagnostics", action="store_true")
    summary.set_defaults(handler=command_write_summary)
    return root


def main() -> int:
    args = parser().parse_args()
    try:
        return int(args.handler(args))
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"capture-build-manifest.py: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

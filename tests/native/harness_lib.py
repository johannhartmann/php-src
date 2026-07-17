#!/usr/bin/env python3
"""Shared standard-library helpers for the native test and benchmark harnesses."""

from __future__ import annotations

import base64
import hashlib
import json
import os
import signal
import subprocess
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence


EXIT_ALL_EQUIVALENT = 0
EXIT_DIFFERENT = 1
EXIT_HARNESS_ERROR = 2
EXIT_TIMEOUT = 3


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def stable_json_dump(data: Any, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(data, stream, indent=2, sort_keys=True, ensure_ascii=False)
        stream.write("\n")
    os.replace(str(temporary), str(path))


def json_load(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def canonical_executable(value: str) -> Path:
    candidate = Path(value).expanduser()
    if not candidate.is_absolute():
        from_path = shutil_which(value)
        if from_path is None:
            candidate = Path.cwd() / candidate
        else:
            candidate = Path(from_path)
    resolved = candidate.resolve()
    if not resolved.is_file():
        raise ValueError("executable is not a file: {}".format(value))
    if not os.access(str(resolved), os.X_OK):
        raise ValueError("file is not executable: {}".format(value))
    return resolved


def shutil_which(name: str) -> Optional[str]:
    # Kept local so every tool remains explicit about the exact executable used.
    import shutil

    return shutil.which(name)


@dataclass
class ProcessResult:
    stdout: bytes
    stderr: bytes
    returncode: int
    timed_out: bool
    duration_ns: int

    @property
    def exit_kind(self) -> str:
        if self.timed_out:
            return "timeout"
        if self.returncode < 0:
            return "signal"
        return "exit"

    @property
    def exit_code(self) -> Optional[int]:
        return self.returncode if self.returncode >= 0 and not self.timed_out else None

    @property
    def signal_number(self) -> Optional[int]:
        return -self.returncode if self.returncode < 0 and not self.timed_out else None

    def termination_json(self) -> Dict[str, Any]:
        return {
            "exit_code": self.exit_code,
            "kind": self.exit_kind,
            "signal": self.signal_number,
            "timeout": self.timed_out,
        }


@dataclass
class FileProcessResult:
    stdout_path: Path
    stderr_path: Path
    returncode: int
    timed_out: bool
    duration_ns: int

    @property
    def exit_kind(self) -> str:
        if self.timed_out:
            return "timeout"
        if self.returncode < 0:
            return "signal"
        return "exit"

    def termination_json(self) -> Dict[str, Any]:
        return {
            "exit_code": self.returncode if self.exit_kind == "exit" else None,
            "kind": self.exit_kind,
            "signal": -self.returncode if self.exit_kind == "signal" else None,
            "timeout": self.timed_out,
        }


def run_process(argv: Sequence[str], timeout: float, cwd: Optional[Path] = None) -> ProcessResult:
    """Run argv without a shell and kill the complete process group on timeout."""
    if timeout <= 0:
        raise ValueError("timeout must be greater than zero")
    started = time.monotonic_ns()
    process = subprocess.Popen(
        list(argv),
        cwd=str(cwd) if cwd is not None else None,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
    )
    timed_out = False
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        stdout, stderr = process.communicate()
    return ProcessResult(
        stdout=stdout,
        stderr=stderr,
        returncode=process.returncode,
        timed_out=timed_out,
        duration_ns=time.monotonic_ns() - started,
    )


def run_process_to_files(
    argv: Sequence[str],
    timeout: float,
    stdout_path: Path,
    stderr_path: Path,
    cwd: Optional[Path] = None,
) -> FileProcessResult:
    """Run argv with bounded memory, preserving complete stdout/stderr in files."""
    if timeout <= 0:
        raise ValueError("timeout must be greater than zero")
    stdout_path.parent.mkdir(parents=True, exist_ok=True)
    stderr_path.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic_ns()
    timed_out = False
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        process = subprocess.Popen(
            list(argv),
            cwd=str(cwd) if cwd is not None else None,
            stdin=subprocess.DEVNULL,
            stdout=stdout,
            stderr=stderr,
            start_new_session=True,
        )
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
    return FileProcessResult(
        stdout_path=stdout_path,
        stderr_path=stderr_path,
        returncode=process.returncode,
        timed_out=timed_out,
        duration_ns=time.monotonic_ns() - started,
    )


def raw_bytes_json(value: bytes) -> Dict[str, Any]:
    return {
        "base64": base64.b64encode(value).decode("ascii"),
        "length": len(value),
        "sha256": hashlib.sha256(value).hexdigest(),
    }


def raw_file_json(path: Path) -> Dict[str, Any]:
    return {
        "length": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def files_equal(left: Path, right: Path, chunk_size: int = 1024 * 1024) -> bool:
    if left.stat().st_size != right.stat().st_size:
        return False
    with left.open("rb") as left_stream, right.open("rb") as right_stream:
        while True:
            left_chunk = left_stream.read(chunk_size)
            right_chunk = right_stream.read(chunk_size)
            if left_chunk != right_chunk:
                return False
            if not left_chunk:
                return True


def command_capture_json(argv: Sequence[str], timeout: float = 10.0) -> Dict[str, Any]:
    result = run_process(argv, timeout)
    return {
        "argv": list(argv),
        "duration_ns": result.duration_ns,
        "stderr": raw_bytes_json(result.stderr),
        "stdout": raw_bytes_json(result.stdout),
        "termination": result.termination_json(),
    }


def git_output(arguments: Sequence[str], cwd: Path) -> Optional[str]:
    try:
        completed = subprocess.run(
            ["git", "-C", str(cwd)] + list(arguments),
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if completed.returncode != 0:
        return None
    return completed.stdout.strip()


def repository_provenance(path: Path) -> Dict[str, Any]:
    root_text = git_output(["rev-parse", "--show-toplevel"], path.parent)
    if root_text is None:
        return {
            "commit": None,
            "dirty": None,
            "reason": "binary path is not inside a Git worktree",
            "repository_root": None,
        }
    root = Path(root_text).resolve()
    try:
        path.relative_to(root)
    except ValueError:
        return {
            "commit": None,
            "dirty": None,
            "reason": "resolved binary path is outside the detected Git worktree",
            "repository_root": None,
        }
    commit = git_output(["rev-parse", "HEAD"], root)
    status = git_output(["status", "--porcelain=v1", "--untracked-files=normal"], root)
    if commit is None or status is None:
        return {
            "commit": None,
            "dirty": None,
            "reason": "Git provenance commands failed",
            "repository_root": str(root),
        }
    return {
        "commit": commit,
        "dirty": bool(status),
        "reason": None,
        "repository_root": str(root),
    }


def ensure_relative_to(path: Path, parent: Path) -> Path:
    resolved = path.resolve()
    resolved.relative_to(parent.resolve())
    return resolved


def relative_artifact_path(path: Path, json_parent: Path) -> str:
    return Path(os.path.relpath(str(path), str(json_parent))).as_posix()

#!/usr/bin/env python3
"""Run one process in isolation and record exact child resource usage."""

from __future__ import annotations

import argparse
import json
import os
import resource
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json-out", required=True, type=Path)
    parser.add_argument("--stdout-out", required=True, type=Path)
    parser.add_argument("--stderr-out", required=True, type=Path)
    parser.add_argument("--timeout", required=True, type=float)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command and args.command[0] == "--" else args.command
    if not command or args.timeout <= 0:
        parser.error("a command and positive timeout are required")
    for path in (args.json_out, args.stdout_out, args.stderr_out):
        path.parent.mkdir(parents=True, exist_ok=True)
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    started = time.monotonic_ns()
    timed_out = False
    with args.stdout_out.open("wb") as stdout, args.stderr_out.open("wb") as stderr:
        process = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=stdout,
            stderr=stderr,
            start_new_session=True,
        )
        timeout_fired = threading.Event()

        def kill_process_group() -> None:
            timeout_fired.set()
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

        watchdog = threading.Timer(args.timeout, kill_process_group)
        watchdog.daemon = True
        watchdog.start()
        process.wait()
        timed_out = timeout_fired.is_set()
        watchdog.cancel()
        watchdog.join()
    wall_ns = time.monotonic_ns() - started
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    max_rss_bytes = int(after.ru_maxrss)
    if sys.platform != "darwin":
        max_rss_bytes *= 1024
    if timed_out:
        termination = {"exit_code": None, "kind": "timeout", "signal": None, "timeout": True}
    elif process.returncode < 0:
        termination = {"exit_code": None, "kind": "signal", "signal": -process.returncode, "timeout": False}
    else:
        termination = {"exit_code": process.returncode, "kind": "exit", "signal": None, "timeout": False}
    result = {
        "max_rss_bytes": max_rss_bytes,
        "metric_sources": {
            "max_rss": "getrusage(RUSAGE_CHILDREN).ru_maxrss in a fresh probe process",
            "system": "getrusage(RUSAGE_CHILDREN).ru_stime",
            "user": "getrusage(RUSAGE_CHILDREN).ru_utime",
            "wall": "time.monotonic_ns around the child process",
        },
        "system_ns": int((after.ru_stime - before.ru_stime) * 1_000_000_000),
        "termination": termination,
        "user_ns": int((after.ru_utime - before.ru_utime) * 1_000_000_000),
        "wall_ns": wall_ns,
    }
    temporary = args.json_out.with_name(args.json_out.name + ".tmp")
    temporary.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(str(temporary), str(args.json_out))
    return 0


if __name__ == "__main__":
    sys.exit(main())

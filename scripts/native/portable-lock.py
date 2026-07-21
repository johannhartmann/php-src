#!/usr/bin/env python3
"""Hold an advisory build lock until the parent requests release."""

from __future__ import annotations

import fcntl
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: portable-lock.py LOCK_PATH", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as lock_file:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        print("locked", flush=True)
        sys.stdin.readline()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

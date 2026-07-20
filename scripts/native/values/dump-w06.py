#!/usr/bin/env python3
"""Compile one PHP source through the default-off W06 test bridge."""

from __future__ import annotations

import argparse
import base64
import json
import subprocess
import sys
from pathlib import Path


def php_array(options: dict[str, object]) -> str:
    return "[" + ",".join(
        f"{json.dumps(key)}=>{json.dumps(value)}"
        for key, value in options.items()
    ) + "]"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--php", required=True, type=Path)
    parser.add_argument("--function")
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    source = args.source.read_bytes()
    options: dict[str, object] = {"wave": 6}
    if args.function:
        options["function"] = args.function
    code = (
        "$r=native_mir_test_compile_dump(base64_decode("
        + json.dumps(base64.b64encode(source).decode("ascii"))
        + ",true),"
        + json.dumps(args.source.name)
        + ","
        + php_array(options)
        + ");echo json_encode($r,JSON_PRETTY_PRINT|JSON_UNESCAPED_SLASHES);"
    )
    completed = subprocess.run(
        [str(args.php), "-n", "-r", code],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.stdout:
        sys.stdout.write(completed.stdout)
        if not completed.stdout.endswith("\n"):
            sys.stdout.write("\n")
    if completed.stderr:
        sys.stderr.write(completed.stderr)
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())

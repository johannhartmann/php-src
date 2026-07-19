#!/usr/bin/env python3
"""Invoke the default-off W05 compile/dump bridge without executing source."""

from __future__ import annotations

import argparse
import base64
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any


class DumpError(RuntimeError):
    """The W05 bridge did not return a trustworthy result."""


def candidate_path(value: str) -> Path:
    path = Path(value).expanduser().resolve()
    if not path.is_file() or not os.access(path, os.X_OK):
        raise DumpError(f"candidate PHP is not executable: {value}")
    return path


def invoke(
    candidate: Path,
    source: bytes,
    filename: str,
    function: str | None = None,
    repeat: int = 1,
    timeout: float = 20.0,
    fault: str | None = None,
    compiler_mode: str | None = None,
) -> dict[str, Any]:
    if repeat < 1 or repeat > 10:
        raise DumpError("repeat must be between 1 and 10")
    payload = base64.b64encode(source).decode("ascii")
    function_json = json.dumps(function)
    program = (
        "$source=base64_decode(" + json.dumps(payload) + ",true);"
        "$options=['wave'=>5];"
        f"$function={function_json};"
        "if($function!==null){$options['function']=$function;}"
        f"$fault={json.dumps(fault)};"
        "if($fault!==null){$options['fault']=$fault;}"
        f"$compilerMode={json.dumps(compiler_mode)};"
        "if($compilerMode!==null){$options['compiler_mode']=$compilerMode;}"
        f"$repeat={repeat};$calls=[];"
        "for($i=0;$i<$repeat;$i++){"
        "$calls[]=native_mir_test_compile_dump($source,"
        + json.dumps(filename)
        + ",$options);}"
        "echo json_encode(['schema_version'=>1,'calls'=>$calls],"
        "JSON_UNESCAPED_SLASHES|JSON_THROW_ON_ERROR);"
    )
    environment = os.environ.copy()
    environment.update(
        {"LANG": "C", "LC_ALL": "C", "TZ": "UTC", "SOURCE_DATE_EPOCH": "0"}
    )
    try:
        completed = subprocess.run(
            [str(candidate), "-n", "-r", program],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=timeout,
            env=environment,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise DumpError(f"W05 bridge invocation failed: {error}") from error
    if completed.returncode != 0:
        raise DumpError(
            "W05 bridge exited with {}: {}".format(
                completed.returncode,
                completed.stderr.decode("utf-8", errors="replace"),
            )
        )
    try:
        document = json.loads(completed.stdout)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise DumpError("W05 bridge returned invalid JSON") from error
    if (
        not isinstance(document, dict)
        or document.get("schema_version") != 1
        or not isinstance(document.get("calls"), list)
        or len(document["calls"]) != repeat
    ):
        raise DumpError("W05 bridge returned an invalid result envelope")
    return document


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate-php", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--filename")
    parser.add_argument("--function")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--fault")
    parser.add_argument(
        "--compiler-mode", choices=["ignore_user_functions"]
    )
    arguments = parser.parse_args()
    source = Path(arguments.source).resolve()
    try:
        document = invoke(
            candidate_path(arguments.candidate_php),
            source.read_bytes(),
            arguments.filename or source.name,
            arguments.function,
            arguments.repeat,
            arguments.timeout,
            arguments.fault,
            arguments.compiler_mode,
        )
    except (DumpError, OSError) as error:
        print(f"dump-w05: {error}", file=sys.stderr)
        return 2
    json.dump(document, sys.stdout, sort_keys=True, separators=(",", ":"))
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

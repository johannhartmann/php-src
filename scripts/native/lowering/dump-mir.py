#!/usr/bin/env python3
"""Compile PHP source through the test-only W03 bridge and emit stable JSON."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[3]
INVOKER = ROOT / "tests/native/lowering/bridge/invoke_dump.php"

EXIT_ACCEPTED = 0
EXIT_REJECTED = 10
EXIT_ERROR = 20

STATUS_EXIT = {
    "accepted": EXIT_ACCEPTED,
    "rejected": EXIT_REJECTED,
    "error": EXIT_ERROR,
}
PHASES = {"compile", "ssa", "lowering", "verify", "dump", "complete"}
DIAGNOSTIC_STAGES = {"compile", "ssa", "MIRL", "MIRV", "bridge"}
ADDRESS_PATTERN = re.compile(r"(?i)(?:0x[0-9a-f]{6,}|(?:pointer|address)\s*[:=])")
MIR_POINTER_PATTERN = re.compile(r"(?i)\b(?:pointer|address)\b\s*[:=]?")


class DumpError(ValueError):
    """The bridge invocation or its structured response is invalid."""


def canonical_executable(value: str) -> Path:
    candidate = Path(value).expanduser()
    if not candidate.is_absolute():
        raise DumpError("candidate PHP path must be absolute")
    resolved = candidate.resolve()
    if not resolved.is_file():
        raise DumpError("candidate PHP is not a file: {}".format(value))
    if not os.access(resolved, os.X_OK):
        raise DumpError("candidate PHP is not executable: {}".format(value))
    return resolved


def stable_json_bytes(value: Any) -> bytes:
    return (
        json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    ).encode("utf-8")


def stable_json_write(value: Any, destination: Path | None) -> None:
    payload = stable_json_bytes(value)
    if destination is None:
        sys.stdout.buffer.write(payload)
        return
    destination = destination.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".tmp")
    temporary.write_bytes(payload)
    os.replace(temporary, destination)


def deterministic_environment(
    base: dict[str, str] | None = None,
) -> dict[str, str]:
    environment = (base if base is not None else os.environ).copy()
    environment.update(
        {
            "LANG": "C",
            "LC_ALL": "C",
            "TZ": "UTC",
            "SOURCE_DATE_EPOCH": "0",
        }
    )
    return environment


def source_identity(filename: str, source: bytes) -> str:
    value = 0xCBF29CE484222325
    for byte in filename.encode("utf-8") + b"\0" + source:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return "fnv1a64:{:016x}".format(value)


def has_address_like_value(value: Any) -> bool:
    if isinstance(value, str):
        return ADDRESS_PATTERN.search(value) is not None
    if isinstance(value, list):
        return any(has_address_like_value(item) for item in value)
    if isinstance(value, dict):
        return any(
            ADDRESS_PATTERN.search(str(key)) is not None
            or (
                MIR_POINTER_PATTERN.search(item) is not None
                if key == "mir" and isinstance(item, str)
                else has_address_like_value(item)
            )
            for key, item in value.items()
        )
    return False


def require_exact_keys(
    value: dict[str, Any], required: set[str], optional: set[str], where: str
) -> None:
    missing = required - value.keys()
    unknown = value.keys() - required - optional
    if missing:
        raise DumpError("{} is missing keys: {}".format(where, ", ".join(sorted(missing))))
    if unknown:
        raise DumpError("{} has unknown keys: {}".format(where, ", ".join(sorted(unknown))))


def validate_source_identity(value: Any, filename: str, source: bytes) -> None:
    if not isinstance(value, dict):
        raise DumpError("result.source must be an object")
    require_exact_keys(
        value,
        {"byte_length", "filename", "source_id"},
        set(),
        "result.source",
    )
    if value["filename"] != filename:
        raise DumpError("result.source.filename does not match the requested filename")
    if value["byte_length"] != len(source):
        raise DumpError("result.source.byte_length does not match the source")
    source_id = value["source_id"]
    expected_source_id = source_identity(filename, source)
    if source_id != expected_source_id:
        raise DumpError("result.source.source_id does not match the requested source")


def validate_diagnostics(value: Any) -> None:
    if not isinstance(value, list):
        raise DumpError("result.diagnostics must be an array")
    previous: tuple[str, str, int, str] | None = None
    for index, diagnostic in enumerate(value):
        where = "result.diagnostics[{}]".format(index)
        if not isinstance(diagnostic, dict):
            raise DumpError("{} must be an object".format(where))
        require_exact_keys(
            diagnostic,
            {"code", "message", "opline", "stage"},
            set(),
            where,
        )
        if diagnostic["stage"] not in DIAGNOSTIC_STAGES:
            raise DumpError("{} has an invalid stage".format(where))
        if not isinstance(diagnostic["code"], str) or not diagnostic["code"]:
            raise DumpError("{} has an invalid code".format(where))
        if not isinstance(diagnostic["message"], str):
            raise DumpError("{} has an invalid message".format(where))
        if diagnostic["opline"] is not None and (
            type(diagnostic["opline"]) is not int or diagnostic["opline"] < 0
        ):
            raise DumpError("{} has an invalid opline".format(where))
        sort_key = (
            diagnostic["stage"],
            diagnostic["code"],
            diagnostic["opline"] if diagnostic["opline"] is not None else -1,
            diagnostic["message"],
        )
        if previous is not None and sort_key < previous:
            raise DumpError("result diagnostics are not canonically ordered")
        previous = sort_key


def validate_call_result(value: Any, filename: str, source: bytes) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise DumpError("bridge call result must be an object")
    require_exact_keys(
        value,
        {"diagnostics", "mir", "phase", "schema_version", "source", "status"},
        set(),
        "result",
    )
    if value["schema_version"] != 1:
        raise DumpError("result.schema_version must be 1")
    if value["status"] not in STATUS_EXIT:
        raise DumpError("result.status is invalid")
    if value["phase"] not in PHASES:
        raise DumpError("result.phase is invalid")
    validate_source_identity(value["source"], filename, source)
    validate_diagnostics(value["diagnostics"])
    mir = value["mir"]
    if value["status"] == "accepted":
        if value["phase"] != "complete":
            raise DumpError("accepted result must have phase complete")
        if not isinstance(mir, str) or not mir.startswith("znmir "):
            raise DumpError("accepted result must contain canonical ZNMIR")
        if "MIRL0000" not in {
            diagnostic["code"] for diagnostic in value["diagnostics"]
        }:
            raise DumpError("accepted result must report MIRL0000")
    else:
        if value["phase"] == "complete":
            raise DumpError("non-accepted result must not have phase complete")
        if mir is not None:
            raise DumpError("non-accepted result must not expose partial MIR")
        if not value["diagnostics"]:
            raise DumpError("non-accepted result must contain a stable diagnostic")
    if has_address_like_value(value):
        raise DumpError("bridge result contains an address-like value")
    return value


def invoke(
    candidate_value: str,
    source: bytes,
    filename: str,
    *,
    function: str | None = None,
    repeat: int = 1,
    diagnostic_limit: int = 32,
    arena_chunk_size: int = 4096,
    fault: str | None = None,
    opcache_enabled: bool = False,
    timeout: float = 30.0,
    environment: dict[str, str] | None = None,
) -> tuple[dict[str, Any], int]:
    candidate = canonical_executable(candidate_value)
    if not filename or "\x00" in filename:
        raise DumpError("filename must be explicit, non-empty, and contain no NUL")
    if repeat < 1 or repeat > 10:
        raise DumpError("repeat must be between 1 and 10")
    if diagnostic_limit < 1 or diagnostic_limit > 256:
        raise DumpError("diagnostic limit must be between 1 and 256")
    if arena_chunk_size < 64 or arena_chunk_size > 1024 * 1024:
        raise DumpError("arena chunk size must be between 64 and 1048576")
    if timeout <= 0:
        raise DumpError("timeout must be greater than zero")
    request = {
        "filename": filename,
        "options": {
            "arena_chunk_size": arena_chunk_size,
            "diagnostic_limit": diagnostic_limit,
            "fault": fault,
            "function": function,
        },
        "repeat": repeat,
        "source_base64": base64.b64encode(source).decode("ascii"),
    }
    try:
        completed = subprocess.run(
            [
                str(candidate),
                "-n",
                "-d",
                "date.timezone=UTC",
                "-d",
                "display_errors=stderr",
                "-d",
                "opcache.enable_cli={}".format(1 if opcache_enabled else 0),
                str(INVOKER),
            ],
            input=json.dumps(request, sort_keys=True).encode("utf-8"),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=timeout,
            env=deterministic_environment(environment),
            cwd=ROOT,
        )
    except subprocess.TimeoutExpired as error:
        raise DumpError("candidate dump bridge timed out") from error
    except OSError as error:
        raise DumpError("candidate dump bridge failed: {}".format(error)) from error
    if completed.returncode != 0:
        raise DumpError(
            "candidate dump bridge exited {}: {}".format(
                completed.returncode,
                completed.stderr.decode("utf-8", errors="replace").strip(),
            )
        )
    if completed.stderr:
        raise DumpError(
            "candidate dump bridge wrote unexpected stderr: {}".format(
                completed.stderr.decode("utf-8", errors="replace").strip()
            )
        )
    try:
        envelope = json.loads(completed.stdout)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise DumpError("candidate dump bridge returned invalid JSON") from error
    if not isinstance(envelope, dict):
        raise DumpError("candidate dump bridge envelope must be an object")
    require_exact_keys(envelope, {"calls", "schema_version"}, set(), "envelope")
    if envelope["schema_version"] != 1:
        raise DumpError("bridge envelope schema_version must be 1")
    if not isinstance(envelope["calls"], list) or len(envelope["calls"]) != repeat:
        raise DumpError("bridge envelope call count does not match repeat")

    calls = []
    for index, item in enumerate(envelope["calls"], start=1):
        result = validate_call_result(item, filename, source)
        mir_bytes = result["mir"].encode("utf-8") if result["mir"] is not None else b""
        calls.append(
            {
                "call": index,
                "mir_sha256": hashlib.sha256(mir_bytes).hexdigest(),
                "result": result,
            }
        )
    first_status = calls[0]["result"]["status"]
    for call in calls[1:]:
        if call["result"] != calls[0]["result"]:
            raise DumpError("repeated bridge calls are not byte-for-byte deterministic")
    document = {
        "calls": calls,
        "candidate": str(candidate),
        "filename": filename,
        "normalization": {"enabled": False, "rules": []},
        "repeat": repeat,
        "schema_version": 1,
        "source_sha256": hashlib.sha256(source).hexdigest(),
        "status": first_status,
    }
    return document, STATUS_EXIT[first_status]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", required=True, help="absolute W03 PHP path")
    parser.add_argument("--source", required=True, type=Path, help="source file to compile")
    parser.add_argument("--filename", required=True, help="explicit compiler filename")
    parser.add_argument("--function", help="optional compiled function to lower")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--diagnostic-limit", type=int, default=32)
    parser.add_argument("--arena-chunk-size", type=int, default=4096)
    parser.add_argument("--opcache", choices=("on", "off"), default="off")
    parser.add_argument(
        "--fault",
        choices=[
            "compile_bailout",
            "ssa_failure",
            "lower_failure",
            "module_oom",
            "dump_failure",
        ],
    )
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args()
    try:
        source = args.source.read_bytes()
        document, exit_code = invoke(
            args.candidate,
            source,
            args.filename,
            function=args.function,
            repeat=args.repeat,
            diagnostic_limit=args.diagnostic_limit,
            arena_chunk_size=args.arena_chunk_size,
            fault=args.fault,
            opcache_enabled=args.opcache == "on",
            timeout=args.timeout,
        )
        stable_json_write(document, args.json_out)
        return exit_code
    except (OSError, DumpError) as error:
        stable_json_write(
            {
                "error": str(error),
                "schema_version": 1,
                "status": "error",
            },
            args.json_out,
        )
        print("dump-mir.py: {}".format(error), file=sys.stderr)
        return EXIT_ERROR


if __name__ == "__main__":
    raise SystemExit(main())

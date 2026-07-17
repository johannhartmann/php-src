#!/usr/bin/env python3
"""Capture binary, PHP, source-control, and host provenance as JSON."""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict

from harness_lib import (
    canonical_executable,
    command_capture_json,
    repository_provenance,
    run_process,
    sha256_file,
    stable_json_dump,
    utc_now,
)
from schema_validation import ValidationError, validate_manifest


FACT_SCRIPT = r'''$facts = [
    "architecture" => php_uname("m"),
    "debug" => (bool) PHP_DEBUG,
    "integer_size_bytes" => PHP_INT_SIZE,
    "opcache_enable_cli" => ini_get("opcache.enable_cli"),
    "opcache_loaded" => extension_loaded("Zend OPcache"),
    "os_family" => PHP_OS_FAMILY,
    "php_binary" => PHP_BINARY,
    "php_sapi" => PHP_SAPI,
    "php_version" => PHP_VERSION,
    "php_version_id" => PHP_VERSION_ID,
    "thread_safety" => PHP_ZTS ? "ZTS" : "NTS",
    "zend_version" => zend_version(),
];
echo json_encode($facts, JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR);
'''

PHP_INFO_KEYS = {
    "Build Date",
    "Build System",
    "Configure Command",
    "Debug Build",
    "PHP Extension Build",
    "Server API",
    "Thread Safety",
    "Zend Extension Build",
}


def cpu_metadata() -> Dict[str, Any]:
    metadata: Dict[str, Any] = {"logical_count": os.cpu_count(), "model": None}
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.is_file():
        try:
            for line in cpuinfo.read_text(encoding="utf-8", errors="replace").splitlines():
                if line.lower().startswith(("model name", "hardware")) and ":" in line:
                    metadata["model"] = line.split(":", 1)[1].strip()
                    break
        except OSError:
            pass
    return metadata


def selected_php_info(binary: Path) -> Dict[str, str]:
    result = run_process([str(binary), "-i"], 15.0)
    if result.timed_out or result.returncode != 0:
        raise RuntimeError("php -i query failed: {}".format(result.termination_json()))
    selected: Dict[str, str] = {}
    for line in result.stdout.decode("utf-8", errors="replace").splitlines():
        if " => " not in line:
            continue
        key, value = line.split(" => ", 1)
        if key in PHP_INFO_KEYS and key not in selected:
            selected[key] = value
    return selected


def elf_build_id(binary: Path) -> Dict[str, Any]:
    readelf = shutil.which("readelf")
    if readelf is None:
        return {"value": None, "source": None, "unsupported_reason": "readelf is unavailable"}
    try:
        completed = subprocess.run(
            [readelf, "-n", str(binary)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"value": None, "source": None, "unsupported_reason": "readelf -n failed: {}".format(error)}
    if completed.returncode != 0:
        return {"value": None, "source": None, "unsupported_reason": "readelf -n returned {}".format(completed.returncode)}
    for line in completed.stdout.splitlines():
        stripped = line.strip()
        if stripped.startswith("Build ID:"):
            value = stripped.split(":", 1)[1].strip().lower()
            if value and all(character in "0123456789abcdef" for character in value):
                return {"value": value, "source": "{} -n".format(readelf), "unsupported_reason": None}
    return {"value": None, "source": "{} -n".format(readelf), "unsupported_reason": "no ELF build ID note found"}


def capture_manifest(binary_value: str) -> Dict[str, Any]:
    binary = canonical_executable(binary_value)
    version = command_capture_json([str(binary), "-v"])
    if version["termination"]["kind"] != "exit" or version["termination"]["exit_code"] != 0:
        raise RuntimeError("php -v query failed: {}".format(version["termination"]))
    facts_process = run_process([str(binary), "-d", "display_errors=stderr", "-r", FACT_SCRIPT], 10.0)
    if facts_process.timed_out or facts_process.returncode != 0:
        raise RuntimeError(
            "PHP fact query failed ({}): {}".format(
                facts_process.termination_json(), facts_process.stderr.decode("utf-8", errors="replace")
            )
        )
    try:
        facts = json.loads(facts_process.stdout.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RuntimeError("PHP fact query did not return valid UTF-8 JSON: {}".format(error))
    manifest = {
        "binary": {
            "canonical_path": str(binary),
            "elf_build_id": elf_build_id(binary),
            "sha256": sha256_file(binary),
            "size_bytes": binary.stat().st_size,
        },
        "generated_at_utc": utc_now(),
        "host": {
            "cpu": cpu_metadata(),
            "kernel": {
                "machine": platform.machine(),
                "release": platform.release(),
                "system": platform.system(),
                "version": platform.version(),
            },
            "platform": platform.platform(),
        },
        "php": {
            "facts": facts,
            "info": selected_php_info(binary),
            "version_command": version,
        },
        "provenance": repository_provenance(binary),
        "result_type": "binary_manifest",
        "schema_version": 1,
        "volatile_fields": [
            "/generated_at_utc",
            "/php/version_command/duration_ns",
        ],
    }
    validate_manifest(manifest)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--php", required=True, help="explicit PHP executable path")
    parser.add_argument("--json-out", required=True, type=Path)
    args = parser.parse_args()
    try:
        manifest = capture_manifest(args.php)
        stable_json_dump(manifest, args.json_out)
    except (OSError, RuntimeError, ValueError, ValidationError) as error:
        print("capture_manifest.py: {}".format(error), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())

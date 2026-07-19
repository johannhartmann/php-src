#!/usr/bin/env python3
"""Run fixed-seed real Zend compile/SSA/W05 call-model fuzz cases."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import random
import subprocess
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
INVOKER = Path(__file__).with_name("invoke_fuzz.php")
BATCH_SIZE = 200


class FuzzError(RuntimeError):
    """A generated W05 case violated failure atomicity or classification."""


def candidate_path(value: str | None) -> Path:
    selected = value or os.environ.get("TEST_PHP_EXECUTABLE")
    if not selected:
        raise FuzzError("set TEST_PHP_EXECUTABLE or use --candidate-php")
    path = Path(selected).expanduser().resolve()
    if not path.is_file() or not os.access(path, os.X_OK):
        raise FuzzError(f"candidate PHP is not executable: {selected}")
    return path


def generated_case(rng: random.Random, index: int) -> dict[str, str]:
    value = rng.randrange(-(1 << 30), 1 << 30)
    category = index % 6
    target = f"w05_target_{index}"
    function = f"w05_case_{index}"
    if category == 0:
        source = (
            f"<?php function {target}(): void {{ echo 1; }} "
            f"function {function}(): void {{ {target}(); }}"
        )
        status, code = "accepted", "MIRL0000"
    elif category == 1:
        source = (
            f"<?php function {target}($value): void {{ echo $value; }} "
            f"function {function}(): void {{ {target}({value}); }}"
        )
        status, code = "accepted", "MIRL0000"
    elif category == 2:
        source = (
            f"<?php function {target}(&$value): void {{}} "
            f"function {function}(): void {{ "
            f"$value={value}; {target}($value); }}"
        )
        status, code = "rejected", "MIRL0024"
    elif category == 3:
        source = (
            f"<?php function {target}($value): void {{ echo $value; }} "
            f"function {function}(): void {{ {target}('s{abs(value)}'); }}"
        )
        status, code = "rejected", "MIRL0024"
    elif category == 4:
        source = (
            f"<?php function {function}(): void {{ "
            f"$name='target{abs(value)}'; $name(); }}"
        )
        status, code = "rejected", "MIRL0023"
    else:
        source = (
            f"<?php function {function}(): void {{ "
            f"external_target_{abs(value)}(value: {value}); }}"
        )
        status, code = "rejected", "MIRL0024"
    return {
        "source": base64.b64encode(source.encode("utf-8")).decode("ascii"),
        "filename": f"w05-fuzz-{index:06d}.php",
        "function": function,
        "status": status,
        "code": code,
    }


def invoke(candidate: Path, cases: list[dict[str, str]]) -> list[dict[str, Any]]:
    payload = json.dumps(
        {"cases": [{"source": case["source"], "filename": case["filename"],
                    "function": case["function"]}
                   for case in cases]},
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    completed = subprocess.run(
        [str(candidate), "-n", str(INVOKER)],
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=60,
        env={
            **os.environ,
            "LANG": "C",
            "LC_ALL": "C",
            "TZ": "UTC",
            "SOURCE_DATE_EPOCH": "0",
        },
    )
    if completed.returncode != 0:
        raise FuzzError(
            f"candidate batch failed ({completed.returncode}): "
            + completed.stderr.decode("utf-8", errors="replace")
        )
    try:
        document = json.loads(completed.stdout)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise FuzzError("candidate returned invalid fuzz JSON") from error
    results = document.get("results")
    if not isinstance(results, list) or len(results) != len(cases):
        raise FuzzError("candidate returned an invalid fuzz result count")
    return results


def run(seed: int, count: int, candidate: Path) -> str:
    if seed < 0 or count < 1 or count > 1_000_000:
        raise FuzzError("invalid seed or case count")
    rng = random.Random(seed)
    digest = hashlib.sha256()
    for offset in range(0, count, BATCH_SIZE):
        cases = [
            generated_case(rng, index)
            for index in range(offset, min(offset + BATCH_SIZE, count))
        ]
        results = invoke(candidate, cases)
        for case, result in zip(cases, results, strict=True):
            codes = [
                diagnostic.get("code")
                for diagnostic in result.get("diagnostics", [])
            ]
            if result.get("status") != case["status"] or case["code"] not in codes:
                raise FuzzError(
                    f"{case['filename']}: expected {case['status']}/{case['code']}, "
                    f"got {result.get('status')}/{codes}"
                )
            if case["status"] == "accepted":
                mir = result.get("mir")
                if (
                    not isinstance(mir, str)
                    or "opcode call_direct_user" not in mir
                    or "codegen-eligible false" not in mir
                ):
                    raise FuzzError(f"{case['filename']}: incomplete accepted MIR")
            elif result.get("mir") is not None:
                raise FuzzError(f"{case['filename']}: rejected case published MIR")
            digest.update(
                json.dumps(result, sort_keys=True, separators=(",", ":")).encode()
            )
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--cases", type=int, required=True)
    parser.add_argument("--candidate-php")
    arguments = parser.parse_args()
    try:
        digest = run(
            arguments.seed,
            arguments.cases,
            candidate_path(arguments.candidate_php),
        )
    except (FuzzError, OSError, subprocess.TimeoutExpired) as error:
        print(f"W05 fuzz: FAIL: {error}", file=sys.stderr)
        return 1
    print(
        f"W05 fuzz: PASS seed={arguments.seed} cases={arguments.cases} "
        f"sha256={digest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

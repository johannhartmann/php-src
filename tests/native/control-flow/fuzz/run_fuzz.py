#!/usr/bin/env python3
"""Run deterministic W04 source, option, diagnostic, bailout, and manifest fuzzing."""

from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
import random
import sys
from pathlib import Path
from typing import Any, Callable


ROOT = Path(__file__).resolve().parents[4]
DUMP_PATH = ROOT / "scripts/native/control-flow/dump-w04.py"
VALIDATOR_PATH = ROOT / "tests/native/control-flow/corpus/validate_manifest.py"


class FuzzError(AssertionError):
    """A W04 evidence validator accepted or rejected the wrong mutation."""


def load_module(name: str, path: Path) -> Any:
    specification = importlib.util.spec_from_file_location(name, path)
    if specification is None or specification.loader is None:
        raise FuzzError("unable to load {}".format(path))
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


dump_w04 = load_module("w04_fuzz_dump", DUMP_PATH)
manifest_validator = load_module("w04_fuzz_manifest", VALIDATOR_PATH)


def base_result(
    source: bytes,
    filename: str,
    *,
    status: str = "rejected",
    phase: str = "lowering",
    code: str = "MIRL0011",
    diagnostics: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    return {
        "diagnostics": diagnostics
        if diagnostics is not None
        else [{"code": code, "message": "stable", "opline": None, "stage": "MIRL"}],
        "mir": None,
        "phase": phase,
        "schema_version": 1,
        "source": {
            "byte_length": len(source),
            "filename": filename,
            "source_id": dump_w04.source_identity(filename, source),
        },
        "status": status,
        "wave": 4,
    }


def expect(
    operation: Callable[[], None],
    should_accept: bool,
    label: str,
) -> str:
    try:
        operation()
    except (dump_w04.DumpError, manifest_validator.ManifestError, ValueError):
        if should_accept:
            raise FuzzError("{} unexpectedly rejected".format(label))
        return "rejected"
    if not should_accept:
        raise FuzzError("{} unexpectedly accepted".format(label))
    return "accepted"


def fuzz_source(rng: random.Random, index: int) -> tuple[str, str]:
    length = rng.randrange(0, 257)
    source = bytes(rng.randrange(0, 256) for _ in range(length))
    filename = "fuzz/source-{:06d}.php".format(index)
    result = base_result(source, filename)
    corrupt = rng.randrange(4) == 0
    if corrupt:
        result["source"]["source_id"] = "fnv1a64:0000000000000000"
    outcome = expect(
        lambda: dump_w04.validate_call_result(result, filename, source),
        not corrupt,
        "source identity",
    )
    return outcome, "corrupt={};length={}".format(int(corrupt), length)


def fuzz_options(rng: random.Random, index: int) -> tuple[str, str]:
    del index
    if rng.randrange(2) == 0:
        repeat = rng.choice([0, 1, 2, 10, 11, True, -1])
        limit = rng.choice([0, 1, 32, 256, 257, True])
        chunk = rng.choice([0, 64, 4096, 1024 * 1024, 1024 * 1024 + 1, True])
        timeout = rng.choice([-1.0, 0.0, 0.1, 5.0, True])
        valid = (
            type(repeat) is int
            and 1 <= repeat <= 10
            and type(limit) is int
            and 1 <= limit <= 256
            and type(chunk) is int
            and 64 <= chunk <= 1024 * 1024
            and isinstance(timeout, (int, float))
            and not isinstance(timeout, bool)
            and timeout > 0
        )
        outcome = expect(
            lambda: dump_w04.validate_request(
                "fuzz-options.php", repeat, limit, chunk, timeout
            ),
            valid,
            "option bounds",
        )
        return outcome, "bounds={},{},{},{}".format(repeat, limit, chunk, timeout)
    source = b"<?php return 1;\n"
    filename = "fuzz-wave.php"
    result = base_result(source, filename)
    wave = rng.choice([-1, 0, 3, 4, 5, "4", None, True])
    result["wave"] = wave
    outcome = expect(
        lambda: dump_w04.validate_call_result(result, filename, source),
        type(wave) is int and wave == 4,
        "wave option",
    )
    return outcome, "wave={!r}".format(wave)


def fuzz_diagnostic_limit(rng: random.Random, index: int) -> tuple[str, str]:
    source = b"<?php return 1;\n"
    filename = "fuzz-diagnostic-{:06d}.php".format(index)
    limit = rng.randrange(1, 17)
    overflow = rng.randrange(3) == 0
    count = limit + 1 if overflow else rng.randrange(1, limit + 1)
    diagnostics = [
        {
            "code": "MIRL{:04d}".format(number),
            "message": "diagnostic-{:04d}".format(number),
            "opline": number,
            "stage": "MIRL",
        }
        for number in range(count)
    ]
    result = base_result(source, filename, diagnostics=diagnostics)
    outcome = expect(
        lambda: dump_w04.validate_call_result(
            result, filename, source, diagnostic_limit=limit
        ),
        not overflow,
        "diagnostic limit",
    )
    return outcome, "limit={};count={}".format(limit, count)


def fuzz_bailout(rng: random.Random, index: int) -> tuple[str, str]:
    source = b"<?php function broken( {\n"
    filename = "fuzz-bailout-{:06d}.php".format(index)
    stage, phase, code = rng.choice(
        [
            ("compile", "compile", "COMPILE_ERROR"),
            ("compile", "compile", "BAILOUT"),
            ("ssa", "ssa", "SSA0001"),
            ("MIRL", "lowering", "MIRL0007"),
            ("MIRV", "dump", "MIRV0011"),
        ]
    )
    diagnostic = {
        "code": code,
        "message": "failure-atomic injected failure",
        "opline": None,
        "stage": stage,
    }
    result = base_result(
        source,
        filename,
        status="error",
        phase=phase,
        code=code,
        diagnostics=[diagnostic],
    )
    corrupt = rng.randrange(5) == 0
    if corrupt:
        result["mir"] = "znmir partial\n"
    outcome = expect(
        lambda: dump_w04.validate_call_result(result, filename, source),
        not corrupt,
        "bailout failure atomicity",
    )
    return outcome, "code={};partial={}".format(code, int(corrupt))


def fuzz_manifest_mutation(rng: random.Random, index: int) -> tuple[str, str]:
    del index
    document = manifest_validator.load_json(manifest_validator.MANIFEST)
    case = copy.deepcopy(rng.choice(document["cases"]))
    mutation = rng.choice(
        ["unknown", "golden", "repeat", "opcode", "classification", "valid"]
    )
    if mutation == "unknown":
        case["unknown"] = True
        outcome = expect(
            lambda: manifest_validator.validate_schema_value(
                case,
                manifest_validator.load_json(manifest_validator.SCHEMA)["$defs"]["case"],
                manifest_validator.load_json(manifest_validator.SCHEMA),
                "$.case",
            ),
            False,
            "manifest unknown field",
        )
    elif mutation == "golden":
        case["mir_sha256"] = "0" * 64
        outcome = expect(
            lambda: manifest_validator.reject_final_mir_fields(case),
            False,
            "manifest final MIR hash",
        )
    elif mutation == "repeat":
        case["repeat_calls"] = list(range(1, 10))
        outcome = expect(
            lambda: manifest_validator.validate_case(case),
            False,
            "manifest call attribution",
        )
    elif mutation == "opcode":
        case["required_source_opcodes"] = ["NOT_A_ZEND_OPCODE"]
        schema = manifest_validator.load_json(manifest_validator.SCHEMA)
        outcome = expect(
            lambda: manifest_validator.validate_schema_value(
                case, schema["$defs"]["case"], schema, "$.case"
            ),
            False,
            "manifest opcode",
        )
    elif mutation == "classification":
        case["expected_mirl"] = "MIRL9999"
        outcome = expect(
            lambda: manifest_validator.validate_case(case),
            False,
            "manifest classification",
        )
    else:
        outcome = expect(
            lambda: manifest_validator.validate_case(case),
            True,
            "valid manifest case",
        )
    return outcome, mutation


FUZZERS: list[tuple[str, Callable[[random.Random, int], tuple[str, str]]]] = [
    ("source", fuzz_source),
    ("options", fuzz_options),
    ("diagnostic_limit", fuzz_diagnostic_limit),
    ("bailout", fuzz_bailout),
    ("manifest_mutation", fuzz_manifest_mutation),
]


def run(seed: int, cases: int) -> dict[str, Any]:
    if type(seed) is not int or seed < 0:
        raise FuzzError("seed must be a non-negative integer")
    if type(cases) is not int or cases < 1 or cases > 1_000_000:
        raise FuzzError("cases must be between 1 and 1000000")
    rng = random.Random(seed)
    digest = hashlib.sha256()
    categories = {name: {"accepted": 0, "rejected": 0, "total": 0} for name, _ in FUZZERS}
    for index in range(cases):
        name, fuzzer = FUZZERS[index % len(FUZZERS)]
        outcome, detail = fuzzer(rng, index)
        categories[name][outcome] += 1
        categories[name]["total"] += 1
        digest.update(
            "{}|{}|{}|{}\n".format(index, name, outcome, detail).encode("utf-8")
        )
    if sum(category["total"] for category in categories.values()) != cases:
        raise FuzzError("fuzz category accounting failed")
    return {
        "cases": cases,
        "categories": categories,
        "schema_version": 1,
        "seed": seed,
        "status": "pass",
        "trace_sha256": digest.hexdigest(),
        "wave": 4,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=20260719)
    parser.add_argument("--cases", type=int, default=5000)
    args = parser.parse_args()
    try:
        result = run(args.seed, args.cases)
    except FuzzError as error:
        print("W04 fuzzing failed: {}".format(error), file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

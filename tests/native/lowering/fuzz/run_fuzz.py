#!/usr/bin/env python3
"""Run fixed-seed W03 source, source-view, and text mutations."""

from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
VALIDATOR_PATH = ROOT / "tests/native/lowering/corpus/validate_manifest.py"
MANIFEST_PATH = ROOT / "tests/native/lowering/corpus/manifest.json"
FRONTEND = ROOT / "Zend/Native/Lowering/Frontend"
FRONTEND_SOURCES = (
    FRONTEND / "zend_mir_zend_source.c",
    FRONTEND / "zend_mir_operand_map.c",
    FRONTEND / "zend_mir_value_facts.c",
    FRONTEND / "zend_mir_literal_pool.c",
    FRONTEND / "zend_mir_slot_map.c",
    FRONTEND / "zend_mir_source_positions.c",
)
TEXT_FUZZER = ROOT / "tests/native/mir/fuzz/run_mutation_fuzz.py"


class FuzzFailure(RuntimeError):
    """A deterministic W03 fuzz invariant failed."""


def load_module(name: str, path: Path) -> Any:
    specification = importlib.util.spec_from_file_location(name, path)
    if specification is None or specification.loader is None:
        raise FuzzFailure(f"unable to load {path}")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


manifest_validator = load_module("w03_fuzz_manifest", VALIDATOR_PATH)


def run_native_mutations(seed: int, counts: dict[str, int], compiler: str) -> None:
    with tempfile.TemporaryDirectory(prefix="w03-fuzz-") as temporary:
        executable = Path(temporary) / "frontend-mutation"
        subprocess.run(
            [
                compiler,
                "-std=c11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'tests/native/lowering/frontend/include'}",
                f"-I{ROOT}",
                f"-I{ROOT / 'Zend'}",
                *(str(source) for source in FRONTEND_SOURCES),
                str(Path(__file__).with_name("frontend_mutation.c")),
                "-o",
                str(executable),
            ],
            cwd=ROOT,
            check=True,
            timeout=60,
        )
        subprocess.run(
            [
                str(executable),
                str(seed),
                str(counts["source"]),
                str(counts["source_view"]),
            ],
            cwd=ROOT,
            check=True,
            timeout=60,
        )
    completed = subprocess.run(
        [
            sys.executable,
            str(TEXT_FUZZER.relative_to(ROOT)),
            "--seed",
            str(seed ^ 0x5A17),
            "--cases",
            str(counts["text"]),
            "--cc",
            compiler,
        ],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
        timeout=120,
    )
    if completed.returncode != 0:
        raise FuzzFailure(
            "text mutation fuzzer failed: "
            + (completed.stderr.strip() or completed.stdout.strip())
        )


def validate_manifest_mutations(generator: random.Random) -> None:
    manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    manifest_validator.validate_document(manifest)
    mutations = (
        ("format_version", "0.0.0"),
        ("manifest_id", "invalid"),
    )
    for key, value in mutations:
        invalid = copy.deepcopy(manifest)
        invalid[key] = value
        try:
            manifest_validator.validate_document(invalid)
        except manifest_validator.ManifestError:
            continue
        raise FuzzFailure(f"manifest mutation {key} was accepted")
    invalid = copy.deepcopy(manifest)
    generator.shuffle(invalid["cases"])
    if invalid["cases"] == manifest["cases"]:
        invalid["cases"].reverse()
    try:
        manifest_validator.validate_document(invalid)
    except manifest_validator.ManifestError:
        return
    raise FuzzFailure("unordered manifest mutation was accepted")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--cases", type=int, required=True)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    arguments = parser.parse_args()
    if arguments.seed < 0 or arguments.cases < 1 or arguments.cases > 1_000_000:
        parser.error("seed must be nonnegative and cases must be in 1..1000000")
    generator = random.Random(arguments.seed)
    try:
        validate_manifest_mutations(generator)
        counts = {"source": 0, "source_view": 0, "text": 0}
        names = tuple(counts)
        for index in range(arguments.cases):
            slot = index % len(names)
            counts[names[slot]] += 1
        run_native_mutations(arguments.seed, counts, arguments.cc)
    except (
        OSError,
        FuzzFailure,
        subprocess.CalledProcessError,
        subprocess.TimeoutExpired,
    ) as error:
        print(f"run_fuzz.py: {error}", file=sys.stderr)
        return 1
    print(
        json.dumps(
            {
                "cases": arguments.cases,
                "counts": counts,
                "seed": arguments.seed,
                "status": "pass",
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

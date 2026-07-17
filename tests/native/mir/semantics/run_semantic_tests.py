#!/usr/bin/env python3
"""Compile the W02 semantic binding and compare it with the frozen W01 model."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import subprocess
import tempfile
from pathlib import Path
from typing import Any, Iterable


REPO_ROOT = Path(__file__).resolve().parents[4]
MODEL_PATH = REPO_ROOT / "docs/native-engine/semantics/effects/effect-model.json"
SEMANTICS_DIR = REPO_ROOT / "Zend/Native/MIR/Semantics"
TEST_SOURCE = Path(__file__).with_name("test_semantic_runtime.c")
HEADER_TEST_SOURCE = Path(__file__).with_name("test_semantic_headers.cpp")


def mask(names: Iterable[str], catalog: list[str]) -> int:
    return sum(1 << catalog.index(name) for name in names)


def expected_rows(model: dict[str, Any]) -> list[list[str]]:
    catalog = model["catalog"]
    effects = catalog["effects"]
    domains = catalog["memory_domains"]
    states = catalog["ownership_states"]
    actions = catalog["ownership_actions"]
    predicates = catalog["predicates"]
    barriers = catalog["barriers"]
    guards = catalog["guard_facts"]
    rules = catalog["composition_rules"]
    rows: list[list[str]] = [["M", hashlib.sha256(MODEL_PATH.read_bytes()).hexdigest()]]

    for index, item in enumerate(model["atomic_effects"]):
        rows.append(["E", str(index), item["id"], str(1 << index),
                     str(mask(item["reads"], domains)), str(mask(item["writes"], domains)),
                     str(mask(item["barriers"], barriers))])
    rows.extend(["D", str(index), name] for index, name in enumerate(domains))
    for index, item in enumerate(model["ownership_states"]):
        rows.append(["S", str(index), item["id"], str(int(item["terminal"])),
                     str(int(item["cleanup_obligations"] == "exactly_one_release"))])
    for index, item in enumerate(model["ownership_actions"]):
        unchanged = item["source_after"] == "unchanged"
        source_after = 0 if unchanged else states.index(item["source_after"])
        has_result = item["result_state"] is not None
        result = states.index(item["result_state"]) if has_result else 0
        rows.append(["A", str(index), item["id"], str(mask(item["allowed_from"], states)),
                     str(int(unchanged)), str(source_after), str(int(has_result)), str(result),
                     str(mask(item["effects"], effects)), str(mask(item["reads"], domains)),
                     str(mask(item["writes"], domains)), str(mask(item["barriers"], barriers))])
    alias_kinds = {"may_alias": 0, "contains_reference": 1,
                   "indirect_access": 2, "shares_pointee": 3}
    for index, item in enumerate(model["alias_relations"]):
        rows.append(["L", str(index), str(domains.index(item["left"])),
                     str(domains.index(item["right"])), str(alias_kinds[item["kind"]])])
    for index, item in enumerate(model["predicates"]):
        rows.append(["P", str(index), item["id"], str(int(item["default_when_unproven"]))])
    rows.extend(["B", str(index), name] for index, name in enumerate(barriers))
    for index, item in enumerate(model["guard_facts"]):
        invalidated = item["invalidated_by"]
        rows.append(["G", str(index), item["id"], str(mask(item["stable_across"], effects)),
                     str(mask(invalidated["effects"], effects)),
                     str(mask(invalidated["writes"], domains)),
                     str(mask(invalidated["barriers"], barriers)),
                     str(mask(invalidated["predicates"], predicates))])
    normal_return = {"unchanged": 0, "permitted": 1, "forbidden": 2}
    for index, item in enumerate(model["composition_rules"]):
        when = item["when"]
        implies = item["implies"]
        rows.append(["R", str(index), item["id"],
                     str(mask(when["predicates"], predicates)),
                     str(mask(when["effects"], effects)), str(mask(when["actions"], actions)),
                     str(mask(when["barriers"], barriers)),
                     str(mask(implies["effects"], effects)), str(mask(implies["reads"], domains)),
                     str(mask(implies["writes"], domains)),
                     str(mask(implies["barriers"], barriers)),
                     str(normal_return[implies["normal_return"]]),
                     str(int(item["id"] == "unmodeled_internal_call"))])
    return rows


def compile_and_run(compiler: str) -> list[list[str]]:
    sources = [
        TEST_SOURCE,
        SEMANTICS_DIR / "zend_mir_semantic_catalog.c",
        SEMANTICS_DIR / "zend_mir_effect_summary.c",
        SEMANTICS_DIR / "zend_mir_alias.c",
        SEMANTICS_DIR / "zend_mir_ownership.c",
    ]
    with tempfile.TemporaryDirectory(prefix="zend-mir-semantics-") as directory:
        executable = Path(directory) / "test_semantic_runtime"
        compiler_command = shlex.split(compiler)
        common_flags = [
            "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(REPO_ROOT / "Zend/Native/MIR"),
            "-I", str(SEMANTICS_DIR),
        ]
        command = compiler_command + common_flags + [
            *map(str, sources), "-o", str(executable),
        ]
        subprocess.run(command, cwd=REPO_ROOT, check=True)
        objects = []
        for index, source in enumerate(sources[1:]):
            object_path = Path(directory) / f"semantic-{index}.o"
            subprocess.run(
                compiler_command + common_flags + ["-c", str(source), "-o", str(object_path)],
                cwd=REPO_ROOT, check=True,
            )
            objects.append(object_path)
        header_executable = Path(directory) / "test_semantic_headers"
        header_command = shlex.split(os.environ.get("CXX", "c++")) + [
            "-std=c++20", "-Wall", "-Wextra", "-Werror",
            "-I", str(REPO_ROOT / "Zend/Native/MIR"),
            "-I", str(SEMANTICS_DIR), str(HEADER_TEST_SOURCE),
            *map(str, objects), "-o", str(header_executable),
        ]
        subprocess.run(header_command, cwd=REPO_ROOT, check=True)
        subprocess.run([str(header_executable)], cwd=REPO_ROOT, check=True)
        output = subprocess.run(
            [str(executable)], cwd=REPO_ROOT, check=True,
            text=True, stdout=subprocess.PIPE,
        ).stdout
    return [line.split("\t") for line in output.splitlines()]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="cc", help="C compiler command")
    arguments = parser.parse_args()
    model = json.loads(MODEL_PATH.read_text(encoding="utf-8"))
    actual = compile_and_run(arguments.cc)
    expected = expected_rows(model)
    if actual != expected:
        for index, (expected_row, actual_row) in enumerate(zip(expected, actual)):
            if expected_row != actual_row:
                raise SystemExit(
                    f"catalog mismatch at row {index}: expected {expected_row}, got {actual_row}"
                )
        raise SystemExit(f"catalog row count differs: expected {len(expected)}, got {len(actual)}")
    print(f"W02 semantic runtime passes ({len(actual)} frozen catalog rows)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Validate the W05-v2 serial phase ownership manifest."""

from __future__ import annotations

import argparse
from fnmatch import fnmatchcase
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "docs/native-engine/calls/w05-phase-manifest.json"
EXPECTED = (
    "W05-v2-contract",
    "W05-v2-wave-pin",
    "W05-v2-implementation",
    "W05-v2-gate",
    "W05-v2-seal",
)


def overlaps(left: str, right: str) -> bool:
    def prefix(pattern: str) -> str | None:
        return pattern[:-3].rstrip("/") if pattern.endswith("/**") else None

    lp, rp = prefix(left), prefix(right)
    if left == right:
        return True
    if lp is not None and rp is not None:
        return lp == rp or lp.startswith(rp + "/") or rp.startswith(lp + "/")
    if lp is not None:
        return right.startswith(lp + "/") or fnmatchcase(right, left)
    if rp is not None:
        return left.startswith(rp + "/") or fnmatchcase(left, right)
    return fnmatchcase(left, right) or fnmatchcase(right, left)


def validate(data: dict[str, object]) -> list[str]:
    errors: list[str] = []
    if data.get("format_version") != 2:
        errors.append("format_version must be 2")
    if data.get("wave") != "W05-v2":
        errors.append("wave must be W05-v2")
    if data.get("start_commit") != "31cac4a51b51c91ad9e25b72c9312775405e2cf4":
        errors.append("start_commit drift")
    if data.get("writer_branch") != "integration/wave-06":
        errors.append("writer branch drift")
    phases = data.get("phases")
    if not isinstance(phases, list):
        return errors + ["phases must be an array"]
    ids = tuple(p.get("phase_id") for p in phases if isinstance(p, dict))
    if ids != EXPECTED:
        errors.append("phase order drift")
        return errors
    for index, phase in enumerate(phases):
        if phase.get("order") != index:
            errors.append(f"{phase['phase_id']}: order drift")
        paths = phase.get("paths")
        if not isinstance(paths, list) or not paths:
            errors.append(f"{phase['phase_id']}: paths must be non-empty")
    for i, left_phase in enumerate(phases):
        for right_phase in phases[i + 1:]:
            for left in left_phase["paths"]:
                for right in right_phase["paths"]:
                    if overlaps(left, right):
                        errors.append(
                            f"phase path overlap: {left_phase['phase_id']}:{left} "
                            f"and {right_phase['phase_id']}:{right}"
                        )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if not args.check:
        parser.error("--check is required")
    try:
        data = json.loads(MANIFEST.read_text(encoding="utf-8"))
        errors = validate(data)
    except (OSError, json.JSONDecodeError) as exc:
        errors = [str(exc)]
    if errors:
        for error in errors:
            print(f"W05-v2 phase manifest invalid: {error}", file=sys.stderr)
        return 1
    print("W05-v2 phase manifest valid: five linear writing phases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

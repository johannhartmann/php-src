#!/usr/bin/env python3
"""Render the generated config.m4 source block from the global manifest."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "docs/native-engine/build/native-source-manifest.json"
W04_MANIFEST = ROOT / "docs/native-engine/control-flow/w04-source-files.json"
CONFIG = ROOT / "ext/native_mir_test/config.m4"
BEGIN = "  dnl BEGIN GENERATED NATIVE SOURCES"
END = "  dnl END GENERATED NATIVE SOURCES"
GROUPS = ("mir", "lowering", "control_flow", "calls")


def rendered() -> str:
    data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if (
        data.get("format_version") != 1
        or data.get("consumer") != "ext/native_mir_test/config.m4"
        or tuple(data.get("groups", {})) != GROUPS
    ):
        raise SystemExit("native source manifest envelope or group order is invalid")
    paths = [path for group in GROUPS for path in data["groups"][group]]
    if len(paths) != len(set(paths)):
        raise SystemExit("native source manifest contains duplicate paths")
    if any(
        not isinstance(path, str)
        or not path.startswith("Zend/Native/")
        or not path.endswith(".c")
        for path in paths
    ):
        raise SystemExit("native source manifest contains an invalid source path")
    missing = [path for path in paths if not (ROOT / path).is_file()]
    if missing:
        raise SystemExit(f"native source manifest has missing paths: {missing}")
    historical = json.loads(W04_MANIFEST.read_text(encoding="utf-8"))
    inherited = (
        historical["existing_production_sources"]
        + historical["w04_production_sources"]
    )
    if paths[:-len(data["groups"]["calls"])] != inherited:
        raise SystemExit("native source manifest drifted from the live W04 source list")
    if data["groups"]["calls"] != [
        "Zend/Native/Calls/Model/zend_mir_call_model.c"
    ]:
        raise SystemExit("native source manifest has an unowned W05 call source")
    lines = [BEGIN]
    for path in paths:
        directory, filename = path.rsplit("/", 1)
        lines.append(f"  PHP_ADD_BUILD_DIR([{directory}])")
        macro = (
            "PHP_ADD_SOURCES_X"
            if path in data["groups"]["control_flow"]
            or path in data["groups"]["calls"]
            else "PHP_ADD_SOURCES"
        )
        suffix = ",, [PHP_GLOBAL_OBJS]" if macro.endswith("_X") else ""
        lines.append(
            f"  {macro}([{directory}], [{filename}]{suffix})"
        )
    lines.append(END)
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    text = CONFIG.read_text()
    start = text.index(BEGIN)
    end = text.index(END, start) + len(END)
    expected = text[:start] + rendered() + text[end:]
    if args.check:
        if text != expected:
            raise SystemExit("config.m4 native source block is stale")
        print("native test source block: ok")
    else:
        CONFIG.write_text(expected)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

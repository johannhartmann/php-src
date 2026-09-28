#!/usr/bin/env python3
"""Time each Zend/bench.php kernel on a candidate and a reference PHP.

Every kernel runs in its own process: the bench.php function definitions
are loaded, the kernel is called once to compile and warm it, then timed
over repeated calls. The best of several runs is reported, with the
candidate/reference ratio (lower is faster) and, with --perf, the
instruction and branch counts of one timed run.

Exit codes follow docs/native-engine/test-command-contract.md: 0 success,
2 usage error, 3 missing prerequisite, 1 other failure.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BENCH = ROOT / "Zend" / "bench.php"
OPCACHE_ARGS = [
    "-d", "opcache.enable_cli=1",
    "-d", "opcache.file_update_protection=0",
    "-d", "memory_limit=-1",
]


def kernels() -> list[tuple[str, str]]:
    """Return (name, call) for every timed call in bench.php's main part."""
    source = BENCH.read_text()
    main = source[source.index("$t0 = $t = start_test();"):]
    return [(call.split("(")[0], call)
            for call in re.findall(r"^(\w+\([^)]*\));$", main, re.M)
            if not call.startswith(("total(", "start_test("))]


def definitions() -> str:
    source = BENCH.read_text()
    return source[:source.index("$t0 = $t = start_test();")]


def driver(call: str, repeat: int) -> str:
    return (definitions()
            + "ob_start();\n"
            + f"{call};\n"
            + "$best = INF;\n"
            + f"for ($r = 0; $r < {repeat}; $r++) {{\n"
            + "    $t = hrtime(true);\n"
            + f"    {call};\n"
            + "    $best = min($best, hrtime(true) - $t);\n"
            + "}\n"
            + "ob_end_clean();\n"
            + "fwrite(STDERR, \"BEST \" . $best . \"\\n\");\n")


def run(php: str, script: Path, perf: bool) -> tuple[float, dict[str, int]]:
    env = {k: v for k, v in os.environ.items()
           if k not in ("PHP_INI_SCAN_DIR", "PHPRC")}
    command = [php, "-n", *OPCACHE_ARGS, str(script)]
    counters: dict[str, int] = {}
    if perf:
        command = ["perf", "stat", "-x", ",", "-e",
                   "instructions:u,branches:u,branch-misses:u", *command]
    result = subprocess.run(command, env=env, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"{php} failed on {script.name}: "
                           f"{result.stderr.strip()[-500:]}")
    best = None
    for line in result.stderr.splitlines():
        if line.startswith("BEST "):
            best = float(line.split()[1]) / 1e9
        elif perf and "," in line:
            fields = line.split(",")
            if fields[0].isdigit():
                counters[fields[2].split(":")[0]] = int(fields[0])
    if best is None:
        raise RuntimeError(f"{php} printed no timing for {script.name}")
    return best, counters


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--reference", required=True)
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--kernel", action="append", default=[],
                        help="run only this kernel; may be repeated")
    parser.add_argument("--perf", action="store_true",
                        help="also count instructions and branches")
    parser.add_argument("--json", action="store_true")
    try:
        args = parser.parse_args()
    except SystemExit as exit:
        return 2 if exit.code else 0
    for php in (args.candidate, args.reference):
        if not os.access(php, os.X_OK):
            print(f"not executable: {php}", file=sys.stderr)
            return 3
    if args.perf and shutil.which("perf") is None:
        print("perf is not on PATH", file=sys.stderr)
        return 3
    selected = [(name, call) for name, call in kernels()
                if not args.kernel or name in args.kernel]
    if not selected:
        print("no kernel selected", file=sys.stderr)
        return 2

    rows = []
    with tempfile.TemporaryDirectory() as directory:
        for name, call in selected:
            script = Path(directory) / f"{name}.php"
            script.write_text(driver(call, args.repeat))
            try:
                candidate, candidate_counts = run(
                    args.candidate, script, args.perf)
                reference, reference_counts = run(
                    args.reference, script, args.perf)
            except RuntimeError as error:
                print(error, file=sys.stderr)
                return 1
            rows.append({
                "kernel": call,
                "candidate_s": candidate,
                "reference_s": reference,
                "ratio": candidate / reference if reference else math.inf,
                "candidate_counters": candidate_counts,
                "reference_counters": reference_counts,
            })

    if args.json:
        json.dump(rows, sys.stdout, indent=2)
        print()
        return 0
    header = f"{'kernel':18} {'candidate':>10} {'reference':>10} {'ratio':>6}"
    if args.perf:
        header += f" {'cand instr':>13} {'ref instr':>13}"
    print(header)
    for row in rows:
        line = (f"{row['kernel']:18} {row['candidate_s']*1e3:8.2f}ms "
                f"{row['reference_s']*1e3:8.2f}ms {row['ratio']:6.2f}")
        if args.perf:
            line += (f" {row['candidate_counters'].get('instructions', 0):13d}"
                     f" {row['reference_counters'].get('instructions', 0):13d}")
        print(line)
    ratios = [row["ratio"] for row in rows]
    geomean = math.exp(sum(math.log(r) for r in ratios) / len(ratios))
    print(f"{'geomean ratio':18} {'':>21} {geomean:6.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

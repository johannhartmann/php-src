"""Make the mandatory contracts-root discovery include every W01 suite."""

import importlib.util
from pathlib import Path
import sys
import unittest


sys.dont_write_bytecode = True

CONTRACTS = Path(__file__).resolve().parent.parent
SPECIALIST_SUITES = ("opcodes", "effects", "frames", "tpde", "corpus")


def load_tests(loader, tests, pattern):
    combined = unittest.TestSuite()
    for suite_name in SPECIALIST_SUITES:
        suite_dir = CONTRACTS / suite_name
        for test_path in sorted(suite_dir.glob(pattern or "test_*.py")):
            module_name = f"w01_{suite_name}_{test_path.stem}"
            spec = importlib.util.spec_from_file_location(module_name, test_path)
            if spec is None or spec.loader is None:
                raise ImportError(f"cannot load {test_path}")
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            combined.addTests(loader.loadTestsFromModule(module))
    return combined

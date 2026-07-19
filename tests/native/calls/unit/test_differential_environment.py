from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
RUNNER_PATH = ROOT / "scripts/native/calls/run-w05-differential.py"


def load_runner() -> Any:
    specification = importlib.util.spec_from_file_location(
        "w05_differential_environment_test", RUNNER_PATH
    )
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


class DifferentialEnvironmentTests(unittest.TestCase):
    def test_execute_pins_locale_timezone_and_epoch(self) -> None:
        runner = load_runner()
        with tempfile.TemporaryDirectory(prefix="w05-env-test-") as directory:
            binary = Path(directory) / "fake-php"
            source = Path(directory) / "source.php"
            binary.write_text(
                "#!/bin/sh\n"
                "printf '%s|%s|%s|%s' \"$LANG\" \"$LC_ALL\" \"$TZ\" "
                "\"$SOURCE_DATE_EPOCH\"\n",
                encoding="utf-8",
            )
            binary.chmod(0o755)
            source.write_text("<?php\n", encoding="utf-8")
            self.assertEqual(
                runner.execute(binary, source, 5.0),
                b"0\0C|C|UTC|0",
            )


if __name__ == "__main__":
    unittest.main()

from __future__ import annotations

import importlib.util
import os
import tempfile
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
DUMP_PATH = ROOT / "scripts/native/calls/dump-w05.py"


def load_dump_module() -> Any:
    specification = importlib.util.spec_from_file_location(
        "w05_dump_test", DUMP_PATH
    )
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


dump_w05 = load_dump_module()

FAKE_PHP = r"""#!/usr/bin/env python3
import json
import re
import sys

program = sys.argv[-1]
match = re.search(r"\$repeat=(\d+);", program)
repeat = int(match.group(1)) if match else 1
result = {
    "schema_version": 1,
    "wave": 5,
    "status": "accepted",
    "phase": "complete",
    "source": {
        "filename": "fixture.php",
        "byte_length": 0,
        "source_id": "fnv1a64:0000000000000000",
    },
    "diagnostics": [{"stage": "MIRL", "code": "MIRL0000", "message": "ok", "opline": None}],
    "mir": "instruction i0 block b0 opcode call_direct_user\ncall-receipt cr0 modeled true codegen-eligible false\n",
}
print(json.dumps({"schema_version": 1, "calls": [result for _ in range(repeat)]}))
"""


class DumpW05Tests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="w05-dump-test-")
        self.candidate = Path(self.temporary.name) / "candidate php"
        self.candidate.write_text(FAKE_PHP, encoding="utf-8")
        self.candidate.chmod(0o755)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_repeat_one_through_ten_is_bounded_and_attributed(self) -> None:
        document = dump_w05.invoke(
            self.candidate,
            b"<?php function fixture(): void {}",
            "fixture.php",
            function="fixture",
            repeat=10,
        )
        self.assertEqual(len(document["calls"]), 10)
        self.assertTrue(
            all(call["diagnostics"][0]["code"] == "MIRL0000"
                for call in document["calls"])
        )

    def test_candidate_and_repeat_must_be_valid(self) -> None:
        with self.assertRaisesRegex(dump_w05.DumpError, "executable"):
            dump_w05.candidate_path("missing-php")
        for repeat in (0, 11):
            with self.subTest(repeat=repeat):
                with self.assertRaisesRegex(dump_w05.DumpError, "repeat"):
                    dump_w05.invoke(
                        self.candidate, b"<?php", "fixture.php", repeat=repeat
                    )

    def test_filename_is_encoded_as_data(self) -> None:
        marker = Path(self.temporary.name) / "NOT_EXECUTED"
        dump_w05.invoke(
            self.candidate,
            b"<?php",
            f"fixture.php;touch {marker}",
        )
        self.assertFalse(marker.exists())


if __name__ == "__main__":
    unittest.main()

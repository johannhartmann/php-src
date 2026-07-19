"""Unit tests for the W04 compile/dump channel."""

from __future__ import annotations

import importlib.util
import os
import tempfile
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
DUMP_PATH = ROOT / "scripts/native/control-flow/dump-w04.py"


def load_dump_module() -> Any:
    specification = importlib.util.spec_from_file_location("w04_dump_test", DUMP_PATH)
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


dump_w04 = load_dump_module()


FAKE_PHP = r"""#!/usr/bin/env python3
import base64
import copy
import json
import os
import sys

def source_id(filename, source):
    value = 0xcbf29ce484222325
    for byte in filename.encode("utf-8") + b"\0" + source:
        value ^= byte
        value = (value * 0x100000001b3) & 0xffffffffffffffff
    return "fnv1a64:{:016x}".format(value)

request = json.loads(sys.stdin.buffer.read())
assert request["options"]["wave"] == 4
source = base64.b64decode(request["source_base64"], validate=True)
filename = request["filename"]
result = {
    "diagnostics": [
        {"code": "MIRL0000", "message": "W04 lowering completed", "opline": None, "stage": "MIRL"}
    ],
    "mir": "znmir 1.3\nmodule m0\n",
    "phase": "complete",
    "schema_version": 1,
    "source": {
        "byte_length": len(source),
        "filename": filename,
        "source_id": source_id(filename, source),
    },
    "status": "accepted",
    "wave": 4,
}
mode = os.environ.get("W04_FAKE_MODE")
if mode == "deferred":
    result.update({
        "diagnostics": [{"code": "MIRL0011", "message": "not implemented", "opline": 2, "stage": "MIRL"}],
        "mir": None,
        "phase": "lowering",
        "status": "rejected",
    })
elif mode == "compile_error":
    result.update({
        "diagnostics": [{"code": "COMPILE_ERROR", "message": "compile failed", "opline": None, "stage": "compile"}],
        "mir": None,
        "phase": "compile",
        "status": "error",
    })
elif mode == "bailout":
    result.update({
        "diagnostics": [{"code": "BAILOUT", "message": "compile bailed out", "opline": None, "stage": "compile"}],
        "mir": None,
        "phase": "compile",
        "status": "error",
    })
elif mode == "missing_wave":
    del result["wave"]
elif mode == "wrong_wave":
    result["wave"] = 3
elif mode == "too_many_diagnostics":
    result["diagnostics"] = result["diagnostics"] * (request["options"]["diagnostic_limit"] + 1)
calls = [copy.deepcopy(result) for _ in range(request["repeat"])]
if mode == "vary" and len(calls) > 1:
    calls[1]["diagnostics"][0]["message"] = "changed"
print(json.dumps({"calls": calls, "schema_version": 1}, sort_keys=True))
"""


class DumpW04Tests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="w04-dump-test-")
        self.root = Path(self.temporary.name)
        self.candidate = self.root / "candidate php"
        self.candidate.write_text(FAKE_PHP, encoding="utf-8")
        self.candidate.chmod(0o755)
        self.source = b"<?php function branch(int $x): int { return $x ? 1 : 2; }\n"
        self.filename = "source;touch NOT_EXECUTED.php"

    def tearDown(self) -> None:
        os.environ.pop("W04_FAKE_MODE", None)
        self.temporary.cleanup()

    def test_calls_one_through_ten_are_individually_attributed(self) -> None:
        document, exit_code = dump_w04.invoke(
            str(self.candidate),
            self.source,
            self.filename,
            repeat=10,
            timeout=5.0,
        )
        self.assertEqual(exit_code, dump_w04.EXIT_ACCEPTED)
        self.assertEqual(
            [call["call"] for call in document["calls"]],
            list(range(1, 11)),
        )
        self.assertEqual(document["wave"], 4)
        self.assertEqual(document["normalization"], {"enabled": False, "rules": []})
        self.assertFalse((self.root / "NOT_EXECUTED").exists())
        self.assertFalse((ROOT / "NOT_EXECUTED").exists())

    def test_deferred_result_retains_mirl_and_opline_without_partial_mir(self) -> None:
        os.environ["W04_FAKE_MODE"] = "deferred"
        document, exit_code = dump_w04.invoke(
            str(self.candidate), self.source, self.filename
        )
        self.assertEqual(exit_code, dump_w04.EXIT_REJECTED)
        result = document["calls"][0]["result"]
        self.assertEqual(result["diagnostics"][0]["code"], "MIRL0011")
        self.assertEqual(result["diagnostics"][0]["opline"], 2)
        self.assertIsNone(result["mir"])

    def test_compile_error_and_bailout_are_structured(self) -> None:
        for mode, code in (("compile_error", "COMPILE_ERROR"), ("bailout", "BAILOUT")):
            with self.subTest(mode=mode):
                os.environ["W04_FAKE_MODE"] = mode
                document, exit_code = dump_w04.invoke(
                    str(self.candidate), self.source, self.filename
                )
                self.assertEqual(exit_code, dump_w04.EXIT_ERROR)
                self.assertEqual(
                    document["calls"][0]["result"]["diagnostics"][0]["code"],
                    code,
                )

    def test_invalid_or_missing_wave_is_rejected(self) -> None:
        for mode in ("missing_wave", "wrong_wave"):
            with self.subTest(mode=mode):
                os.environ["W04_FAKE_MODE"] = mode
                with self.assertRaisesRegex(dump_w04.DumpError, "wave"):
                    dump_w04.invoke(str(self.candidate), self.source, self.filename)

    def test_diagnostic_limit_is_enforced(self) -> None:
        os.environ["W04_FAKE_MODE"] = "too_many_diagnostics"
        with self.assertRaisesRegex(dump_w04.DumpError, "diagnostic_limit"):
            dump_w04.invoke(
                str(self.candidate),
                self.source,
                self.filename,
                diagnostic_limit=1,
            )
        for invalid in (0, 257, True):
            with self.subTest(limit=invalid):
                with self.assertRaisesRegex(dump_w04.DumpError, "diagnostic limit"):
                    dump_w04.validate_request(self.filename, 1, invalid, 4096, 5.0)

    def test_invalid_function_result_and_non_determinism_are_failure_atomic(self) -> None:
        os.environ["W04_FAKE_MODE"] = "deferred"
        document, _ = dump_w04.invoke(
            str(self.candidate),
            self.source,
            self.filename,
            function="missing_function",
        )
        self.assertIsNone(document["calls"][0]["result"]["mir"])
        os.environ["W04_FAKE_MODE"] = "vary"
        with self.assertRaisesRegex(dump_w04.DumpError, "byte-for-byte"):
            dump_w04.invoke(
                str(self.candidate),
                self.source,
                self.filename,
                repeat=2,
            )

    def test_candidate_and_repeat_are_explicit_and_bounded(self) -> None:
        with self.assertRaisesRegex(dump_w04.DumpError, "must be absolute"):
            dump_w04.invoke("php", self.source, self.filename)
        for repeat in (0, 11, True):
            with self.subTest(repeat=repeat):
                with self.assertRaisesRegex(dump_w04.DumpError, "repeat"):
                    dump_w04.validate_request(self.filename, repeat, 32, 4096, 5.0)


if __name__ == "__main__":
    unittest.main()

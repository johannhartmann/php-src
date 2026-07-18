"""Unit tests for the W03 compile/dump CLI bridge."""

from __future__ import annotations

import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
DUMP_PATH = ROOT / "scripts/native/lowering/dump-mir.py"


def load_dump_module() -> Any:
    specification = importlib.util.spec_from_file_location("w03_dump_test", DUMP_PATH)
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


dump_mir = load_dump_module()


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
source = base64.b64decode(request["source_base64"], validate=True)
filename = request["filename"]
result = {
    "diagnostics": [
        {"code": "MIRL0000", "message": "ok", "opline": 0, "stage": "MIRL"}
    ],
    "mir": "znmir 1.0\nmodule m0\nflags 0x00000000\n",
    "phase": "complete",
    "schema_version": 1,
    "source": {
        "byte_length": len(source),
        "filename": filename,
        "source_id": source_id(filename, source),
    },
    "status": "accepted",
}
mode = os.environ.get("W03_FAKE_MODE")
if mode == "address":
    result["diagnostics"][0]["message"] = "pointer=0x123456789abc"
elif mode == "identity":
    result["source"]["source_id"] = "fnv1a64:0000000000000000"
elif mode == "vary":
    result["source"]["source_id"] = source_id(filename, source)
elif mode == "mir_address":
    result["mir"] += "pointer=0x123456789abc\n"
elif mode == "no_diagnostics":
    result["diagnostics"] = []
elif mode == "diagnostic_order":
    result["diagnostics"] = [
        {"code": "MIRL0000", "message": "z-last", "opline": 0, "stage": "MIRL"},
        {"code": "MIRL0000", "message": "a-first", "opline": 0, "stage": "MIRL"},
    ]
calls = [copy.deepcopy(result) for _ in range(request["repeat"])]
if mode == "vary" and len(calls) > 1:
    calls[1]["diagnostics"][0]["message"] = "changed but still valid"
print(json.dumps({"calls": calls, "schema_version": 1}, sort_keys=True))
"""


class DumpMirTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="w03-dump-test-")
        self.root = Path(self.temporary.name)
        self.candidate = self.root / "candidate php"
        self.candidate.write_text(FAKE_PHP, encoding="utf-8")
        self.candidate.chmod(0o755)
        self.source = b"<?php function f(): int { return 1; }\n"
        self.filename = "source;touch NOT_EXECUTED.php"

    def tearDown(self) -> None:
        os.environ.pop("W03_FAKE_MODE", None)
        self.temporary.cleanup()

    def test_repeated_dump_is_exact_and_shell_free(self) -> None:
        document, exit_code = dump_mir.invoke(
            str(self.candidate),
            self.source,
            self.filename,
            repeat=10,
            timeout=5.0,
        )
        self.assertEqual(exit_code, dump_mir.EXIT_ACCEPTED)
        self.assertEqual(document["repeat"], 10)
        self.assertEqual(
            {call["mir_sha256"] for call in document["calls"]},
            {document["calls"][0]["mir_sha256"]},
        )
        self.assertFalse((ROOT / "NOT_EXECUTED").exists())
        self.assertFalse((self.root / "NOT_EXECUTED").exists())

    def test_candidate_must_be_an_absolute_explicit_executable(self) -> None:
        with self.assertRaisesRegex(dump_mir.DumpError, "must be absolute"):
            dump_mir.invoke("php", self.source, self.filename)

    def test_source_identity_is_checked_against_bytes(self) -> None:
        os.environ["W03_FAKE_MODE"] = "identity"
        with self.assertRaisesRegex(dump_mir.DumpError, "does not match"):
            dump_mir.invoke(str(self.candidate), self.source, self.filename)

    def test_metadata_addresses_are_rejected_but_canonical_hex_is_allowed(self) -> None:
        document, _ = dump_mir.invoke(
            str(self.candidate), self.source, self.filename
        )
        self.assertIn("flags 0x00000000", document["calls"][0]["result"]["mir"])
        os.environ["W03_FAKE_MODE"] = "address"
        with self.assertRaisesRegex(dump_mir.DumpError, "address-like"):
            dump_mir.invoke(str(self.candidate), self.source, self.filename)
        os.environ["W03_FAKE_MODE"] = "mir_address"
        with self.assertRaisesRegex(dump_mir.DumpError, "address-like"):
            dump_mir.invoke(str(self.candidate), self.source, self.filename)

    def test_accepted_result_requires_success_diagnostic(self) -> None:
        os.environ["W03_FAKE_MODE"] = "no_diagnostics"
        with self.assertRaisesRegex(dump_mir.DumpError, "MIRL0000"):
            dump_mir.invoke(str(self.candidate), self.source, self.filename)

    def test_non_deterministic_repetition_is_rejected(self) -> None:
        os.environ["W03_FAKE_MODE"] = "vary"
        with self.assertRaisesRegex(dump_mir.DumpError, "byte-for-byte"):
            dump_mir.invoke(
                str(self.candidate),
                self.source,
                self.filename,
                repeat=2,
            )

    def test_diagnostics_require_complete_canonical_order(self) -> None:
        os.environ["W03_FAKE_MODE"] = "diagnostic_order"
        with self.assertRaisesRegex(dump_mir.DumpError, "canonically ordered"):
            dump_mir.invoke(str(self.candidate), self.source, self.filename)

    def test_rejects_unknown_result_fields(self) -> None:
        result = {
            "diagnostics": [],
            "mir": None,
            "phase": "lowering",
            "schema_version": 1,
            "source": {
                "byte_length": len(self.source),
                "filename": self.filename,
                "source_id": dump_mir.source_identity(self.filename, self.source),
            },
            "status": "rejected",
            "unexpected": True,
        }
        with self.assertRaisesRegex(dump_mir.DumpError, "unknown keys"):
            dump_mir.validate_call_result(result, self.filename, self.source)


if __name__ == "__main__":
    unittest.main()

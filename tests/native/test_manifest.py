#!/usr/bin/env python3

from __future__ import annotations

import shutil
import unittest

from capture_manifest import capture_manifest
from schema_validation import validate_manifest


PHP = shutil.which("php")


@unittest.skipIf(PHP is None, "system PHP is required")
class ManifestTests(unittest.TestCase):
    def test_capture_contains_binary_php_host_and_provenance(self) -> None:
        manifest = capture_manifest(PHP)
        validate_manifest(manifest)
        self.assertEqual(64, len(manifest["binary"]["sha256"]))
        self.assertGreater(manifest["binary"]["size_bytes"], 0)
        self.assertIn(manifest["php"]["facts"]["thread_safety"], ("NTS", "ZTS"))
        self.assertIn("Server API", manifest["php"]["info"])
        self.assertIn("value", manifest["binary"]["elf_build_id"])
        self.assertIn("kernel", manifest["host"])
        self.assertIn("commit", manifest["provenance"])


if __name__ == "__main__":
    unittest.main()

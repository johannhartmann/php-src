import copy
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest


sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[5]
SCRIPT = ROOT / "scripts/native/semantics/validate-w01.py"

SPEC = importlib.util.spec_from_file_location("validate_w01", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
validate_w01 = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(validate_w01)


class W01IntegrationTests(unittest.TestCase):
    def make_artifact_root(self, temporary):
        root = Path(temporary)
        for relative in validate_w01.ARTIFACTS.values():
            source = ROOT / relative
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
        return root

    def mutate_json(self, root, artifact_name, callback):
        path = root / validate_w01.ARTIFACTS[artifact_name]
        document = json.loads(path.read_text(encoding="utf-8"))
        callback(document)
        path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    def assert_invalid_mutation(self, artifact_name, callback, expected):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_artifact_root(temporary)
            self.mutate_json(root, artifact_name, callback)
            with self.assertRaisesRegex(validate_w01.ValidationError, expected):
                validate_w01.build_report(root)

    def test_checked_in_report_is_current_and_deterministic(self):
        first = validate_w01.render_report(validate_w01.build_report(ROOT))
        second = validate_w01.render_report(validate_w01.build_report(ROOT))
        checked_in = (ROOT / validate_w01.REPORT_PATH).read_text(encoding="utf-8")
        self.assertEqual(first.encode("utf-8"), second.encode("utf-8"))
        self.assertEqual(first, checked_in)
        self.assertNotIn("timestamp", first)

    def test_missing_opcode_fails_closed(self):
        self.assert_invalid_mutation(
            "opcodes",
            lambda document: document["opcodes"].pop(),
            "does not cover every executable opcode",
        )

    def test_unregistered_effect_fails_closed(self):
        self.assert_invalid_mutation(
            "effects",
            lambda document: document["atomic_effects"].__setitem__(
                slice(None),
                [effect for effect in document["atomic_effects"] if effect["id"] != "read_memory"],
            ),
            "effects omit frozen IDs",
        )

    def test_missing_frame_safepoint_fails_closed(self):
        def remove_observer(document):
            safepoint = document["properties"]["safepoint"]["properties"]["class"]["enum"]
            safepoint.remove("observer")

        self.assert_invalid_mutation("frame_schema", remove_observer, "frame schema omits safepoints")

    def test_missing_tpde_capability_fails_closed(self):
        self.assert_invalid_mutation(
            "tpde",
            lambda document: document["capabilities"].__setitem__(
                slice(None),
                [capability for capability in document["capabilities"] if capability["id"] != "statepoint-code-offset"],
            ),
            "TPDE analysis omits integration capabilities",
        )

    def test_uncovered_manifest_family_fails_closed(self):
        def remove_family_coverage(document):
            for fixture in document["fixtures"]:
                fixture["opcode_families"] = [
                    family for family in fixture["opcode_families"] if family != "strings"
                ]

        self.assert_invalid_mutation("manifest", remove_family_coverage, "opcode families lack fixtures")

    def test_unresolved_critical_blocker_fails_closed(self):
        def add_blocker(document):
            document["blockers"].append({
                "affected_waves": ["W02"],
                "blocker_id": "W01-BLOCK-001",
                "decision": "Choose and validate a concrete frame-state publication API.",
                "github_issue_url": "https://github.com/php/php-src/issues/1",
                "issue_ready": True,
                "owner": "native engine",
                "severity": "critical",
                "source_refs": ["docs/native-engine/semantics/frames/safepoint-contract.md"],
                "status": "unresolved",
                "title": "Synthetic unresolved critical blocker",
            })

        self.assert_invalid_mutation("blockers", add_blocker, "critical W01 blockers remain unresolved")

    def test_placeholder_in_normative_artifact_fails_closed(self):
        def add_placeholder(document):
            document["opcodes"][0]["reason"] = "TBD"

        self.assert_invalid_mutation("opcodes", add_placeholder, "semantic placeholder")


if __name__ == "__main__":
    unittest.main()

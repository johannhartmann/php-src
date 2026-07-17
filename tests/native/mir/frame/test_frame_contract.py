from __future__ import annotations

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[4]
FRAME = ROOT / "Zend" / "Native" / "MIR" / "Frame"


class FrameContractTests(unittest.TestCase):
    def test_persistent_record_has_no_raw_pointer(self) -> None:
        header = (FRAME / "zend_mir_frame_state.h").read_text(encoding="utf-8")
        match = re.search(
            r"typedef struct _zend_mir_frame_state_record \{(?P<body>.*?)\} zend_mir_frame_state_record;",
            header,
            re.DOTALL,
        )
        self.assertIsNotNone(match)
        self.assertNotIn("*", match.group("body"))

    def test_no_machine_or_runtime_location_contract(self) -> None:
        sources = "\n".join(
            path.read_text(encoding="utf-8")
            for path in sorted(FRAME.glob("*.[ch]"))
        )
        for forbidden in (
            "machine_offset",
            "vm_dispatch",
            "zend_execute_data",
            "zend_op *",
            "register_location",
            "stackmap",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, sources)

    def test_source_map_uses_stable_owner_and_opline_identity(self) -> None:
        header = (ROOT / "Zend" / "Native" / "MIR" / "zend_mir_frame_state.h").read_text(
            encoding="utf-8"
        )
        for field in (
            "source_position_id",
            "op_array_id",
            "opline_index",
            "opline_phase",
            "owner_frame_id",
        ):
            self.assertIn(field, header)


if __name__ == "__main__":
    unittest.main()

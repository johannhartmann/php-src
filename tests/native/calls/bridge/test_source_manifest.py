from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[4]
GENERATOR = ROOT / "scripts/native/build/update-native-test-sources.py"


def load_generator() -> Any:
    specification = importlib.util.spec_from_file_location(
        "native_source_generator_test", GENERATOR
    )
    assert specification is not None and specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


generator = load_generator()


class SourceManifestTests(unittest.TestCase):
    def test_render_is_deterministic_and_has_one_w05_source(self) -> None:
        first = generator.rendered()
        second = generator.rendered()
        self.assertEqual(first, second)
        self.assertEqual(
            first.count(
                "PHP_ADD_SOURCES_X([Zend/Native/Calls/Model], "
                "[zend_mir_call_model.c],, [PHP_GLOBAL_OBJS])"
            ),
            1,
        )

    def test_checked_in_block_matches_render(self) -> None:
        config = generator.CONFIG.read_text(encoding="utf-8")
        start = config.index(generator.BEGIN)
        end = config.index(generator.END, start) + len(generator.END)
        self.assertEqual(config[start:end], generator.rendered())


if __name__ == "__main__":
    unittest.main()

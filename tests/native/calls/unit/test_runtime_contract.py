from __future__ import annotations

import re
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
ABI_SOURCE = Path(__file__).with_name("test_runtime_abi.c")
CALLS_SOURCE = ROOT / "Zend/Native/Runtime/Common/zend_native_calls.c"
RUNTIME_SOURCE = ROOT / "Zend/Native/Runtime/Common/zend_native_runtime.c"
TPDE_BACKEND = ROOT / "Zend/Native/TPDE/Common/zend_tpde_backend.cpp"


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


class RuntimeContractTests(unittest.TestCase):
    def test_v80_abi_static_asserts_compile(self) -> None:
        include_dirs = (
            ".",
            "main",
            "Zend",
            "TSRM",
            "ext/date/lib",
            "ext/lexbor",
            "Zend/Native/TPDE/ThirdParty/tpde/include",
            "Zend/Native/TPDE/ThirdParty/tpde/fadec",
            "Zend/Native/TPDE/ThirdParty/tpde/disarm",
        )
        with tempfile.TemporaryDirectory(
            prefix="native-runtime-abi-"
        ) as directory:
            output = Path(directory) / "test_runtime_abi.o"
            subprocess.run(
                [
                    "cc",
                    "-std=gnu23",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    *(f"-I{path}" for path in include_dirs),
                    "-c",
                    str(ABI_SOURCE),
                    "-o",
                    str(output),
                ],
                cwd=ROOT,
                check=True,
            )

    def test_helpers_177_and_178_registry_are_exact(self) -> None:
        source = RUNTIME_SOURCE.read_text(encoding="utf-8")
        pop_entry = re.compile(
            r"\{ZEND_NATIVE_HELPER_FRAME_ACTIVATION_POP,\s*"
            r"ZEND_NATIVE_EFFECT_FRAME_WRITE\s*"
            r"\| ZEND_NATIVE_RUNTIME_EFFECT_HEAP_WRITE,\s*"
            r"\(const void \*\) zend_native_frame_activation_pop\}"
        )
        release_entry = re.compile(
            r"\{ZEND_NATIVE_HELPER_FRAME_ACTIVATION_RELEASE,\s*"
            r"ZEND_NATIVE_EFFECT_CALL,\s*"
            r"\(const void \*\) zend_native_frame_activation_release\}"
        )
        self.assertRegex(source, pop_entry)
        self.assertRegex(source, release_entry)

        header = (ROOT / "Zend/Native/Runtime/Common/zend_native_calls.h").read_text(
            encoding="utf-8"
        )
        self.assertRegex(
            header,
            r"void zend_native_frame_activation_release\(\s*"
            r"zend_native_direct_activation \*activation\);",
        )

    def test_runtime_and_serial_image_reject_v80_mismatches(self) -> None:
        runtime = RUNTIME_SOURCE.read_text(encoding="utf-8")
        validator = function_body(runtime, "zend_native_runtime_validate(")
        self.assertIn(
            "runtime->abi_version != ZEND_NATIVE_RUNTIME_ABI_VERSION",
            validator,
        )
        self.assertIn(
            "runtime->helper_count != ZEND_NATIVE_HELPER_COUNT - 1",
            validator,
        )
        backend = TPDE_BACKEND.read_text(encoding="utf-8")
        self.assertIn(
            "header.runtime_abi != ZEND_NATIVE_RUNTIME_ABI_VERSION",
            backend,
        )

    def test_setup_pop_requires_unlinked_ownership_free_activation(self) -> None:
        source = CALLS_SOURCE.read_text(encoding="utf-8")
        body = function_body(source, "void zend_native_frame_activation_pop(")
        for invariant in (
            "zend_native_active_direct_call == activation",
            "activation->callee != NULL",
            "activation->resolution.ownership != 0",
            "activation->cell_active",
            "activation->raw_arguments_owned",
            "activation->frame_initialized",
            "activation->frame_requires_finish",
            "!Z_ISUNDEF(activation->discarded_return)",
        ):
            self.assertIn(invariant, body)
        self.assertIn("zend_vm_stack_free_call_frame(setup_frame);", body)
        for forbidden in (
            "zval_ptr_dtor",
            "zend_native_call_release_user_resolution",
            "zend_native_entry_cell_release_active",
            "zend_native_call_release_target",
        ):
            self.assertNotIn(forbidden, body)

    def test_no_call_builds_a_discard_frame_and_placements(self) -> None:
        source = CALLS_SOURCE.read_text(encoding="utf-8")
        resolver = function_body(source, "zend_native_call_resolve_user(")
        no_call = resolver[resolver.index("if (no_call) {") :]
        self.assertIn(
            "resolution->function = (zend_function *) &zend_pass_function;",
            no_call,
        )
        self.assertIn(
            "resolution->target_kind = ZEND_NATIVE_USER_CALL_TARGET_NO_CALL;",
            no_call,
        )
        self.assertIn("zend_native_call_resolution_build_placements(", no_call)
        self.assertIn("zend_native_call_resolution_set_sizes(", no_call)
        placements = function_body(
            source, "zend_native_call_resolution_build_placements("
        )
        self.assertIn("== ZEND_NATIVE_USER_CALL_TARGET_NO_CALL", placements)
        self.assertIn("runtime_tail = true;", placements)


if __name__ == "__main__":
    unittest.main()

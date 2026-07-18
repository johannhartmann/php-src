from __future__ import annotations

import os
from pathlib import Path
import subprocess
import textwrap
import unittest


SCRIPT = textwrap.dedent(
    r"""
    function row($name, $value) {
        echo $name, "=", json_encode($value, JSON_PRESERVE_ZERO_FRACTION), "\n";
    }
    row("strict_int_float", 1 === 1.0);
    row("loose_int_float", 1 == 1.0);
    row("nan_identical", NAN === NAN);
    row("nan_equal", NAN == NAN);
    row("nan_lt", NAN < NAN);
    row("nan_le", NAN <= NAN);
    row("nan_cmp", NAN <=> NAN);
    row("zero_identical", 0.0 === -0.0);
    row("zero_equal", 0.0 == -0.0);
    row("zero_lt", -0.0 < 0.0);
    row("zero_le", -0.0 <= 0.0);
    row("zero_cmp", -0.0 <=> 0.0);
    row("inf_identical", INF === INF);
    row("ninf_lt_inf", -INF < INF);
    row("inf_cmp", INF <=> -INF);
    row("large_loose", 9007199254740993 == 9007199254740993.0);
    row("large_strict", 9007199254740993 === 9007199254740993.0);
    row("null_false_loose", null == false);
    row("null_false_strict", null === false);
    row("null_zero_loose", null == 0);
    row("bool_not_true", !true);
    row("bool_xor", true xor false);
    row("cast_true_int", (int) true);
    row("cast_true_float", (float) true);
    row("cast_int_float", (float) 9007199254740993);
    row("cast_float_int", (int) 42.75);
    row("bool_inf", (bool) INF);
    row("bool_nan", (bool) NAN);
    row("cast_nan_int", (int) NAN);
    """
)

EXPECTED_STDOUT = textwrap.dedent(
    """\
    strict_int_float=false
    loose_int_float=true
    nan_identical=false
    nan_equal=false
    nan_lt=false
    nan_le=false
    nan_cmp=1
    zero_identical=true
    zero_equal=true
    zero_lt=false
    zero_le=true
    zero_cmp=0
    inf_identical=true
    ninf_lt_inf=true
    inf_cmp=1
    large_loose=true
    large_strict=false
    null_false_loose=true
    null_false_strict=false
    null_zero_loose=true
    bool_not_true=false
    bool_xor=true
    cast_true_int=1
    cast_true_float=1.0
    cast_int_float=9007199254740992.0
    cast_float_int=42
    bool_inf=true
    bool_nan=true
    cast_nan_int=0
    """
)


class ReferencePhpBoundaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        requested = os.environ.get("W03_D_REFERENCE_PHP")
        if requested is None:
            raise unittest.SkipTest(
                "W03_D_REFERENCE_PHP is not explicitly set to the reference PHP"
            )
        reference = Path(requested)
        if not reference.is_absolute():
            raise AssertionError("W03_D_REFERENCE_PHP must be an absolute path")
        if not reference.is_file() or not os.access(reference, os.X_OK):
            raise AssertionError(
                f"W03_D_REFERENCE_PHP is not an executable file: {reference}"
            )
        cls.reference = reference

    def test_scalar_logic_boundaries_against_reference_php(self) -> None:
        environment = os.environ.copy()
        environment.update({"LC_ALL": "C", "TZ": "UTC"})
        completed = subprocess.run(
            [
                str(self.reference),
                "-n",
                "-d",
                "date.timezone=UTC",
                "-d",
                "display_errors=stderr",
                "-d",
                "error_reporting=-1",
                "-r",
                SCRIPT,
            ],
            env=environment,
            check=False,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(0, completed.returncode)
        self.assertEqual(EXPECTED_STDOUT, completed.stdout)
        diagnostics = completed.stderr.splitlines()
        self.assertEqual(2, len(diagnostics))
        self.assertIn("unexpected NAN value was coerced to bool", diagnostics[0])
        self.assertIn("The float NAN is not representable as an int", diagnostics[1])


if __name__ == "__main__":
    unittest.main()

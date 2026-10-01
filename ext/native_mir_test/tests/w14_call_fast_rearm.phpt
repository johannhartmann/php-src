--TEST--
Native fast call sites re-armed after the call-cache epoch advances
--DESCRIPTION--
When the call-cache epoch advances, as it does at the end of every
request, a published fast site of an immutable function or method is
re-armed in place by a stub that preserves every caller-saved register:
the name must still bind the function, the class must be immutable, the
entry cell must hold the published entry and the run-time cache is
refreshed. Values live in registers across the stale site survive.
--EXTENSIONS--
opcache
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function plain($a, $b) { return $a . $b; }
function floats($x, $y) { return $x * $y; }
class Imm { public $v = 2; function scale($f) { return $this->v * $f; } function self_call($n) { return $n ? $this->self_call($n - 1) + 1 : 0; } }
function invalidate() { if (function_exists('native_mir_test_call_cache_invalidate')) { native_mir_test_call_cache_invalidate(); } }
function run($i) {
    $f = 1.5 * $i; $g = $f + 0.25; $s = "s$i"; $o = new Imm;
    $out = [];
    for ($k = 0; $k < 3; $k++) {
        $out[] = plain($s, $k) . '|' . floats($f, $g) . '|' . $o->scale($g) . '|' . $o->self_call(3);
        invalidate();
        /* Values live across the stale sites must survive the re-arm call. */
        $out[] = sprintf('%.2f %.2f %s', $f + $k, $g * $k, $s);
    }
    return implode(' ', $out);
}
for ($i = 0; $i < 3; $i++) { echo run($i), "\n"; }
?>
--EXPECT--
s00|0|0.5|3 0.00 0.00 s0 s01|0|0.5|3 1.00 0.25 s0 s02|0|0.5|3 2.00 0.50 s0
s10|2.625|3.5|3 1.50 0.00 s1 s11|2.625|3.5|3 2.50 1.75 s1 s12|2.625|3.5|3 3.50 3.50 s1
s20|9.75|6.5|3 3.00 0.00 s2 s21|9.75|6.5|3 4.00 3.25 s2 s22|9.75|6.5|3 5.00 6.50 s2

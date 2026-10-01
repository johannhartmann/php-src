--TEST--
Native fast call sites of one target re-arm through its entry cell
--DESCRIPTION--
Several call sites of the same function and the same method: the first
site's re-arm verifies the target, the others reuse it; a receiver of
another class still misses the method site's guard.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function twice($x) { return $x * 2; }
class A { function v($x) { return "A$x"; } }
class B extends A { function v($x) { return "B$x"; } }
function sites($o, $i) {
    return twice($i) + twice($i + 1) . ':' . $o->v($i) . $o->v($i + 1);
}
$out = [];
for ($i = 0; $i < 4; $i++) {
    $out[] = sites($i % 2 ? new B : new A, $i);
}
echo implode(' ', $out), "\n";
?>
--EXPECT--
2:A0A1 6:B1B2 10:A2A3 14:B3B4

--TEST--
Native fast call sites rely on the callee's return type verification
--DESCRIPTION--
A fast call site completes a returned frame without checking its return
type again: the callee's VERIFY_RETURN_TYPE coerces in weak mode, throws
for a mismatch or a missing value, and VERIFY_NEVER_TYPE throws for a
never function that returns, as in the VM.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
class Box { function self(): static { return $this; } function bad(): static { return new stdClass; } }
function coerce($v): int { return $v; }
function nullable($v): ?string { return $v; }
function missing($v): int { if ($v) { return 1; } }
function nothing(): never { }
function run() {
    $b = new Box;
    $out = [];
    for ($i = 0; $i < 3; $i++) {
        $out[] = var_export(coerce("4$i"), true) . ' ' . var_export(nullable($i ? 7 : null), true)
            . ' ' . get_class($b->self());
        foreach ([fn() => coerce("x"), fn() => missing(0), fn() => $b->bad(), fn() => nothing()] as $f) {
            try { $f(); $out[] = 'no error'; } catch (TypeError $e) { $out[] = $e->getMessage(); }
        }
    }
    return $out;
}
echo implode("\n", array_unique(run())), "\n";
?>
--EXPECT--
40 NULL Box
coerce(): Return value must be of type int, string returned
missing(): Return value must be of type int, none returned
Box::bad(): Return value must be of type Box, stdClass returned
nothing(): never-returning function must not implicitly return
41 '7' Box
42 '7' Box

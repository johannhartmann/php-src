--TEST--
Native SEND_ARRAY sends array_slice() forms of packed arrays
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function show(...$a) { return json_encode($a); }
function two($a, $b = 'default') { return "$a/$b"; }
function by_ref(&$a, $b = null) { $a = 'changed'; return 'ref'; }
function apply($cb, $args, $n) { return call_user_func_array($cb, array_slice($args, 0, $n)); }
function apply_skip($cb, $args, $skip, $n) { return call_user_func_array($cb, array_slice($args, $skip, $n)); }
function apply_null($cb, $args) { return call_user_func_array($cb, array_slice($args, 1, null)); }
$args = [1, 'two', [3], 4.5];
$x = 'x';
$with_ref = [&$x, 'y'];
$hashed = ['a' => 1, 'b' => 2];
for ($i = 0; $i < 3; $i++) {
    foreach ([0, 1, 2, 4, 9, -1, -3, -9] as $n) {
        echo apply('show', $args, $n), ' ', apply('two', $args, max($n, 1)), "\n";
        echo apply_skip('show', $args, 2, $n), ' ', apply_skip('show', $args, 7, $n), "\n";
    }
    echo apply_null('show', $args), "\n";
    echo apply('show', $with_ref, 2), ' ', apply('by_ref', $with_ref, 2), ' ', $x, "\n";
    $x = 'x';
    echo apply('show', $hashed, 1), "\n";
    try { echo call_user_func_array('show', array_slice($args, 0, '2')), "\n"; } catch (Throwable $e) { echo get_class($e), "\n"; }
}
--EXPECTF--
[] 1/default
[] []
[1] 1/default
[[3]] []
[1,"two"] 1/two
[[3],4.5] []
[1,"two",[3],4.5] 1/two
[[3],4.5] []
[1,"two",[3],4.5] 1/two
[[3],4.5] []
[1,"two",[3]] 1/default
[[3]] []
[1] 1/default
[] []
[] 1/default
[] []
["two",[3],4.5]
["x","y"] ref changed
[1]
[1,"two"]
[] 1/default
[] []
[1] 1/default
[[3]] []
[1,"two"] 1/two
[[3],4.5] []
[1,"two",[3],4.5] 1/two
[[3],4.5] []
[1,"two",[3],4.5] 1/two
[[3],4.5] []
[1,"two",[3]] 1/default
[[3]] []
[1] 1/default
[] []
[] 1/default
[] []
["two",[3],4.5]
["x","y"] ref changed
[1]
[1,"two"]
[] 1/default
[] []
[1] 1/default
[[3]] []
[1,"two"] 1/two
[[3],4.5] []
[1,"two",[3],4.5] 1/two
[[3],4.5] []
[1,"two",[3],4.5] 1/two
[[3],4.5] []
[1,"two",[3]] 1/default
[[3]] []
[1] 1/default
[] []
[] 1/default
[] []
["two",[3],4.5]
["x","y"] ref changed
[1]
[1,"two"]

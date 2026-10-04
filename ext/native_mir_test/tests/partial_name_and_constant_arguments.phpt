--TEST--
Native executor passes PFA names and constant pre-bound arguments
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
function native_pfa_target($a, $b, $c)
{
    return "$a|$b|$c";
}

function native_pfa_positional(int $n): array
{
    $out = [];
    for ($i = 0; $i < $n; $i++) {
        /* 'k' is a constant pre-bound argument, $i is not: only the constant
         * may be baked into the recycled application. */
        $out[] = native_pfa_target($i, 'k', ?)('z');
    }
    return $out;
}

function native_pfa_named(int $n): array
{
    $out = [];
    for ($i = 0; $i < $n; $i++) {
        $out[] = native_pfa_target(c: 'k', a: $i, b: ?)('y');
    }
    return $out;
}

function native_pfa_name(): string
{
    return (new ReflectionFunction(native_pfa_target(1, ?, ?)))->getName();
}

var_dump(native_pfa_positional(3));
var_dump(native_pfa_named(3));
var_dump(native_pfa_name());
?>
--EXPECT--
array(3) {
  [0]=>
  string(5) "0|k|z"
  [1]=>
  string(5) "1|k|z"
  [2]=>
  string(5) "2|k|z"
}
array(3) {
  [0]=>
  string(5) "0|y|k"
  [1]=>
  string(5) "1|y|k"
  [2]=>
  string(5) "2|y|k"
}
string(34) "{closure:pfa:native_pfa_name():29}"

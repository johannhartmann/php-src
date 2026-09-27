--TEST--
Native x64 inline return type check for boxed results
--DESCRIPTION--
A Zend-entry function whose result may be int or float verifies its return
type at run time. The x64 check accepts a boxed result whose type is in the
declared mask and leaves coercion, nullable and union returns, and the
TypeError for an overflowed int return to the runtime helper.
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
function inc(int $v): int { return $v + 1; }
function to_float(int $v): float { return $v + 1; }
function either(int $v): int|string { return $v > 2 ? $v + 1 : "s$v"; }
function maybe(int $v): ?int { return $v > 2 ? $v + 1 : null; }
function anything(int $v): mixed { return $v + 1; }
function rec(int $d): int { return $d === 0 ? 1 : rec($d - 1) + 1; }
function overflow_int(int $v): int { return $v + 1; }
for ($i = 0; $i < 3; $i++) {
    var_dump(inc($i), to_float($i), either($i), maybe($i), anything($i), rec(4));
}
var_dump(to_float(PHP_INT_MAX));
try {
    var_dump(overflow_int(PHP_INT_MAX));
} catch (TypeError $e) {
    echo get_class($e), ': ', $e->getMessage(), "\n";
}
?>
--EXPECT--
int(1)
float(1)
string(2) "s0"
NULL
int(1)
int(5)
int(2)
float(2)
string(2) "s1"
NULL
int(2)
int(5)
int(3)
float(3)
string(2) "s2"
NULL
int(3)
int(5)
float(9.223372036854776E+18)
TypeError: overflow_int(): Return value must be of type int, float returned

--TEST--
Native typed bodies: long arithmetic at the overflow boundaries
--DESCRIPTION--
A long ADD, SUB or MUL whose operand ranges exclude overflow keeps an exact
long result without an overflow exit; every other one continues in double
precision or fails its int return type exactly like the VM, also when called
from untyped callers and loops.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
/* Typed bodies whose long arithmetic Zend's ranges prove in bounds. */
function low_add(int $a): int { if ($a < 1000) { return $a + 1; } return 0; }
function high_sub(int $a): int { if ($a > -1000) { return $a - 1; } return 0; }
function small_mul(int $a): int { if ($a > -100000 && $a < 100000) { return $a * $a; } return -1; }
function masked(int $a): int { $b = $a & 0xFFFF; return $b * 3 + ($b - 7); }
function walk(int $n): int { $s = 0; for ($i = 0; $i < $n; $i++) { $j = $i + 1; if ($j > $s) { $s = $j; } } return $s; }
function sum_to(int $n): int { $t = 0; for ($i = 0; $i < $n && $i < 1000; $i++) { $t = ($i & 255) + 1; } return $t; }
/* Ranges that admit overflow: the result stays a number. */
function inc_any(int $a) { return $a + 1; }
function dec_any(int $a) { return $a - 1; }
function dbl_any(int $a) { return $a * 2; }
function inc_int(int $a): int { return $a + 1; }
function dbl_int(int $a): int { return $a * 2; }
function sub_any(int $a, int $b) { return $a - $b; }
function nested(int $a): int { return low_add($a) + high_sub($a); }
function untyped_caller($x) { return low_add($x) + small_mul($x) + masked($x); }
for ($round = 0; $round < 2; $round++) {
    echo "round $round\n";
    var_dump(low_add(5), low_add(PHP_INT_MIN), low_add(999), low_add(1000), low_add(PHP_INT_MAX));
    var_dump(high_sub(5), high_sub(PHP_INT_MAX), high_sub(-999), high_sub(-1000), high_sub(PHP_INT_MIN));
    var_dump(small_mul(99999), small_mul(-99999), small_mul(100000), small_mul(PHP_INT_MAX));
    var_dump(masked(PHP_INT_MAX), masked(PHP_INT_MIN), masked(-1), masked(6));
    var_dump(walk(0), walk(10), sum_to(5), sum_to(PHP_INT_MAX));
    var_dump(inc_any(PHP_INT_MAX), inc_any(PHP_INT_MAX - 1), dec_any(PHP_INT_MIN), dec_any(PHP_INT_MIN + 1));
    var_dump(dbl_any(PHP_INT_MAX), dbl_any(intdiv(PHP_INT_MAX, 2)), dbl_any(PHP_INT_MIN));
    var_dump(sub_any(PHP_INT_MIN, 1), sub_any(PHP_INT_MAX, -1), sub_any(0, PHP_INT_MIN), sub_any(-1, PHP_INT_MAX));
    foreach ([fn() => inc_int(PHP_INT_MAX), fn() => dbl_int(PHP_INT_MAX), fn() => dbl_int(PHP_INT_MIN), fn() => inc_int(41)] as $f) {
        try { var_dump($f()); } catch (TypeError $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
    }
    var_dump(nested(PHP_INT_MAX), nested(PHP_INT_MIN), nested(7));
    var_dump(untyped_caller(3), untyped_caller(PHP_INT_MAX), untyped_caller(PHP_INT_MIN));
    $acc = 0;
    for ($k = PHP_INT_MAX - 3; $k > 0 && $k <= PHP_INT_MAX - 1; $k++) { $acc = low_add(-$k) + $acc; }
    var_dump($acc, $k);
    $x = PHP_INT_MAX;
    var_dump($x + 1, $x * 2, PHP_INT_MIN - 1);
}
?>
--EXPECT--
round 0
int(6)
int(-9223372036854775807)
int(1000)
int(0)
int(0)
int(4)
int(9223372036854775806)
int(-1000)
int(0)
int(0)
int(9999800001)
int(9999800001)
int(-1)
int(-1)
int(262133)
int(-7)
int(262133)
int(17)
int(0)
int(10)
int(5)
int(232)
float(9.223372036854776E+18)
int(9223372036854775807)
float(-9.223372036854776E+18)
int(-9223372036854775808)
float(1.8446744073709552E+19)
int(9223372036854775806)
float(-1.8446744073709552E+19)
float(-9.223372036854776E+18)
float(9.223372036854776E+18)
float(9.223372036854776E+18)
int(-9223372036854775808)
TypeError: inc_int(): Return value must be of type int, float returned
TypeError: dbl_int(): Return value must be of type int, float returned
TypeError: dbl_int(): Return value must be of type int, float returned
int(42)
int(9223372036854775806)
int(-9223372036854775807)
int(14)
int(18)
int(262132)
float(-9.223372036854776E+18)
float(-2.7670116110564327E+19)
int(9223372036854775807)
float(9.223372036854776E+18)
float(1.8446744073709552E+19)
float(-9.223372036854776E+18)
round 1
int(6)
int(-9223372036854775807)
int(1000)
int(0)
int(0)
int(4)
int(9223372036854775806)
int(-1000)
int(0)
int(0)
int(9999800001)
int(9999800001)
int(-1)
int(-1)
int(262133)
int(-7)
int(262133)
int(17)
int(0)
int(10)
int(5)
int(232)
float(9.223372036854776E+18)
int(9223372036854775807)
float(-9.223372036854776E+18)
int(-9223372036854775808)
float(1.8446744073709552E+19)
int(9223372036854775806)
float(-1.8446744073709552E+19)
float(-9.223372036854776E+18)
float(9.223372036854776E+18)
float(9.223372036854776E+18)
int(-9223372036854775808)
TypeError: inc_int(): Return value must be of type int, float returned
TypeError: dbl_int(): Return value must be of type int, float returned
TypeError: dbl_int(): Return value must be of type int, float returned
int(42)
int(9223372036854775806)
int(-9223372036854775807)
int(14)
int(18)
int(262132)
float(-9.223372036854776E+18)
float(-2.7670116110564327E+19)
int(9223372036854775807)
float(9.223372036854776E+18)
float(1.8446744073709552E+19)
float(-9.223372036854776E+18)

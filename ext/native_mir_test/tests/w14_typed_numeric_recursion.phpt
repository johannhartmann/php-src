--TEST--
Native x64 typed bodies return numbers from recursive arithmetic
--DESCRIPTION--
A component member whose returns are all numbers returns a boxed number from
its typed body. Addition, subtraction and multiplication of such results
cannot fail: integer overflow continues in double precision. Scalars returned
through an untyped result are boxed. Members with another return, and typed
returns that a double would violate, keep their Zend behavior.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function fib($n) { return $n < 2 ? 1 : fib($n - 2) + fib($n - 1); }
function pow3($n) { return $n < 1 ? 1 : pow3($n - 1) * 3; }
function half($n) { return $n < 1 ? 0.5 : half($n - 1) + $n; }
function down($n) { return $n < 1 ? PHP_INT_MIN + 2 : down($n - 1) - 1; }
function mixed_ret($n) { return $n < 1 ? "s" : mixed_ret($n - 1) . "x"; }
function maybe_string($n) { if ($n > 5) return "big"; return $n < 1 ? 1 : maybe_string($n - 1) + 1; }
function scalar_ret(int $n) { if ($n > 0) return 1; if ($n < 0) return 2.5; return true; }
function typed_over(int $n): int { return $n < 1 ? PHP_INT_MAX - 1 : typed_over($n - 1) + 1; }
function loopsum($n) { $s = 0; for ($i = 0; $i < $n; $i++) { $s = $s + $i * PHP_INT_MAX; } return $s; }
for ($round = 0; $round < 2; $round++) {
    var_dump(fib(20), fib(1), fib("7"), fib(3.5));
    var_dump(pow3(10), pow3(39), pow3(40), pow3(45));
    var_dump(half(4), down(0), down(1), down(3));
    var_dump(mixed_ret(3), maybe_string(3), maybe_string(9));
    var_dump(scalar_ret(3), scalar_ret(-3), scalar_ret(0));
    var_dump(typed_over(1));
    try { var_dump(typed_over(3)); } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
    var_dump(loopsum(3));
}
?>
--EXPECT--
int(10946)
int(1)
int(21)
int(3)
int(59049)
int(4052555153018976267)
float(1.2157665459056929E+19)
float(2.954312706550834E+21)
float(10.5)
int(-9223372036854775806)
int(-9223372036854775807)
float(-9.223372036854776E+18)
string(4) "sxxx"
int(4)
string(3) "big"
int(1)
float(2.5)
bool(true)
int(9223372036854775807)
typed_over(): Return value must be of type int, float returned
float(2.7670116110564327E+19)
int(10946)
int(1)
int(21)
int(3)
int(59049)
int(4052555153018976267)
float(1.2157665459056929E+19)
float(2.954312706550834E+21)
float(10.5)
int(-9223372036854775806)
int(-9223372036854775807)
float(-9.223372036854776E+18)
string(4) "sxxx"
int(4)
string(3) "big"
int(1)
float(2.5)
bool(true)
int(9223372036854775807)
typed_over(): Return value must be of type int, float returned
float(2.7670116110564327E+19)

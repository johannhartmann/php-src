--TEST--
Native x64 typed bodies merge numbers from ternaries
--DESCRIPTION--
A ternary assigns a constant or a computed number to one temporary; its
phi becomes a register zval of the typed body, whose QM_ASSIGN forwards (and
boxes) its source. Returns of int or float check or convert the number.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function fib(int $n): int { return $n < 2 ? 1 : fib($n - 2) + fib($n - 1); }
function over(int $n): int { return $n < 1 ? PHP_INT_MAX - 1 : over($n - 1) + 1; }
function half(int $n): float { return $n < 1 ? 1 : half($n - 1) + 0.5; }
function mul(int $n): int { return $n < 1 ? 3 : mul($n - 1) * 3; }
function un($n) { return $n < 1 ? 2 : un($n - 1) * 2; }
for ($i = 0; $i < 2; $i++) {
    var_dump(fib(20), over(1), half(3), mul(5), un(10), un(70));
    try { var_dump(over(3)); } catch (TypeError $e) { echo $e->getMessage(), " @", count($e->getTrace()), "\n"; }
    try { var_dump(mul(40)); } catch (TypeError $e) { echo $e->getMessage(), " @", count($e->getTrace()), "\n"; }
}
?>
--EXPECT--
int(10946)
int(9223372036854775807)
float(2.5)
int(729)
int(2048)
float(2.3611832414348226E+21)
over(): Return value must be of type int, float returned @2
mul(): Return value must be of type int, float returned @2
int(10946)
int(9223372036854775807)
float(2.5)
int(729)
int(2048)
float(2.3611832414348226E+21)
over(): Return value must be of type int, float returned @2
mul(): Return value must be of type int, float returned @2

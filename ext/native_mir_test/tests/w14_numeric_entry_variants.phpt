--TEST--
Native x64 numeric entry variants call typed bodies with number arguments
--DESCRIPTION--
A function whose recursive integer arguments may overflow compiles a variant
with int|float parameters. Calls whose checked arguments are numbers, results
of number-returning members included, target it; its typed body compares
numbers into register booleans, boxes scalar arguments for zval parameters
and returns boxed numbers. A reused temporary returns the call result, not the
arithmetic that previously held its slot.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function Ack($m, $n) { if ($m == 0) return $n + 1; if ($n == 0) return Ack($m - 1, 1); return Ack($m - 1, Ack($m, $n - 1)); }
function A2(int|float $m, int|float $n) { if ($m == 0) return $n + 1; return A2($m - 1, $n); }
function A3(int|float $m, int|float $n) { if ($m == 0) return $n + 1; return A3($m - 1, A3(0, $n)); }
function z(int|float $m) { if ($m == 0) return 10; return 20; }
function zz($x) { return z($x) + z($x - $x); }
function c(int|float $m, int|float $n) { return $m < $n ? 1 : 2; }
function cc($a, $b) { return c($a, $b) * 10 + c($b, $a); }
function box(int|float $m) { return $m; }
function bb($x) { return box(1) + box($x); }
function u(int|float $m, int|float $n) { return $m * 100 + $n; }
function w(int|float $x, int|float $y) { return u($x - 1, $y); }
function w2(int|float $x, int|float $y) { return u($y, $x - 1); }
function w3(int|float $x, int|float $y) { return u($x + 0, $y * 1); }
function discard() { return 1; }
function calls_discard() { discard(); return 2; }
for ($i = 0; $i < 2; $i++) {
    echo Ack(3, 5), " ", Ack(2, 3), " ", Ack("2", 3), " ", Ack(2.0, 2), " ", Ack(true, 2), " ", Ack(2, "1"), "\n";
    var_dump(Ack(0, PHP_INT_MAX), Ack(0, 1.5), Ack(1, 2.0));
    var_dump(A2(3, 5), A2(2, 2.5), A3(3, 5), A3(1, PHP_INT_MAX));
    var_dump(zz(0), zz(3), zz(0.0), cc(1, 2), cc(2.5, 1), cc(NAN, 1), bb(5), bb(1.5));
    var_dump(w(1, 7), w(2, 8), w2(1, 7), w2(2.5, 8), w3(1, 7), w3(2, 0.5), calls_discard());
}
?>
--EXPECT--
253 9 9 7 4 5
float(9.223372036854776E+18)
float(2.5)
int(4)
int(6)
float(3.5)
int(9)
float(9.223372036854776E+18)
int(20)
int(30)
int(20)
int(12)
int(21)
int(22)
int(6)
float(2.5)
int(7)
int(108)
int(700)
float(801.5)
int(107)
float(200.5)
int(2)
253 9 9 7 4 5
float(9.223372036854776E+18)
float(2.5)
int(4)
int(6)
float(3.5)
int(9)
float(9.223372036854776E+18)
int(20)
int(30)
int(20)
int(12)
int(21)
int(22)
int(6)
float(2.5)
int(7)
int(108)
int(700)
float(801.5)
int(107)
float(200.5)
int(2)

--TEST--
Native: loop longs stay in registers in a typed body and fail it on overflow
--DESCRIPTION--
A typed body keeps loop phis of longs and the arithmetic merged into them in
registers. ADD, SUB and MUL of two longs whose range Zend's inference does
not bound fail the body on overflow: the call runs again through its Zend
entry, which continues in double.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function grow(int $n) { $a = 1; for ($i = 0; $i < $n; $i++) { $a = $a * 3 + 1; } return $a; }
function grow_typed(int $n): int|float { $a = 1; for ($i = 0; $i < $n; $i++) { $a = $a * 3 - $i; } return $a; }
function sum_to(int $n) { $s = PHP_INT_MAX - 10; for ($i = 0; $i < $n; $i++) { $s += 1; } return $s; }
function neg(int $n) { $s = PHP_INT_MIN + 5; for ($i = 0; $i < $n; $i++) { $s -= 1; } return $s; }
function mix(int $n) { $h = 0; for ($i = 0; $i < $n; $i++) { $h = ($h * 31 + $i) & 0xFFFFFFFF; $h ^= $i << 3; } return $h; }
function fib(int $n): int { return $n < 2 ? $n : fib($n - 1) + fib($n - 2); }
function fibl(int $n) { $a = 0; $b = 1; for ($i = 0; $i < $n; $i++) { $t = $a + $b; $a = $b; $b = $t; } return $a; }
function pow3(int $n): int { $a = 1; for ($i = 0; $i < $n; $i++) { $a *= 3; } return $a; }
function count_down(int $n) { $c = 0; while ($n > 0) { $n--; $c += 2; } return $c; }
for ($k = 0; $k < 2; $k++) {
  var_dump(grow(10), grow(39), grow(40), grow(41), grow(60));
  var_dump(grow_typed(5), grow_typed(45));
  var_dump(sum_to(5), sum_to(10), sum_to(11), sum_to(20));
  var_dump(neg(5), neg(6), neg(7));
  var_dump(mix(1000));
  var_dump(fib(20));
  var_dump(fibl(10), fibl(92), fibl(93), fibl(100));
  var_dump(count_down(1000));
}
foreach ([10, 39, 40] as $n) {
  try { var_dump(pow3($n)); } catch (TypeError $e) { echo $e->getMessage(), "\n"; }
}
--EXPECT--
int(88573)
int(6078832729528464400)
float(1.8236498188585394E+19)
float(5.470949456575618E+19)
float(6.358673741282431E+28)
int(185)
float(2.2157345299131256E+21)
int(9223372036854775802)
int(9223372036854775807)
float(9.223372036854776E+18)
float(9.223372036854776E+18)
int(-9223372036854775808)
float(-9.223372036854776E+18)
float(-9.223372036854776E+18)
int(3376177364)
int(6765)
int(55)
int(7540113804746346429)
float(1.2200160415121877E+19)
float(3.54224848179262E+20)
int(2000)
int(88573)
int(6078832729528464400)
float(1.8236498188585394E+19)
float(5.470949456575618E+19)
float(6.358673741282431E+28)
int(185)
float(2.2157345299131256E+21)
int(9223372036854775802)
int(9223372036854775807)
float(9.223372036854776E+18)
float(9.223372036854776E+18)
int(-9223372036854775808)
float(-9.223372036854776E+18)
float(-9.223372036854776E+18)
int(3376177364)
int(6765)
int(55)
int(7540113804746346429)
float(1.2200160415121877E+19)
float(3.54224848179262E+20)
int(2000)
int(59049)
int(4052555153018976267)
pow3(): Return value must be of type int, float returned

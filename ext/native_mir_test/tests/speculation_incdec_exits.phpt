--TEST--
Native: a specialized version keeps longs that ++ and -- may overflow
--DESCRIPTION--
With speculation on, ++ and -- of a long without a range proof narrow to a
long behind a deoptimization exit, also in pre- and post-form and through
loop-carried values; where the step would overflow, the generic copy
continues in double as the VM does.
--EXTENSIONS--
opcache
--ENV--
ZEND_NATIVE_SPECULATE=1
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function inc(int $start, int $n) { $s = $start; for ($i = 0; $i < $n; $i++) { $s++; } return $s; }
function dec(int $start, int $n) { $s = $start; for ($i = 0; $i < $n; $i++) { --$s; } return $s; }
function post(int $start, int $n) { $s = $start; $t = 0; for ($i = 0; $i < $n; $i++) { $t = $s++; } return [$s, $t]; }
function pre(int $start, int $n) { $s = $start; $t = 0; for ($i = 0; $i < $n; $i++) { $t = ++$s; } return [$s, $t]; }
function mixed_steps(int $start, int $n) { $s = $start; for ($i = 0; $i < $n; $i++) { $s++; $s += 2; $s--; } return $s; }
var_dump(inc(5, 10), inc(PHP_INT_MAX - 2, 5), dec(PHP_INT_MIN + 2, 5));
var_dump(post(PHP_INT_MAX - 1, 3), pre(PHP_INT_MAX - 1, 3));
var_dump(mixed_steps(0, 4), mixed_steps(PHP_INT_MAX - 5, 4));
?>
--EXPECT--
int(15)
float(9.223372036854776E+18)
float(-9.223372036854776E+18)
array(2) {
  [0]=>
  float(9.223372036854776E+18)
  [1]=>
  float(9.223372036854776E+18)
}
array(2) {
  [0]=>
  float(9.223372036854776E+18)
  [1]=>
  float(9.223372036854776E+18)
}
int(8)
float(9.223372036854776E+18)

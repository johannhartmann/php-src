--TEST--
Native: a specialized version keeps longs that may overflow and leaves on overflow
--DESCRIPTION--
With speculation on, a function with a loop compiles as a specialized
version whose SSA narrows ADD, SUB and MUL of two longs to long, also
through loop-carried values, and a generic copy with landings. Where such
an operation would overflow, the specialized version stores the scalars
live there to their frame slots and continues in the generic copy before
the operation, which computes the double. Output, state after the exit,
exceptions and recursion match the VM.
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
function grow(array $unused, int $n) { $s = 1; for ($i = 0; $i < $n; $i++) { $s = $s * 3 + $i; } return $s; }
function grow_echo(int $n) { $s = PHP_INT_MAX - 20; for ($i = 0; $i < $n; $i++) { $s = $s + 7; echo $i, ":", is_int($s) ? "i" : "f", " "; } echo "\n"; return $s; }
function state(array $a, int $n) { $s = PHP_INT_MAX - 3; $c = 0; $t = 10; for ($i = 0; $i < $n; $i++) { $c = $c + 2; $s = $s + 1; $t = $t * 2; } return [$s, $c, $t, $i]; }
function first(array $a) { $s = PHP_INT_MAX; for ($i = 0; $i < 3; $i++) { $s = $s + 1; } return $s; }
function nested(array $a, int $n) { $r = []; for ($i = 0; $i < $n; $i++) { $s = PHP_INT_MAX - 2; for ($j = 0; $j < 4; $j++) { $s = $s + $i; } $r[] = $s; } return $r; }
function chain(array $a, int $n) { $s = 7; for ($i = 0; $i < $n; $i++) { $s = (($s * 31 + $i) & 0xFFFFFF) % 1000003; } return $s; }
function mixed_calls(array $a, int $start) { $s = $start; for ($i = 0; $i < 5; $i++) { $s = $s + $start; } return $s; }
function throws_after(array $a, int $n) { $s = PHP_INT_MAX - 1; for ($i = 0; $i < $n; $i++) { $s = $s + 1; } if (is_float($s)) { throw new RuntimeException("float " . $s); } return $s; }
function rec(array $a, int $n) { if ($n <= 0) { return 1; } $s = 1; for ($i = 0; $i < 3; $i++) { $s = $s * 1000003; } return $s + rec($a, $n - 1); }
function sub_neg(array $a, int $n) { $s = PHP_INT_MIN + 5; for ($i = 0; $i < $n; $i++) { $s = $s - 2; } return $s; }
function views(array $a, int $n) { $s = PHP_INT_MAX - 1; for ($i = 0; $i < $n; $i++) { $s = $s + 1; $v = get_defined_vars(); } return [$s, $v['s'], $v['i']]; }

var_dump(grow([], 10), grow([], 39), grow([], 40), grow([], 60));
var_dump(grow_echo(5));
var_dump(state([], 2), state([], 5), state([], 65));
var_dump(first([]));
var_dump(nested([], 3));
var_dump(chain([], 1000));
foreach ([1, PHP_INT_MAX >> 2, 5, PHP_INT_MAX >> 1, 3] as $start) { var_dump(mixed_calls([], $start)); }
try { var_dump(throws_after([], 1)); var_dump(throws_after([], 3)); } catch (RuntimeException $e) { echo $e->getMessage(), "\n"; }
var_dump(rec([], 3));
var_dump(sub_neg([], 2), sub_neg([], 4));
var_dump(views([], 1), views([], 3));
?>
--EXPECT--
int(73806)
int(5065693941273720314)
float(1.519708182382116E+19)
float(5.298894784402026E+28)
0:i 1:i 2:f 3:f 4:f 
float(9.223372036854776E+18)
array(4) {
  [0]=>
  int(9223372036854775806)
  [1]=>
  int(4)
  [2]=>
  int(40)
  [3]=>
  int(2)
}
array(4) {
  [0]=>
  float(9.223372036854776E+18)
  [1]=>
  int(10)
  [2]=>
  int(320)
  [3]=>
  int(5)
}
array(4) {
  [0]=>
  float(9.223372036854776E+18)
  [1]=>
  int(130)
  [2]=>
  float(3.68934881474191E+20)
  [3]=>
  int(65)
}
float(9.223372036854776E+18)
array(3) {
  [0]=>
  int(9223372036854775805)
  [1]=>
  float(9.223372036854776E+18)
  [2]=>
  float(9.223372036854776E+18)
}
int(102007)
int(6)
float(1.3835058055282164E+19)
int(30)
float(2.7670116110564327E+19)
int(18)
int(9223372036854775807)
float 9.2233720368548E+18
int(3000027000081000082)
int(-9223372036854775807)
float(-9.223372036854776E+18)
array(3) {
  [0]=>
  int(9223372036854775807)
  [1]=>
  int(9223372036854775807)
  [2]=>
  int(0)
}
array(3) {
  [0]=>
  float(9.223372036854776E+18)
  [1]=>
  float(9.223372036854776E+18)
  [2]=>
  int(2)
}

--TEST--
Native x64 double arithmetic in declared and inferred double chains
--DESCRIPTION--
Double values produced by arithmetic on float parameters reach later
operations as boxed zvals whose payload is a general-purpose part; loading
it must not use an SSE register. Infinities and NaN keep VM results.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function sq(float $x) { $y = $x * $x; return $y + $x - 0.5; }
function poly(float $x) { $a = $x * 2.5; $b = $a - $x; $c = $b * $b; return $c + $a; }
function loop(float $s) { $acc = 0.0; for ($i = 0; $i < 1000; $i++) { $acc = $acc * 0.5 + $s; $s = $s - 0.25; } return $acc; }
function mixed_ops($n) { $x = 0.1; $t = 0.0; for ($i = 0; $i < $n; $i++) { $t = $t + $x * $x - $x; $x = $x + 0.001; } return $t; }
function inf_nan() { $big = 1e308; $x = $big * 10.0; $y = $x - $x; return [$x, $y, $x > 1.0, $y == $y, $y != $y]; }
for ($r = 0; $r < 3; $r++) {
    var_dump(sq(3.0), sq(-1.5), poly(2.0), loop(1.0), mixed_ops(100));
    var_dump(inf_nan());
}
?>
--EXPECT--
float(11.5)
float(0.25)
float(14)
float(-497)
float(-12.631650000000008)
array(5) {
  [0]=>
  float(INF)
  [1]=>
  float(NAN)
  [2]=>
  bool(true)
  [3]=>
  bool(false)
  [4]=>
  bool(true)
}
float(11.5)
float(0.25)
float(14)
float(-497)
float(-12.631650000000008)
array(5) {
  [0]=>
  float(INF)
  [1]=>
  float(NAN)
  [2]=>
  bool(true)
  [3]=>
  bool(false)
  [4]=>
  bool(true)
}
float(11.5)
float(0.25)
float(14)
float(-497)
float(-12.631650000000008)
array(5) {
  [0]=>
  float(INF)
  [1]=>
  float(NAN)
  [2]=>
  bool(true)
  [3]=>
  bool(false)
  [4]=>
  bool(true)
}

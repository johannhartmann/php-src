--TEST--
Native boxed copies of scalar operation results and copies (without opcache)
--DESCRIPTION--
A boxed-identity COPY of a value a Zend entry defines in registers (a scalar operation's machine result or a selected machine copy) reads that register, so loops that copy comparisons and arithmetic through several CVs compile with and without opcache.
--INI--
opcache.enable=0
opcache.enable_cli=0
--FILE--
<?php
function k($n) { $s = 0; for ($i = 0; $i < $n; $i++) { $z = $i > 1; $w = $z; $x = $i * 2 + 1; $y = $x; $s += $y + (int)$w; } return $s; }
function k2(int $n) { $s = 0; for ($i = 0; $i < $n; $i++) { $z = $i >= 2; $w = $z; $v = $w; $s += $v ? 3 : 1; } return [$s, $z ?? null, $w ?? null]; }
function k3(int $n) { $t = 0.0; for ($i = 0; $i < $n; $i++) { $f = $i * 0.5; $g = $f; $t += $g; } return $t; }
function k4(int $n) { $c = 0; for ($i = 0; $i < $n; $i++) { $e = $i == 3; $ne = $i != 3; $a = $e; $b = $ne; if ($a) $c += 10; if ($b) $c++; } return $c; }
function k5($n) { $last = null; for ($i = 0; $i < $n; $i++) { $q = $i < 2; $last = $q; } return $last; }
function k6(int $n) { $x = 0; for ($i = 0; $i < $n; $i++) { $x = $i + 1; $y = $x; $z = $y; } return isset($z) ? [$x, $y, $z] : 'none'; }
function k7(int $n) { $s = ''; for ($i = 0; $i < $n; $i++) { $b = $i % 2 == 0; $c = $b; $s .= $c ? 'e' : 'o'; } return $s; }
foreach ([0, 1, 3, 5] as $n) {
    var_dump(k($n), k2($n), k3($n), k4($n), k5($n), k6($n), k7($n));
}
?>
--EXPECT--
int(0)
array(3) {
  [0]=>
  int(0)
  [1]=>
  NULL
  [2]=>
  NULL
}
float(0)
int(0)
NULL
string(4) "none"
string(0) ""
int(1)
array(3) {
  [0]=>
  int(1)
  [1]=>
  bool(false)
  [2]=>
  bool(false)
}
float(0)
int(1)
bool(true)
array(3) {
  [0]=>
  int(1)
  [1]=>
  int(1)
  [2]=>
  int(1)
}
string(1) "e"
int(10)
array(3) {
  [0]=>
  int(5)
  [1]=>
  bool(true)
  [2]=>
  bool(true)
}
float(1.5)
int(3)
bool(false)
array(3) {
  [0]=>
  int(3)
  [1]=>
  int(3)
  [2]=>
  int(3)
}
string(3) "eoe"
int(28)
array(3) {
  [0]=>
  int(11)
  [1]=>
  bool(true)
  [2]=>
  bool(true)
}
float(5)
int(14)
bool(false)
array(3) {
  [0]=>
  int(5)
  [1]=>
  int(5)
  [2]=>
  int(5)
}
string(5) "eoeoe"

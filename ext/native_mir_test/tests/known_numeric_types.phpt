--TEST--
Native x64 frame numerics with operand types fixed by type inference
--DESCRIPTION--
When Zend's type inference fixes both operands of ADD, SUB, MUL or a
comparison as long or double, the inline form skips the type dispatch and
evaluates double operations straight from the frame slots. The Mandelbrot
loops of Zend/bench.php and mixed long/double operands keep VM results.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function mandel2() {
  $b = " .:,;!/>)|&IH%*#";
  for ($y=30; printf("\n"), $C = $y*0.1 - 1.5, $y--;){
    for ($x=0; $c = $x*0.04 - 2, $z=0, $Z=0, $x++ < 75;){
      for ($r=$c, $i=$C, $k=0; $t = $z*$z - $Z*$Z + $r, $Z = 2*$z*$Z + $i, $z=$t, $k<5000; $k++)
        if ($z*$z + $Z*$Z > 500000) break;
      echo $b[$k%16];
    }
  }
}
function mixed_numbers($n) {
  $l = 3; $d = 0.5; $out = [];
  for ($i = 0; $i < $n; $i++) {
    $out[] = [$l * $d, $l + $d, $d - $l, $l < $d, $d <= $l, $l == 3.0, $d != 0.5];
    $l = $l + 1; $d = $d * 2.0;
  }
  return $out;
}
function limits() {
  $max = PHP_INT_MAX; $one = 1; $x = 1.5;
  return [$max + $one, $max * 2, -$max - 2, $x * 0.0, -0.0 * $x, 1e308 * $x];
}
ob_start();
mandel2();
echo md5(ob_get_clean()), "\n";
var_dump(mixed_numbers(3), limits());
?>
--EXPECT--
858997d7b2eebfe545ae353dd707859d
array(3) {
  [0]=>
  array(7) {
    [0]=>
    float(1.5)
    [1]=>
    float(3.5)
    [2]=>
    float(-2.5)
    [3]=>
    bool(false)
    [4]=>
    bool(true)
    [5]=>
    bool(true)
    [6]=>
    bool(false)
  }
  [1]=>
  array(7) {
    [0]=>
    float(4)
    [1]=>
    float(5)
    [2]=>
    float(-3)
    [3]=>
    bool(false)
    [4]=>
    bool(true)
    [5]=>
    bool(false)
    [6]=>
    bool(true)
  }
  [2]=>
  array(7) {
    [0]=>
    float(10)
    [1]=>
    float(7)
    [2]=>
    float(-3)
    [3]=>
    bool(false)
    [4]=>
    bool(true)
    [5]=>
    bool(false)
    [6]=>
    bool(true)
  }
}
array(6) {
  [0]=>
  float(9.223372036854776E+18)
  [1]=>
  float(1.8446744073709552E+19)
  [2]=>
  float(-9.223372036854776E+18)
  [3]=>
  float(0)
  [4]=>
  float(-0)
  [5]=>
  float(1.5E+308)
}

--TEST--
Native compound assignment of a literal to a CV keeps PHP semantics
--DESCRIPTION--
$cv op= CONST takes the inline integer path; overflow, non-integer and
undefined left operands take the helper.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function ints($n) {
    $s = 0; $t = 7;
    for ($i = 0; $i < $n; $i++) {
        $s += 3; $s -= 1; $t |= 8; $t &= 0xff; $t ^= 5;
    }
    return [$s, $t];
}
function mixed_left() {
    $x = PHP_INT_MAX; $x += 1;
    $y = "5"; $y += 2;
    $z = 1.5; $z -= 1;
    $a = null; $a += 4;
    $b = [1]; $b += [2, 3];
    return [$x, $y, $z, $a, $b];
}
function undefined_left() {
    $u += 1;
    return $u;
}
var_dump(ints(10), mixed_left(), undefined_left());
?>
--EXPECTF--

Warning: Undefined variable $u in %s on line %d
array(2) {
  [0]=>
  int(20)
  [1]=>
  int(15)
}
array(5) {
  [0]=>
  float(9.223372036854776E+18)
  [1]=>
  int(7)
  [2]=>
  float(0.5)
  [3]=>
  int(4)
  [4]=>
  array(2) {
    [0]=>
    int(1)
    [1]=>
    int(3)
  }
}
int(1)

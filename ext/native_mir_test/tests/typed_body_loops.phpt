--TEST--
Native: integer loops with bitwise operations run in a typed body
--DESCRIPTION--
A loop of longs keeps its CVs in registers in a typed body: scalar CV
assignments, bitwise operations (whose double operand fails the body, so the
call runs again through its Zend entry and converts with Zend's diagnostics)
and the interrupt poll at the back edge.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function mix(int $n) {
    $acc = $n;
    for ($i = 0; $i < 1000; $i++) {
        $acc = ($acc * 3 + ($i & 7)) & 0x3FFFFFFF;
    }
    return $acc;
}
function masks(int $n) {
    $a = $n;
    $b = 0;
    for ($i = 0; $i < 64; $i++) {
        $a = ($a ^ ($i | 5)) & 0xFFFF;
        $b = $b | ($a & 1);
    }
    return $a + $b;
}
function nested(int $n) {
    $sum = 0;
    for ($i = 0; $i < $n; $i++) {
        for ($j = 0; $j < $n; $j++) {
            $sum = ($sum + ($i ^ $j)) & 0xFFFFFF;
        }
    }
    return $sum;
}
function overflowing(int $n) {
    $acc = $n;
    for ($i = 0; $i < 70; $i++) {
        $acc = $acc * 2 + 1;
    }
    return $acc & 0xFFFF;
}
function fractional(int $n) {
    $acc = $n;
    for ($i = 0; $i < 3; $i++) {
        $acc = ($acc * 1.5) & 0xFF;
    }
    return $acc;
}
function caller() {
    return [mix(1), masks(7), nested(30), fractional(3)];
}
var_dump(caller());
var_dump(overflowing(1));
var_dump(mix(PHP_INT_MAX));
?>
--EXPECTF--

Deprecated: Implicit conversion from float 4.5 to int loses precision in %s on line 37
array(4) {
  [0]=>
  int(569837813)
  [1]=>
  int(8)
  [2]=>
  int(13890)
  [3]=>
  int(9)
}

Warning: The float 2.3611832414348226E+21 is not representable as an int, cast occurred in %s on line 32
int(0)

Warning: The float 2.7670116110564327E+19 is not representable as an int, cast occurred in %s on line 5
int(238988756)

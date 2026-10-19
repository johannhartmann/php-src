--TEST--
Native (float) and (int) casts of numbers convert inline
--DESCRIPTION--
(float) and (int) of a long or a double convert inline; a double outside
the long range, NaN, infinities, strings, null, booleans, arrays, objects,
references and undefined variables keep the helper and its diagnostics.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function f($x) { return [(float)$x, (int)$x]; }
function g(int $i) { $s = 0.0; for ($k = 0; $k < 5; $k++) { $s += sqrt((float)($i + $k)) + (int)($i / 3); } return $s; }
$vals = [0, 1, -1, 7, PHP_INT_MAX, PHP_INT_MIN, 2.5, -2.5, 0.0, -0.0, 1e18, -1e18, 9.2233720368547758E+18, -9.2233720368547758E+18, 1e19, -1e19, NAN, INF, -INF, "12", "3.7", "abc", "", null, true, false, [1], [], 0.9999999999999999, -0.9999999999999999, 4503599627370497.0];
foreach ($vals as $v) { echo json_encode(@f($v), JSON_PARTIAL_OUTPUT_ON_ERROR), "\n"; }
foreach ([0, 1, -1, 7] as $v) { var_dump(f($v)); }
var_dump(f(NAN), f(1e19), f(-INF));
$o = new stdClass; try { var_dump(f($o)); } catch (Throwable $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
$r = 5; $ref = &$r; var_dump((float)$r, (int)$ref);
var_dump(g(10), g(-100));
$acc = 0.0; for ($i = 1; $i <= 100; $i++) { $acc += sqrt((float)$i) + sin((float)$i) + (int)(float)$i; } var_dump($acc);
function u() { return (float)$undefined + (int)$alsoundefined; } var_dump(u());
?>
--EXPECTF--
[0,0]
[1,1]
[-1,-1]
[7,7]
[9.223372036854776e+18,9223372036854775807]
[-9.223372036854776e+18,-9223372036854775808]
[2.5,2]
[-2.5,-2]
[0,0]
[-0,0]
[1.0e+18,1000000000000000000]
[-1.0e+18,-1000000000000000000]
[9.223372036854776e+18,-9223372036854775808]
[-9.223372036854776e+18,-9223372036854775808]
[1.0e+19,-8446744073709551616]
[-1.0e+19,8446744073709551616]
[0,0]
[0,0]
[0,0]
[12,12]
[3.7,3]
[0,0]
[0,0]
[0,0]
[1,1]
[0,0]
[1,1]
[0,0]
[0.9999999999999999,0]
[-0.9999999999999999,0]
[4503599627370497,4503599627370497]
array(2) {
  [0]=>
  float(0)
  [1]=>
  int(0)
}
array(2) {
  [0]=>
  float(1)
  [1]=>
  int(1)
}
array(2) {
  [0]=>
  float(-1)
  [1]=>
  int(-1)
}
array(2) {
  [0]=>
  float(7)
  [1]=>
  int(7)
}

Warning: The float NAN is not representable as an int, cast occurred in %s on line 2

Warning: The float 1.0E+19 is not representable as an int, cast occurred in %s on line 2

Warning: The float -INF is not representable as an int, cast occurred in %s on line 2
array(2) {
  [0]=>
  float(NAN)
  [1]=>
  int(0)
}
array(2) {
  [0]=>
  float(1.0E+19)
  [1]=>
  int(-8446744073709551616)
}
array(2) {
  [0]=>
  float(-INF)
  [1]=>
  int(0)
}

Warning: Object of class stdClass could not be converted to float in %s on line 2

Warning: Object of class stdClass could not be converted to int in %s on line 2
array(2) {
  [0]=>
  float(1)
  [1]=>
  int(1)
}
float(5)
int(5)
float(32.29021272789946)
float(NAN)
float(5721.33577608949)

Warning: Undefined variable $undefined in %s on line 12

Warning: Undefined variable $alsoundefined in %s on line 12
float(0)

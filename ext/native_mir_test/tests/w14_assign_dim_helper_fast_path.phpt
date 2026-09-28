--TEST--
Native assign_dim helper stores integer-keyed scalars and strings directly
--DESCRIPTION--
The ASSIGN_DIM helper writes $a[int] = scalar-or-string and $a[] = value on
an array container with an unused result through one hash lookup. Shared,
immutable and referenced arrays separate first; reference elements, a full
next index, undefined values and other operands keep the general path.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D { public $n; function __construct($n){$this->n=$n;} function __destruct(){ echo "d{$this->n} "; } }
function f($n) {
    $a = [];
    for ($i = 0; $i < $n; $i++) { $a[$i] = $i * 2; }
    $a[] = "x" . $n; $a[1] = str_repeat("s", 3); $a[1] = 1.5; $a[1] = null;
    $b = $a; $b[0] = 99; echo $a[0], " ", $b[0], "\n";
    $r = &$a[2]; $a[2] = 7; echo $r, "\n"; unset($r);
    $h = ["k" => 1]; $h[5] = true; $h[] = false; $h[-3] = "neg"; var_dump($h);
    $o = [new D(1)]; $o[0] = 5; echo "\n";
    $ref = [1]; $rr = &$ref; $rr[3] = 4; var_dump($ref);
    $im = [1, 2, 3]; $im[1] = 9; var_dump($im);
    $x = [PHP_INT_MAX => 1]; try { $x[] = 2; } catch (Error $e) { echo get_class($e), "\n"; }
    $s = "ab"; $key = 1; $a[$key] = $s; $s .= "c"; echo $a[1], "\n";
    $u = [1]; $u[0] = $undef ?? 3; $u[1] = @$undef2; var_dump($u);
    return count($a);
}
for ($j = 0; $j < 3; $j++) echo f(3 + $j), "\n";

?>

--EXPECT--
0 99
7
array(4) {
  ["k"]=>
  int(1)
  [5]=>
  bool(true)
  [6]=>
  bool(false)
  [-3]=>
  string(3) "neg"
}
d1 
array(2) {
  [0]=>
  int(1)
  [3]=>
  int(4)
}
array(3) {
  [0]=>
  int(1)
  [1]=>
  int(9)
  [2]=>
  int(3)
}
Error
ab
array(2) {
  [0]=>
  int(3)
  [1]=>
  NULL
}
4
0 99
7
array(4) {
  ["k"]=>
  int(1)
  [5]=>
  bool(true)
  [6]=>
  bool(false)
  [-3]=>
  string(3) "neg"
}
d1 
array(2) {
  [0]=>
  int(1)
  [3]=>
  int(4)
}
array(3) {
  [0]=>
  int(1)
  [1]=>
  int(9)
  [2]=>
  int(3)
}
Error
ab
array(2) {
  [0]=>
  int(3)
  [1]=>
  NULL
}
5
0 99
7
array(4) {
  ["k"]=>
  int(1)
  [5]=>
  bool(true)
  [6]=>
  bool(false)
  [-3]=>
  string(3) "neg"
}
d1 
array(2) {
  [0]=>
  int(1)
  [3]=>
  int(4)
}
array(3) {
  [0]=>
  int(1)
  [1]=>
  int(9)
  [2]=>
  int(3)
}
Error
ab
array(2) {
  [0]=>
  int(3)
  [1]=>
  NULL
}
6

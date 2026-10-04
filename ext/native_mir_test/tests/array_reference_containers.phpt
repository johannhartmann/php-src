--TEST--
Native x64 array accesses read through reference containers
--DESCRIPTION--
Reads, isset, appends and writes on a by-reference array parameter dereference the container inline; register reads copy scalar elements of any type and publish temporaries to their slots.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function rd($n, &$a) { $s = 0; for ($i = 0; $i < $n; $i++) { $s += $a[$i]; if (isset($a[$i + 1])) $s++; } return $s; }
function wr($n, &$a) { for ($i = 0; $i < $n; $i++) { $a[$i] = $a[$i] * 2; $a[] = $i; } }
function hs($n, &$ra) { $x = $ra[1]; for ($j = 1; $j < $n; $j++) { if ($ra[$j] < $ra[$j + 1]) { $ra[$j] = $ra[$j + 1]; } } return $x; }
$a = [1, 2, 3, 4.5, "x" => 5]; var_dump(rd(4, $a));
$b = [1, 2, 3]; wr(3, $b); var_dump($b);
$c = [0, 3.5, 1.5, 9.0, 2.0]; var_dump(hs(4, $c), $c);
$d = [1, 2]; $e = &$d; $f = $d; wr(2, $d); var_dump($d, $f);
$g = "str"; try { var_dump(rd(1, $g)); } catch (Error $x) { echo get_class($x), "\n"; }
$h = null; wr(1, $h); var_dump($h);
?>
--EXPECTF--
float(13.5)
array(6) {
  [0]=>
  int(2)
  [1]=>
  int(4)
  [2]=>
  int(6)
  [3]=>
  int(0)
  [4]=>
  int(1)
  [5]=>
  int(2)
}
float(3.5)
array(5) {
  [0]=>
  int(0)
  [1]=>
  float(3.5)
  [2]=>
  float(9)
  [3]=>
  float(9)
  [4]=>
  float(2)
}
array(4) {
  [0]=>
  int(2)
  [1]=>
  int(4)
  [2]=>
  int(0)
  [3]=>
  int(1)
}
array(2) {
  [0]=>
  int(1)
  [1]=>
  int(2)
}
TypeError

Warning: Trying to access array offset on null in %s on line 3
array(2) {
  [0]=>
  int(0)
  [1]=>
  int(0)
}

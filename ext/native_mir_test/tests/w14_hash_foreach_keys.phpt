--TEST--
Native x64 foreach yields hash keys and values inline
--DESCRIPTION--
Integer and string keys, deleted buckets and packed arrays with keys.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function f($h) { $s = 0; $ks = ""; foreach ($h as $k => $v) { $s += $v; $ks .= $k . ","; } return [$s, $ks]; }
function g($h) { $s = 0; foreach ($h as $v) { $s += $v; } return $s; }
function p($h) { $o = []; foreach ($h as $k => $v) { $o[] = $k; } return $o; }
$a = ["a" => 1, "b" => 2, 5 => 3, "c" => 4]; unset($a["b"]);
var_dump(f($a), g($a), p($a), f([1, 2, 3]), p([10, 20]), f([]), f(["x" => 1.5, "y" => 2]));
$big = []; for ($i = 0; $i < 100; $i++) $big["k$i"] = $i; unset($big["k5"]); var_dump(f($big)[0]);
$keys = []; foreach ($big as $k => $v) { $keys[] = $k; } var_dump(count($keys), $keys[0]);
?>
--EXPECTF--
array(2) {
  [0]=>
  int(8)
  [1]=>
  string(6) "a,5,c,"
}
int(8)
array(3) {
  [0]=>
  string(1) "a"
  [1]=>
  int(5)
  [2]=>
  string(1) "c"
}
array(2) {
  [0]=>
  int(6)
  [1]=>
  string(6) "0,1,2,"
}
array(2) {
  [0]=>
  int(0)
  [1]=>
  int(1)
}
array(2) {
  [0]=>
  int(0)
  [1]=>
  string(0) ""
}
array(2) {
  [0]=>
  float(3.5)
  [1]=>
  string(4) "x,y,"
}
int(4945)
int(99)
string(2) "k0"

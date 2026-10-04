--TEST--
Native inline calls pass register scalars without an exact descriptor type
--DESCRIPTION--
A loop counter or other scalar held in a register may reach an inline call
frame whose descriptor records no exact type for the argument; its payload
and type are stored instead of copying a frame slot through the value.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function h($m, $f, $i) { return is_array($m) ? "$f:$i:" . count($m) : $i; }
function r($m) { $out = []; for ($i = 0; count($m) > $i; $i++) { $out[] = h($m, "s", $i); } return $out; }
function f($m) { $out = []; for ($x = 0.5; $x < 3; $x += 1.0) { $out[] = h($m, "d", $x); } $b = count($m) > 1; $out[] = h($m, "b", $b); return $out; }
var_dump(r([1, 2, 3]), f([1, 2]));

?>

--EXPECT--
array(3) {
  [0]=>
  string(5) "s:0:3"
  [1]=>
  string(5) "s:1:3"
  [2]=>
  string(5) "s:2:3"
}
array(4) {
  [0]=>
  string(7) "d:0.5:2"
  [1]=>
  string(7) "d:1.5:2"
  [2]=>
  string(7) "d:2.5:2"
  [3]=>
  string(5) "b:1:2"
}

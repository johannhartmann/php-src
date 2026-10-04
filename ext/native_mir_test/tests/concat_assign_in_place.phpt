--TEST--
Native .= extends a uniquely owned string in place
--DESCRIPTION--
CONCAT_ASSIGN with a string CV and a string CONST or CV operand extends a
non-interned string with refcount 1 in place. Shared, interned and
self-appended strings, references and non-string operands take the general
path.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function f($n) { $s = ""; $t = "ab"; for ($i = 0; $i < $n; $i++) { $s .= "x"; $s .= $t; $s .= $i; } return $s; }
function g() { $a = "q"; $b = $a; $b .= "r"; $c = "z"; $d = &$c; $d .= "w"; $e = "e"; $e .= $e; $f = 5; $f .= "x"; $h = str_repeat("k", 3); $h .= ""; return [$a, $b, $c, $d, $e, $f, $h]; }
var_dump(f(5), strlen(f(1000)), g());

?>

--EXPECT--
string(20) "xab0xab1xab2xab3xab4"
int(5890)
array(7) {
  [0]=>
  string(1) "q"
  [1]=>
  string(2) "qr"
  [2]=>
  string(2) "zw"
  [3]=>
  string(2) "zw"
  [4]=>
  string(2) "ee"
  [5]=>
  string(2) "5x"
  [6]=>
  string(3) "kkk"
}

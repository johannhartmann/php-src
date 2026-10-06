--TEST--
Identity tests of temporaries against variables and temporaries
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D { function __destruct() { echo "d\n"; } }
function k($x) { return $x; }
function f($a, $s) {
	$r = [];
	$r[] = $a === k('x');
	$r[] = $a !== strtolower($s);
	$r[] = k(1) === k(1);
	$r[] = $a === k([1]);
	$r[] = k('ab' . $s) === k('ab' . $s);
	$r[] = $a === k(new D);
	$r[] = $a === k($s . 'q');
	$r[] = $a === k(1.0);
	$r[] = $s === (string) 5;
	return $r;
}
var_dump(f('x', 'X')); var_dump(f('xq', 'x'));
--EXPECT--
d
array(9) {
  [0]=>
  bool(true)
  [1]=>
  bool(false)
  [2]=>
  bool(true)
  [3]=>
  bool(false)
  [4]=>
  bool(true)
  [5]=>
  bool(false)
  [6]=>
  bool(false)
  [7]=>
  bool(false)
  [8]=>
  bool(false)
}
d
array(9) {
  [0]=>
  bool(false)
  [1]=>
  bool(true)
  [2]=>
  bool(true)
  [3]=>
  bool(false)
  [4]=>
  bool(true)
  [5]=>
  bool(false)
  [6]=>
  bool(true)
  [7]=>
  bool(false)
  [8]=>
  bool(false)
}

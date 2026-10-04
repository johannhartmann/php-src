--TEST--
Native ! and (bool) of untyped scalars
--DESCRIPTION--
! and (bool) of an untyped null, boolean or integer decide inline; undefined variables, references and other types keep the helper.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function nots($v) { $a = !$v; $b = (bool) $v; $c = !!$v; return [$a, $b, $c, !$v ? 'y' : 'n']; }
foreach ([null, false, true, 0, 1, -5, PHP_INT_MIN, '', '0', 'a', [], [0], 0.0, 0.5, NAN, new stdClass] as $v) {
    echo json_encode(nots($v)), "\n";
}
$x = 0; $r = &$x; var_dump(nots($r));
function undef_not() { return @!$undefined; }
var_dump(undef_not());
function loopnot(array $xs) { $n = 0; foreach ($xs as $x) { if (!$x) $n++; } return $n; }
var_dump(loopnot([0, 1, null, false, true, 2, '', 'x']));
?>
--EXPECTF--
[true,false,false,"y"]
[true,false,false,"y"]
[false,true,true,"n"]
[true,false,false,"y"]
[false,true,true,"n"]
[false,true,true,"n"]
[false,true,true,"n"]
[true,false,false,"y"]
[true,false,false,"y"]
[false,true,true,"n"]
[true,false,false,"y"]
[false,true,true,"n"]
[true,false,false,"y"]
[false,true,true,"n"]

Warning: unexpected NAN value was coerced to bool in %s on line 2

Warning: unexpected NAN value was coerced to bool in %s on line 2

Warning: unexpected NAN value was coerced to bool in %s on line 2

Warning: unexpected NAN value was coerced to bool in %s on line 2
[false,true,true,"n"]
[false,true,true,"n"]
array(4) {
  [0]=>
  bool(true)
  [1]=>
  bool(false)
  [2]=>
  bool(false)
  [3]=>
  string(1) "y"
}
bool(true)
int(4)

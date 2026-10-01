--TEST--
Native && and || decide null, boolean and integer conditions inline
--DESCRIPTION--
JMPZ_EX and JMPNZ_EX of a null, boolean or integer condition decide in
the guard and publish the boolean result on both edges, as the VM does.
A condition the cold block materializes, a branch whose PHI inputs only
the cold block defines, and any other value keep the helper.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function f($a, $b) { return $a && $b; }
function g($a, $b) { return $a || $b; }
function h($a, $b) { $x = ($a == 1) && ($b == 2); $y = ($a == 3) || ($b == 4); return [$x, $y]; }
function k($a) { $r = is_array($a) && isset($a['x']); return $r ? 'yes' : 'no'; }
class BT { public $cb; function __construct($cb) { $this->cb = $cb; } function isd() { return is_callable($this->cb); } }
class B { public $name; public $bt;
  function __construct($n, $bt) { $this->name = $n; $this->bt = $bt; }
  function render($options) {
    $is_dynamic = $options['dynamic'] && $this->name && null !== $this->bt && $this->bt->isd();
    return $is_dynamic ? 'dyn' : 'static';
  }
}
echo json_encode([f(true, 1), f(0, true), f(1, 0), f(null, 1), g(0, 0), g(0, 5), g(true, 0), g(null, null), h(1, 2), h(3, 9), k(['x' => 1]), k([]), k(5)]), "\n";
$out = [];
foreach ([null, '', 'x', 0, 1] as $n) foreach ([null, new BT(null), new BT('strlen')] as $bt) foreach ([true, false, 1, 0, null] as $d) {
  $out[] = (new B($n, $bt))->render(['dynamic' => $d]);
}
echo implode(' ', $out), "\n";
?>
--EXPECT--
[true,false,false,false,false,true,true,false,[true,false],[false,true],"yes","no","no"]
static static static static static static static static static static static static static static static static static static static static static static static static static static static static static static static static static static static static static static static static dyn static dyn static static static static static static static static static static static static static static static static static static static static static static static static static static static dyn static dyn static static

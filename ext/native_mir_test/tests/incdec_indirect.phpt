--TEST--
Native ++ and -- through a write fetch
--DESCRIPTION--
++$a[k] and $a[k]-- update an integer element or an untyped reference's integer through the INDIRECT the write fetch left; overflow, other types, typed references and ArrayAccess objects keep the helper.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function incs() {
    $a = ['n' => 1, 's' => 'x', 'f' => 1.5, 'nul' => null, 'max' => PHP_INT_MAX, 'min' => PHP_INT_MIN, 'arr' => ['k' => 5]];
    $r = [];
    $r[] = ++$a['n']; $r[] = $a['n']++; $r[] = $a['n']; --$a['n']; $a['n']--;
    $r[] = $a['n'];
    ++$a['s']; $r[] = $a['s'];
    ++$a['f']; $r[] = $a['f'];
    ++$a['nul']; $r[] = $a['nul'];
    ++$a['max']; $r[] = $a['max'];
    --$a['min']; $r[] = $a['min'];
    @++$a['missing']; $r[] = $a['missing'];
    ++$a['arr']['k']; $r[] = $a['arr']['k'];
    $x = 10; $a['ref'] = &$x; ++$a['ref']; $r[] = $x;
    $counts = [];
    foreach (['a', 'b', 'a', 'c', 'a'] as $w) { if (!isset($counts[$w])) $counts[$w] = 0; $counts[$w]++; }
    $r[] = json_encode($counts);
    for ($i = 0; $i < 3; $i++) { $a['loop'] = ($a['loop'] ?? 0); ++$a['loop']; }
    $r[] = $a['loop'];
    return $r;
}
var_dump(incs());
class AA implements ArrayAccess { public $d = ['x' => 1];
  function offsetExists($o): bool { return isset($this->d[$o]); } function offsetGet($o): mixed { return $this->d[$o]; }
  function offsetSet($o, $v): void { $this->d[$o] = $v; } function offsetUnset($o): void { unset($this->d[$o]); } }
$o = new AA; @$o['x']++; var_dump($o->d);
?>
--EXPECTF--

Deprecated: Increment on non-numeric string is deprecated, use str_increment() instead in %s on line 7
array(14) {
  [0]=>
  int(2)
  [1]=>
  int(2)
  [2]=>
  int(3)
  [3]=>
  int(1)
  [4]=>
  string(1) "y"
  [5]=>
  float(2.5)
  [6]=>
  int(1)
  [7]=>
  float(9.223372036854776E+18)
  [8]=>
  float(-9.223372036854776E+18)
  [9]=>
  int(1)
  [10]=>
  int(6)
  [11]=>
  int(11)
  [12]=>
  string(19) "{"a":3,"b":1,"c":1}"
  [13]=>
  int(3)
}
array(1) {
  ["x"]=>
  int(1)
}

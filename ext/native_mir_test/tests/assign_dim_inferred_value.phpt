--TEST--
Native ASSIGN_DIM of values whose type inference rules out checks
--DESCRIPTION--
An inline array assignment tests its value only for what Zend's type
inference allows: a typed CV needs no undefined or reference test and an
integer needs no reference count, while an untyped CV keeps both.
--EXTENSIONS--
native_mir_test
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class P {
    public $v = 0;
    function set(int $n) { $this->v = $n; return $this->v; }
}
function typed(array $a, string $s, int $i, array $x) {
    $a['s'] = $s;
    $a['i'] = $i;
    $a['x'] = $x;
    $a[] = $s . $i;
    return $a;
}
function untyped($a, $v) {
    $a['v'] = $v;
    $a['u'] = @$undefined;
    return $a;
}
function by_ref($a) {
    $r = 1;
    $ref = &$r;
    $a['r'] = $ref;
    $r = 2;
    return $a;
}
$p = new P;
$out = [];
for ($k = 0; $k < 40; $k++) {
    $s = str_repeat('s', $k % 3);
    $x = [$k];
    $a = typed(['z' => 1], $s, $k, $x);
    $x[] = 1;
    $out = [$a, untyped([], $s), by_ref([]), $p->set($k), count($x)];
}
var_dump($out);
?>
--EXPECT--
array(5) {
  [0]=>
  array(5) {
    ["z"]=>
    int(1)
    ["s"]=>
    string(0) ""
    ["i"]=>
    int(39)
    ["x"]=>
    array(1) {
      [0]=>
      int(39)
    }
    [0]=>
    string(2) "39"
  }
  [1]=>
  array(2) {
    ["v"]=>
    string(0) ""
    ["u"]=>
    NULL
  }
  [2]=>
  array(1) {
    ["r"]=>
    int(1)
  }
  [3]=>
  int(39)
  [4]=>
  int(2)
}

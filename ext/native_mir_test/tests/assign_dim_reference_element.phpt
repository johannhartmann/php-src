--TEST--
Native ASSIGN_DIM stores into reference and symbol table elements directly
--DESCRIPTION--
The address form assigns through an untyped reference element, a typed
reference element (with its TypeError and coercion) and a symbol table copy
with CV, temporary and literal values, without leaving its fast path.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
class C { public int $i = 0; }
function plain($k, $v) {
    $x = 1;
    $a = ['p' => 0, 'q' => 0];
    $a['p'] = &$x;
    $a[$k] = $v;
    $a[$k] = $v . 's';
    $a['q'] = $v;
    return [$a, $x];
}
function typed($k) {
    $c = new C;
    $a = ['i' => 0];
    $a['i'] = &$c->i;
    try {
        $a[$k] = "abc";
    } catch (TypeError $e) {
        echo $e->getMessage(), "\n";
    }
    $a[$k] = "5";
    $a[$k] = 7;
    return $c->i;
}
function symbols($k) {
    $GLOBALS['zz'] = 1;
    $t = $GLOBALS;
    $t[$k] = 2;
    return [$t[$k], $GLOBALS['zz']];
}
var_dump(plain('p', 'new'));
var_dump(typed('i'));
var_dump(symbols('zz'));
?>
--EXPECT--
array(2) {
  [0]=>
  array(2) {
    ["p"]=>
    string(4) "news"
    ["q"]=>
    string(3) "new"
  }
  [1]=>
  string(4) "news"
}
Cannot assign string to reference held by property C::$i of type int
int(7)
array(2) {
  [0]=>
  int(2)
  [1]=>
  int(1)
}

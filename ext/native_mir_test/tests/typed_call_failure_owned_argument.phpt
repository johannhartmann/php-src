--TEST--
Native typed call that fails releases its owned argument once
--DESCRIPTION--
A typed body that fails (an int overflow returned as int) repeats through the
canonical call, which consumes the property copy passed as argument; the hot
edge must not drop that copy's reference as well.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function leaf(array $values, int $x): int
{
    if ($x > 5) {
        return $x + 1;
    }
    return $x + 2;
}
function root(object $o, int $x)
{
    return leaf($o->v, $x);
}
$o = new stdClass;
$o->v = [str_repeat('a', 3), str_repeat('b', 3)];
for ($i = 0; $i < 3; $i++) {
    echo root($o, $i), "\n";
}
try {
    root($o, PHP_INT_MAX);
} catch (TypeError $e) {
    echo $e->getMessage(), "\n";
}
unset($e);
$fill = [];
for ($i = 0; $i < 8; $i++) {
    $fill[] = [str_repeat('x', 3), str_repeat('y', 3)];
}
var_dump($o->v);
?>
--EXPECT--
2
3
4
leaf(): Return value must be of type int, float returned
array(2) {
  [0]=>
  string(3) "aaa"
  [1]=>
  string(3) "bbb"
}

--TEST--
Native foreach overwrites a counted loop variable inline only when safe
--DESCRIPTION--
The loop variable holds a counted value the array still owns (overwritten
inline), the last reference to an object with a destructor (destroyed at
the overwrite, in order), a reference (assigned through it) or an array.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
class D { function __construct(public $n) {} function __destruct() { echo "~{$this->n} "; } }
function strings(array $a) {
    $out = [];
    foreach ($a as $v) { $out[] = strlen($v); }
    return implode(',', $out) . ' last=' . $v;
}
function last_owner() {
    $v = new D('first');
    foreach ([1, 2] as $v) { echo "it$v "; }
    echo "\n";
}
function through_reference() {
    $target = 'old';
    $v = &$target;
    foreach (['x', 'y'] as $v) {}
    return $target;
}
function nested_arrays(array $rows) {
    $n = 0;
    foreach ($rows as $row) { $n += count($row); }
    return $n;
}
$a = [str_repeat('a', 3), str_repeat('bb', 2), 'lit'];
echo strings($a), "\n";
last_owner();
echo through_reference(), "\n";
echo nested_arrays([[1, 2], [3], range(1, 4)]), "\n";
$objs = [new D('o1'), new D('o2')];
foreach ($objs as $o) { echo $o->n, ' '; }
unset($objs);
echo "\n";
unset($o);
echo "\ndone\n";
?>
--EXPECT--
3,4,3 last=lit
~first it1 it2 
y
7
o1 o2 ~o1 
~o2 
done

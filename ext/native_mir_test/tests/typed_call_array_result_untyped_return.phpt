--TEST--
Native untyped function returns a typed call's array result directly
--DESCRIPTION--
The array result of a typed-body call lives in a register; an untyped caller
that returns it at once must return that register, not its unwritten slot.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function leaf(array $values, int $x): array { return $values; }
function root_cv(array $a, int $x) { return leaf($a, $x); }
function root_elem(array $a, int $x) { return leaf($a[0], $x); }
function root_typed(object $o, int $x): array { return leaf($o->v, $x); }
function root_assign(object $o, int $x) { $r = leaf($o->v, $x); return $r; }
$o = new stdClass;
$o->v = range(1, 2);
for ($i = 0; $i < 3; $i++) {
    echo json_encode([root_cv([1, $i], $i), root_elem([[3, $i]], $i),
        root_typed($o, $i), root_assign($o, $i)]), "\n";
    root_cv([str_repeat('a', $i + 1)], $i);
}
$base = [str_repeat('b', 2)];
var_dump(root_cv($base, 0));
var_dump($base);
?>
--EXPECT--
[[1,0],[3,0],[1,2],[1,2]]
[[1,1],[3,1],[1,2],[1,2]]
[[1,2],[3,2],[1,2],[1,2]]
array(1) {
  [0]=>
  string(2) "bb"
}
array(1) {
  [0]=>
  string(2) "bb"
}

--TEST--
Native === and !== of two variables
--DESCRIPTION--
=== and !== of a variable and a CV decide null, booleans, integers and strings (interned, equal, different lengths, word and byte tails) inline; doubles, arrays, objects, references and undefined variables keep the helper.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function cmp_all($a, $b) {
    $r = [];
    $r[] = $a === $b; $r[] = $a !== $b;
    $t = $a . '';
    $r[] = ($a . '') === $b;
    if ($a === $b) { $r[] = 'eq'; } else { $r[] = 'ne'; }
    return implode(',', array_map('var_export', $r, array_fill(0, count($r), true)));
}
$cases = [['abc','abc'], ['abc','abd'], ['abc','abcd'], [str_repeat('x', 20), str_repeat('x', 20)],
  [str_repeat('x', 20), str_repeat('x', 19).'y'], [1, 1], [1, 2], [1, '1'], [true, true], [true, false],
  [null, null], [null, false], [1.5, 1.5], [NAN, NAN], [[1], [1]], [[1], [2]], ['', ''], ['a', '']];
foreach ($cases as [$x, $y]) { echo cmp_all($x, $y), "\n"; }
$o = new stdClass; echo cmp_all($o, $o), "\n", cmp_all($o, new stdClass), "\n";
$s = 'abc'; $ref = &$s; $other = 'abc';
function by_ref_cmp(&$p, $q) { return [$p === $q, $q === $p]; }
var_dump(by_ref_cmp($s, $other));
function undef_cmp($q) { return @($undefined === $q); }
var_dump(undef_cmp(null), undef_cmp(1));
function loop_cmp(array $xs, $needle) { $n = 0; foreach ($xs as $x) { if ($x === $needle) $n++; } return $n; }
var_dump(loop_cmp(['a', 'b', 'a', 1, 'a'], 'a'), loop_cmp([1, 2, 1, '1'], 1));
?>
--EXPECTF--
true,false,true,'eq'
false,true,false,'ne'
false,true,false,'ne'
true,false,true,'eq'
false,true,false,'ne'
true,false,false,'eq'
false,true,false,'ne'
false,true,true,'ne'
true,false,false,'eq'
false,true,false,'ne'
true,false,false,'eq'
false,true,false,'ne'
true,false,false,'eq'

Warning: unexpected NAN value was coerced to string in %s on line 5

Warning: unexpected NAN value was coerced to string in %s on line 6
false,true,false,'ne'

Warning: Array to string conversion in %s on line 5

Warning: Array to string conversion in %s on line 6
true,false,false,'eq'

Warning: Array to string conversion in %s on line 5

Warning: Array to string conversion in %s on line 6
false,true,false,'ne'
true,false,true,'eq'
false,true,false,'ne'

Fatal error: Uncaught Error: Object of class stdClass could not be converted to string in %s:5
Stack trace:
#0 %s(14): cmp_all(Object(stdClass), Object(stdClass))
#1 {main}
  thrown in %s on line 5

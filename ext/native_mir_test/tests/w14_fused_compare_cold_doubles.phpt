--TEST--
Native fused comparison branches decide longs inline and doubles out of line
--DESCRIPTION--
<, <=, == and != branches of long, double and mixed operands (NaN compares
false except !=), references and other types through the helper, and
known-boolean and plain truthiness conditions.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function cmp($a, $b) {
    $r = '';
    if ($a < $b) { $r .= 'l'; } else { $r .= '-'; }
    if ($a <= $b) { $r .= 'e'; } else { $r .= '-'; }
    if ($a == $b) { $r .= 'q'; } else { $r .= '-'; }
    if ($a != $b) { $r .= 'n'; } else { $r .= '-'; }
    return $r;
}
function truth($v) {
    $b = $v > 0;
    if ($b) { return 'T'; }
    if ($v) { return 't'; }
    return 'f';
}
$nan = NAN;
$x = 3;
$ref = &$x;
$cases = [[1, 2], [2, 2], [3, 2], [1.5, 2], [2, 2.0], [2.5, 1.5], [-0.0, 0.0],
    [$nan, 1.0], [1, $nan], [$nan, $nan], ['10', 9], ['abc', 'abd'], [null, 0],
    [true, 1], [[1], [1]], [$ref, 3], [PHP_INT_MAX, PHP_INT_MAX + 1]];
foreach ($cases as [$a, $b]) {
    echo cmp($a, $b), ' ';
}
echo "\n";
foreach ([1, 0, -1, 0.5, 0.0, '0', '', 'a', [], [0], null] as $v) {
    echo truth($v);
}
echo "\n";
?>
--EXPECT--
le-n -eq- ---n le-n -eq- ---n -eq- ---n ---n ---n ---n le-n -eq- -eq- -eq- -eq- -eq- 
TftTfffTTTf

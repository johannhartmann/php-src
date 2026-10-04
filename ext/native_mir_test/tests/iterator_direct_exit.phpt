--TEST--
Native foreach without a value result leaves for its successors directly
--DESCRIPTION--
FE_FETCH_R into a CV, with and without a key, over packed and hash arrays,
arrays with holes, references and an array changed by the loop body: the
inline fetch continues into the body or exits, the helper path branches on
its decision.
--EXTENSIONS--
native_mir_test
--FILE--
<?php
function sum_values(array $a) {
    $s = 0;
    foreach ($a as $v) {
        $s += is_int($v) ? $v : strlen((string) $v);
    }
    return $s;
}
function keys_values(array $a) {
    $out = [];
    foreach ($a as $k => $v) {
        if ($v === 'stop') {
            break;
        }
        if ($v === 'skip') {
            continue;
        }
        $out[] = "$k=$v";
    }
    return implode(',', $out);
}
function nested(array $rows) {
    $n = 0;
    foreach ($rows as $row) {
        foreach ($row as $cell) {
            $n += $cell;
        }
    }
    return $n;
}
$holes = [1, 2, 3, 4];
unset($holes[1]);
$x = 'ref';
$refs = ['a' => &$x, 'b' => 'plain'];
echo sum_values([1, 2, 3]), "\n";
echo sum_values([]), "\n";
echo sum_values($holes), "\n";
echo sum_values(['a' => 'xx', 'b' => 5, 'c' => str_repeat('y', 3)]), "\n";
echo keys_values(['a' => 1, 'b' => 'skip', 'c' => 3, 'd' => 'stop', 'e' => 5]), "\n";
echo keys_values([10, 'skip', 30]), "\n";
echo keys_values($refs), "\n";
echo nested([[1, 2], [], [3], ['k' => 4]]), "\n";
$grow = [1, 2];
$n = 0;
foreach ($grow as $g) {
    if ($n++ < 3) {
        $grow[] = $g;
    }
}
echo $n, ' ', count($grow), "\n";
?>
--EXPECT--
6
0
8
10
a=1,c=3
0=10,2=30
a=ref,b=plain
10
2 4

--TEST--
W01 semantic corpus: dynamic named by-reference and variadic call
--FILE--
<?php
function combineValues(int &$target, int $base, int ...$extra): int
{
    $target += $base + array_sum($extra);
    return $target;
}

$callable = 'combineValues';
$value = 1;
$result = $callable(target: $value, base: 2, first: 3, second: 4);
echo "result:$result\n";
echo "reference:$value\n";

$closure = static fn (int $input): int => $input * 2;
echo 'closure:', $closure($result), "\n";
?>
--EXPECT--
result:10
reference:10
closure:20

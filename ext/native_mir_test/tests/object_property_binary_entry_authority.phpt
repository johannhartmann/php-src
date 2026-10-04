--TEST--
Native object-property operands remain authoritative across repeated entries
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
class PropertyBinaryBox
{
    public int $value;
}

function property_identical(PropertyBinaryBox $box, int $value): bool
{
    return $value === $box->value;
}

function property_subtract(PropertyBinaryBox $box, int $value): int
{
    return $value - $box->value;
}

function property_subtract_pair(
    PropertyBinaryBox $left,
    PropertyBinaryBox $right,
): int {
    return $left->value - $right->value;
}

$box = new PropertyBinaryBox();
foreach ([0, 1, 2] as $value) {
    $box->value = $value;
    var_dump(property_identical($box, $value));
}
foreach ([3, 4, 5] as $value) {
    $box->value = $value;
    var_dump(property_subtract($box, $value));
}
$left = new PropertyBinaryBox();
$right = new PropertyBinaryBox();
foreach ([[2, 1], [0, 2], [3, 0], [1, 1]] as [$a, $b]) {
    $left->value = $a;
    $right->value = $b;
    var_dump(property_subtract_pair($left, $right));
}
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
int(0)
int(0)
int(0)
int(1)
int(-2)
int(3)
int(0)

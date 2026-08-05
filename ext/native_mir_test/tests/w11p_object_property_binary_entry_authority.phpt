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

$box = new PropertyBinaryBox();
foreach ([0, 1, 2] as $value) {
    $box->value = $value;
    var_dump(property_identical($box, $value));
}
foreach ([3, 4, 5] as $value) {
    $box->value = $value;
    var_dump(property_subtract($box, $value));
}
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
int(0)
int(0)
int(0)

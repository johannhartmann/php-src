--TEST--
Native property spaceship comparison preserves condition flags
--SKIPIF--
<?php
if (!extension_loaded('native_mir_test')) {
    die('skip native_mir_test extension not available');
}
?>
--FILE--
<?php
final class NativeSpaceshipBox
{
    public function __construct(public int $value) {}
}

function nativePropertySpaceship(
    NativeSpaceshipBox $left,
    NativeSpaceshipBox $right,
): int {
    return $left->value <=> $right->value;
}

$positive = new NativeSpaceshipBox(4);
$negative = new NativeSpaceshipBox(-15);
var_dump(
    nativePropertySpaceship($positive, $positive),
    nativePropertySpaceship($negative, $negative),
    nativePropertySpaceship($negative, $positive),
    nativePropertySpaceship($positive, $negative),
);
?>
--EXPECT--
int(0)
int(0)
int(-1)
int(1)

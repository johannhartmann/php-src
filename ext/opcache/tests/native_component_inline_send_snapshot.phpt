--TEST--
Native inline component results preserve distinct internal-call arguments
--INI--
opcache.enable_cli=1
opcache.optimization_level=-1
opcache.file_update_protection=0
--FILE--
<?php
function native_component_snapshot_leaf(int $value): int
{
    return $value + 1;
}

var_dump(
    native_component_snapshot_leaf(0),
    native_component_snapshot_leaf(1),
    native_component_snapshot_leaf(41),
);
?>
--EXPECT--
int(1)
int(2)
int(42)

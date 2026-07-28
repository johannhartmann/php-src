--TEST--
Native component call results remain defined across optimized loop PHIs
--INI--
opcache.enable_cli=1
opcache.optimization_level=-1
opcache.file_update_protection=0
--FILE--
<?php
function native_component_phi_leaf(int $value): int
{
    return $value + 1;
}

function native_component_phi_root(int $count): int
{
    $value = 0;
    for ($index = 0; $index < $count; $index++) {
        $value = native_component_phi_leaf($value);
    }
    return $value;
}

foreach ([0, 1, 1000] as $count) {
    printf("%d=%d\n", $count, native_component_phi_root($count));
}
?>
--EXPECT--
0=0
1=1
1000=1000

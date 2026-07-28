--TEST--
Native register paths consume full-DFA packed and property read results
--INI--
opcache.enable_cli=1
opcache.optimization_level=-1
opcache.file_update_protection=0
--FILE--
<?php
class NativeHotReadBox
{
    public int $value = 7;
}

function native_hot_packed_read(int $count): int
{
    $values = [1, 2, 3, 4, 5, 6, 7, 8];
    $sum = 0;
    for ($index = 0; $index < $count; $index++) {
        $key = $index & 7;
        $sum += $values[$key];
    }
    return $sum;
}

function native_hot_property_read(int $count): int
{
    $box = new NativeHotReadBox();
    $sum = 0;
    for ($index = 0; $index < $count; $index++) {
        $sum += $box->value;
    }
    return $sum;
}

foreach ([0, 1, 10, 1000] as $count) {
    printf(
        "%d=%d,%d\n",
        $count,
        native_hot_packed_read($count),
        native_hot_property_read($count),
    );
}
?>
--EXPECT--
0=0,0
1=1,7
10=39,70
1000=4500,7000

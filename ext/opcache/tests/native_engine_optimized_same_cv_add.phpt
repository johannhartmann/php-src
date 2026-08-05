--TEST--
Native engine preserves an optimized same-CV ADD across loop PHIs
--INI--
opcache.enable=1
opcache.enable_cli=1
--FILE--
<?php
function native_optimized_same_cv_add(int $count, bool $exit): int
{
    $total = 0;
    for ($i = 0; $i < $count; $i++) {
        $total += false;
        if ($exit) {
            return $total;
        }
    }
    return $total;
}

function native_optimized_nested_same_cv_add(array $values, $exit): int
{
    $total = 0;
    $count = count($values);
    if ($count == 0) {
        return 0;
    }
    $copy = [];
    foreach ($values as $value) {
        $copy[] = $value;
    }
    $count = $copy[5];
    for ($i = 0; $i < $count; $i++) {
        $left = $values[$i];
        for ($k = $i + 1; $k < $count; $k++) {
            $right = $values[$k];
            $total += $left > $right;
        }
        if ($exit) {
            return $total;
        }
    }
    return $total;
}

var_dump(native_optimized_same_cv_add(6, true));
var_dump(native_optimized_same_cv_add(6, false));
var_dump(native_optimized_nested_same_cv_add([1, 2, 3, 4, 5, 6], true));
var_dump(native_optimized_nested_same_cv_add([1, 2, 3, 4, 5, 6], true));
var_dump(native_optimized_nested_same_cv_add([1, 2, 3, 4, 5, 6], false));
?>
--EXPECT--
int(0)
int(0)
int(0)
int(0)
int(0)

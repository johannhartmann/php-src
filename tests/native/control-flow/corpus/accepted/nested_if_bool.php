<?php
function w04_nested_if_bool(bool $left, bool $right) {
    if ($left) {
        if ($right) {
            return 3;
        }
        return 2;
    }
    return 1;
}
echo w04_nested_if_bool(false, false), ",", w04_nested_if_bool(true, false), ",", w04_nested_if_bool(true, true), "\n";

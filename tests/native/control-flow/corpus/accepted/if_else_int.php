<?php
function w04_if_else_int(int $value): int {
    if ($value > 0) {
        return $value + 1;
    } else {
        return $value - 1;
    }
}
echo w04_if_else_int(-2), ",", w04_if_else_int(3), "\n";

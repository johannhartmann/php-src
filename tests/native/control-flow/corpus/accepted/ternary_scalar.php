<?php
function w04_ternary_scalar(bool $condition, int $left, int $right) {
    return $condition ? $left : $right;
}
echo w04_ternary_scalar(true, 7, 9), ",", w04_ternary_scalar(false, 7, 9), "\n";

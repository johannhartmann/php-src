<?php
function w04_empty_fallthrough(bool $condition, int $value) {
    if ($condition) {
    }
    return $value;
}
echo w04_empty_fallthrough(false, 5), ",", w04_empty_fallthrough(true, 6), "\n";

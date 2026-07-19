<?php
function w04_short_circuit_and(bool $left, bool $right): bool {
    return $left && $right;
}
echo (int) w04_short_circuit_and(false, true), ",", (int) w04_short_circuit_and(true, true), "\n";

<?php
function w04_short_circuit_or(bool $left, bool $right): bool {
    return $left || $right;
}
echo (int) w04_short_circuit_or(false, false), ",", (int) w04_short_circuit_or(false, true), "\n";

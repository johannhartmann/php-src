<?php
function w04_switch_or_match_if_not_profiled(int $value): int {
    return match ($value) {
        0 => 10,
        1 => 20,
        default => 30,
    };
}
echo w04_switch_or_match_if_not_profiled(0), ",", w04_switch_or_match_if_not_profiled(2), "\n";

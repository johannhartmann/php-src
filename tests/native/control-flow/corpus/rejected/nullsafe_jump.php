<?php
function w04_nullsafe_jump(?object $value): int {
    if ($value?->enabled) {
        return 1;
    }
    return 0;
}
echo w04_nullsafe_jump(null), "\n";

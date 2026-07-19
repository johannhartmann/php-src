<?php
function w04_array_condition(array $value): int {
    if ($value) {
        return 1;
    }
    return 0;
}
echo w04_array_condition([]), ",", w04_array_condition([1]), "\n";

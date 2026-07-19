<?php
function w04_call_inside_branch(bool $condition, string $value): int {
    if ($condition) {
        return strlen($value);
    }
    return 0;
}
echo w04_call_inside_branch(false, "abc"), ",", w04_call_inside_branch(true, "abc"), "\n";

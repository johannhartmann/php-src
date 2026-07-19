<?php
function w04_string_condition_without_profile(string $value) {
    if ($value) {
        return 1;
    }
    return 0;
}
echo w04_string_condition_without_profile(""), ",", w04_string_condition_without_profile("x"), "\n";

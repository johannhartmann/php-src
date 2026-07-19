<?php
function w04_missing_condition_type_proof(mixed $value) {
    if ($value) {
        return 1;
    }
    return 0;
}
echo w04_missing_condition_type_proof(0), ",", w04_missing_condition_type_proof(1), "\n";

<?php
function w04_reference_loop_phi(array &$values) {
    $total = 0;
    foreach ($values as &$value) {
        $value++;
        $total += $value;
    }
    unset($value);
    return $total;
}
$values = [1, 2];
echo w04_reference_loop_phi($values), "\n";

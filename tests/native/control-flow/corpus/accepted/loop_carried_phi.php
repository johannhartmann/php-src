<?php
function w04_loop_carried_phi(int $limit): int {
    $value = 1;
    $counter = 0;
    while ($counter < $limit) {
        $value = $value + $counter;
        $counter++;
    }
    return $value;
}
echo w04_loop_carried_phi(0), ",", w04_loop_carried_phi(4), "\n";

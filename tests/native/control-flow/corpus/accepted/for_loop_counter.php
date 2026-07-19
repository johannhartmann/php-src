<?php
function w04_for_loop_counter(int $limit) {
    $total = 0;
    for ($counter = 0; $counter < $limit; $counter++) {
        $total += $counter;
    }
    return $total;
}
echo w04_for_loop_counter(0), ",", w04_for_loop_counter(5), "\n";

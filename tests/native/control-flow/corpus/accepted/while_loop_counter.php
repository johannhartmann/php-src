<?php
function w04_while_loop_counter(int $limit) {
    $counter = 0;
    while ($counter < $limit) {
        $counter++;
    }
    return $counter;
}
echo w04_while_loop_counter(0), ",", w04_while_loop_counter(4), "\n";

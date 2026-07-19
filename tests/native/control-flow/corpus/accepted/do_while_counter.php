<?php
function w04_do_while_counter(int $limit) {
    $counter = 0;
    do {
        $counter++;
    } while ($counter < $limit);
    return $counter;
}
echo w04_do_while_counter(0), ",", w04_do_while_counter(4), "\n";

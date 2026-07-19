<?php
function w04_multiple_backedges_reducible(int $limit) {
    $counter = 0;
    $total = 0;
    while ($counter < $limit) {
        $counter++;
        if (($counter & 1) === 0) {
            continue;
        }
        $total += $counter;
    }
    return $total;
}
echo w04_multiple_backedges_reducible(0), ",", w04_multiple_backedges_reducible(6), "\n";

<?php
function w04_reference_loop_phi(array &$values, bool $repeat) {
loop:
    if ($repeat) {
        goto loop;
    }
    return 0;
}
$values = [1, 2];
echo w04_reference_loop_phi($values, false), "\n";

<?php
function w04_loop_carried_phi(bool $repeat, bool $left, bool $right) {
loop:
    if ($repeat) {
        goto loop;
    }
    return $left && $right;
}
echo w04_loop_carried_phi(false, false, false), "\n";

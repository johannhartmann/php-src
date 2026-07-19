<?php
function w04_loop_carried_phi(bool $left, bool $right) {
loop:
    if ($left && $right) {
        goto loop;
    }
    return 0;
}
echo w04_loop_carried_phi(false, false), "\n";

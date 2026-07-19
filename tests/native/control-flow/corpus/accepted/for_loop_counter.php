<?php
function w04_for_loop_counter(bool $repeat) {
    for (;;) {
        if (!$repeat) {
            break;
        }
    }
    return 0;
}
echo w04_for_loop_counter(false), "\n";

<?php
function w04_while_loop_counter(bool $repeat) {
loop:
    if ($repeat) {
        goto loop;
    }
    return 0;
}
echo w04_while_loop_counter(false), "\n";

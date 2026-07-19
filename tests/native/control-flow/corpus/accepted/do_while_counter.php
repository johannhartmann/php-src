<?php
function w04_do_while_counter(bool $repeat) {
loop:
    if (!$repeat) {
        return 0;
    }
    goto loop;
}
echo w04_do_while_counter(false), "\n";

<?php
function w04_multiple_backedges_reducible(bool $left, bool $right) {
loop:
    if ($left) {
        goto loop;
    }
    if ($right) {
        goto loop;
    }
    return 0;
}
echo w04_multiple_backedges_reducible(false, false), "\n";

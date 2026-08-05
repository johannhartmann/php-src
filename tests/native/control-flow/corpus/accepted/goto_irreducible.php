<?php
function w04_goto_irreducible(bool $enterLeft, bool $loopRight, bool $repeat) {
    if ($enterLeft) {
        goto left;
    }
right:
    if ($loopRight) {
        goto left;
    }
    return 20;
left:
    if ($repeat) {
        goto right;
    }
    return 10;
}
echo w04_goto_irreducible(false, false, false), ",",
    w04_goto_irreducible(true, false, false), "\n";

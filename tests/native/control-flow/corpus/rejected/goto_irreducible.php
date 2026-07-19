<?php
function w04_goto_irreducible(int $value) {
    $steps = 0;
    if ($value > 0) {
        goto left;
    }
right:
    $steps++;
    $value--;
    if ($steps >= 4) {
        goto done;
    }
left:
    $value++;
    if ($value < 2) {
        goto right;
    }
done:
    return $value;
}
echo w04_goto_irreducible(0), ",", w04_goto_irreducible(3), "\n";

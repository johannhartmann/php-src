<?php
function w04_early_return(int $value): int {
    if ($value < 0) {
        return 0;
    }
    return $value + 1;
}
echo w04_early_return(-1), ",", w04_early_return(4), "\n";

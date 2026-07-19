<?php
function w04_early_return(bool $early, int $value) {
    if ($early) {
        return 0;
    }
    return $value;
}
echo w04_early_return(true, 4), ",", w04_early_return(false, 4), "\n";

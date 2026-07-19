<?php
function w04_unsupported_pi_constraint(int $value, int $limit) {
    if ($value >= 0 && $value < $limit) {
        return $value;
    }
    return -1;
}
echo w04_unsupported_pi_constraint(2, 4), ",", w04_unsupported_pi_constraint(5, 4), "\n";

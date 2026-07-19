<?php
function w04_if_else_int(bool $positive) {
    if ($positive) {
        return 1;
    } else {
        return -1;
    }
}
echo w04_if_else_int(false), ",", w04_if_else_int(true), "\n";

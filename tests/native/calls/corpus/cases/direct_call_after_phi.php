<?php
function w05_target(): void { echo 1; }
function w05_case(bool $repeat, bool $left, bool $right): bool {
loop:
    if ($repeat) {
        goto loop;
    }
    w05_target();
    return $left && $right;
}

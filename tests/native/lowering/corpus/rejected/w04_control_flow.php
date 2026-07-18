<?php

function w03_rejected_w04(int $value): int
{
    while ($value > 0) {
        $value--;
    }
    return $value;
}

printf("%d\n", w03_rejected_w04(3));

<?php

function w03_rejected_branches(int $value): int
{
    if ($value > 0) {
        return 1;
    }
    return 0;
}

printf("%d\n", w03_rejected_branches(3));

<?php

function w03_rejected_missing_proofs(int $left, int $right): int
{
    return $left + $right;
}

printf("%d\n", w03_rejected_missing_proofs(20, 22));

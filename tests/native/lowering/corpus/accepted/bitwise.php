<?php

function w03_bitwise(int $left, int $right): int
{
    return ($left & $right) | ($left ^ $right);
}

printf("%d\n", w03_bitwise(12, 10));

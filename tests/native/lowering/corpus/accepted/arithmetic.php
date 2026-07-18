<?php

function w03_arithmetic(int $value): int
{
    return $value % 2;
}

printf("%d\n", w03_arithmetic(17));

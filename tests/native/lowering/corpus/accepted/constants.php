<?php

function w03_constants(int $value): int
{
    return $value | 42;
}

printf("%d\n", w03_constants(16));

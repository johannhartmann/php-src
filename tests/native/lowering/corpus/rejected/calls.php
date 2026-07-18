<?php

function w03_rejected_calls(int $value): int
{
    return abs($value);
}

printf("%d\n", w03_rejected_calls(-8));

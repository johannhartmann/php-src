<?php

function w03_rejected_w06(int &$value): int
{
    $previous = $value;
    $value = 31;
    return $previous;
}

$value = 17;
printf("%d,%d\n", w03_rejected_w06($value), $value);

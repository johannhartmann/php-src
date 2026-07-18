<?php

function w03_casts(bool $value): int
{
    return (int) $value;
}

printf("%d\n", w03_casts(true));

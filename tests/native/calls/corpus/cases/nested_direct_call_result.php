<?php
function w05_nested_inner(): int
{
    echo 1;
    return 7;
}

function w05_nested_outer($value): void
{
    echo $value;
}

function w05_case(): void
{
    w05_nested_outer(w05_nested_inner());
}

<?php

function w03_comparisons(int $left, int $right): bool
{
    return $left < $right;
}

var_export(w03_comparisons(4, 9));
echo "\n";

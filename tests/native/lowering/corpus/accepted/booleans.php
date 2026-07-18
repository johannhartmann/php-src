<?php

function w03_booleans(bool $left, bool $right): bool
{
    return (!$left) xor $right;
}

var_export(w03_booleans(false, true));
echo "\n";

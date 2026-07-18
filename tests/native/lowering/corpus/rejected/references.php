<?php

function w03_rejected_references(int &$value): int
{
    $alias =& $value;
    return $alias;
}

$referenceValue = 23;
printf("%d\n", w03_rejected_references($referenceValue));

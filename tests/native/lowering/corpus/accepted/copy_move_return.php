<?php

function w03_copy_move_return(int $value): int
{
    return $value ^ 0;
}

printf("%d\n", w03_copy_move_return(17));

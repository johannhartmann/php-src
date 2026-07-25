<?php

function native_engine_file_cache_twice(int $value): int
{
    return $value * 2;
}

echo native_engine_file_cache_twice(21), "\n";

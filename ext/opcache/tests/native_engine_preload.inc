<?php

function native_engine_preload_leaf(int $value): int
{
    return $value + 1;
}

function native_engine_preload_sum(int $count): int
{
    $value = 0;
    for ($index = 0; $index < $count; $index++) {
        $value = native_engine_preload_leaf($value);
    }
    return $value;
}

function native_engine_preload_generator(): Generator
{
    yield native_engine_preload_sum(4);
    yield native_engine_preload_sum(7);
}

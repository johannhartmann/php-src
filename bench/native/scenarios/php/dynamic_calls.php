<?php

declare(strict_types=1);

function native_bench_mix(int $left, int $right): int
{
    return (($left * 31) ^ ($right * 17)) & 0x7fffffff;
}

return static function (array $config): array {
    $function = 'native_bench_mix';
    $value = 5;
    for ($index = 0; $index < $config['iterations']; $index++) {
        $value = $function($value, $index);
    }
    return [$value, is_callable($function)];
};

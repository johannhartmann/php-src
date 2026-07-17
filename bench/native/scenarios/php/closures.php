<?php

declare(strict_types=1);

return static function (array $config): array {
    $offset = 11;
    $callback = static fn (int $value): int => ($value * 3) + $offset;
    $sum = 0;
    for ($index = 0; $index < $config['iterations']; $index++) {
        $sum += $callback($index % 97);
    }
    return [$sum, $callback(123)];
};

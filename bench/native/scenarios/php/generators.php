<?php

declare(strict_types=1);

return static function (array $config): array {
    $sequence = static function (int $count): Generator {
        for ($index = 0; $index < $count; $index++) {
            yield $index => (($index * $index) + 3) % 257;
        }
    };
    $sum = 0;
    $lastKey = null;
    foreach ($sequence($config['iterations']) as $key => $value) {
        $sum += $value;
        $lastKey = $key;
    }
    return [$sum, $lastKey];
};

<?php

declare(strict_types=1);

return static function (array $config): array {
    $source = range(1, 32);
    $copy = $source;
    $reference =& $copy[0];
    $sum = 0;
    for ($index = 0; $index < $config['iterations']; $index++) {
        $reference += ($index % 3);
        $slot = $index % count($copy);
        $copy[$slot] = $copy[$slot] + 1;
        $sum += $copy[$slot];
    }
    unset($reference);
    return [$source[0], $copy[0], $sum, array_sum($copy)];
};

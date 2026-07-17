<?php

declare(strict_types=1);

return static function (array $config): array {
    $integer = 17;
    $double = 1.25;
    for ($index = 1; $index <= $config['iterations']; $index++) {
        $integer = (($integer * 33) + $index) % 1000003;
        $double = ($double * 1.00001) + (($index % 11) / 10.0);
    }
    return [$integer, sprintf('%.6f', $double)];
};

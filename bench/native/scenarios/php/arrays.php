<?php

declare(strict_types=1);

return static function (array $config): array {
    $packed = [];
    $hash = [];
    for ($index = 0; $index < $config['iterations']; $index++) {
        $packed[] = ($index * 7) % 101;
        $hash['key_' . ($index % 37)] = $index;
    }
    return [array_sum($packed), count($packed), array_sum($hash), count($hash), $hash['key_0']];
};

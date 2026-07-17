<?php

declare(strict_types=1);

return static function (array $config): array {
    $caught = 0;
    $normal = 0;
    $finalized = 0;
    for ($index = 0; $index < $config['iterations']; $index++) {
        try {
            if (($index % 7) === 0) {
                throw new RuntimeException((string) $index);
            }
            $normal += $index;
        } catch (RuntimeException $error) {
            $caught += (int) $error->getMessage();
        } finally {
            $finalized++;
        }
    }
    return [$normal, $caught, $finalized];
};

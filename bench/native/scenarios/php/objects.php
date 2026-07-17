<?php

declare(strict_types=1);

final class NativeBenchCounter
{
    public int $value = 0;

    public function add(int $amount): int
    {
        return $this->value += $amount;
    }
}

return static function (array $config): array {
    $counter = new NativeBenchCounter();
    $sum = 0;
    for ($index = 0; $index < $config['iterations']; $index++) {
        $sum += $counter->add(($index % 5) + 1);
    }
    return [$counter->value, $sum];
};

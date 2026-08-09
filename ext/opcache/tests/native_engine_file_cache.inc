<?php

function native_engine_file_cache_twice(int $value): int
{
    return $value * 2;
}

final class NativeEngineFileCacheFinalMethod
{
    public function step(int $value): int
    {
        return $value + 1;
    }

    public function fail(): never
    {
        throw new RuntimeException('final-method-exception');
    }
}

class NativeEngineFileCacheMethodBase
{
    public function value(): string
    {
        return 'base';
    }
}

class NativeEngineFileCacheMethodChild extends NativeEngineFileCacheMethodBase
{
    public function value(): string
    {
        return 'child';
    }
}

function native_engine_file_cache_final_method(int $count): int
{
    $receiver = new NativeEngineFileCacheFinalMethod();
    $result = 0;
    for ($index = 0; $index < $count; $index++) {
        $result = $receiver->step($result);
    }
    return $result;
}

function native_engine_file_cache_polymorphic_method(
    NativeEngineFileCacheMethodBase $receiver,
): string {
    return $receiver->value();
}

function native_engine_file_cache_final_method_exception(): string
{
    try {
        (new NativeEngineFileCacheFinalMethod())->fail();
    } catch (RuntimeException $exception) {
        return $exception->getMessage();
    }
}

echo native_engine_file_cache_twice(21), "\n";
echo native_engine_file_cache_final_method(42), "\n";
echo native_engine_file_cache_polymorphic_method(
    new NativeEngineFileCacheMethodChild(),
), "\n";
echo native_engine_file_cache_final_method_exception(), "\n";

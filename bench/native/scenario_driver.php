<?php

declare(strict_types=1);

function fail(string $message): never
{
    fwrite(STDERR, "scenario_driver.php: {$message}\n");
    exit(2);
}

function opcache_metrics(): ?array
{
    if (!function_exists('opcache_get_status')) {
        return null;
    }
    $status = opcache_get_status(false);
    if (!is_array($status)) {
        return null;
    }
    $memory = is_array($status['memory_usage'] ?? null) ? $status['memory_usage'] : [];
    return [
        'enabled' => (bool) ($status['opcache_enabled'] ?? false),
        'free_memory_bytes' => isset($memory['free_memory']) ? (int) $memory['free_memory'] : null,
        'used_memory_bytes' => isset($memory['used_memory']) ? (int) $memory['used_memory'] : null,
        'wasted_memory_bytes' => isset($memory['wasted_memory']) ? (int) $memory['wasted_memory'] : null,
    ];
}

if ($argc !== 4 || $argv[1] !== '--descriptor') {
    fail('usage: scenario_driver.php --descriptor <scenario.json> <steady-state-calls>');
}

$descriptorPath = realpath($argv[2]);
if ($descriptorPath === false || !is_file($descriptorPath)) {
    fail('descriptor is not a file');
}
$steadyStateCalls = filter_var($argv[3], FILTER_VALIDATE_INT, ['options' => ['min_range' => 0]]);
if ($steadyStateCalls === false) {
    fail('steady-state call count must be a non-negative integer');
}

try {
    $descriptor = json_decode((string) file_get_contents($descriptorPath), true, 32, JSON_THROW_ON_ERROR);
} catch (Throwable $error) {
    fail('invalid descriptor JSON: ' . $error->getMessage());
}

foreach (['scenario_id', 'php_file', 'warmup_calls', 'config', 'expected_checksum'] as $required) {
    if (!array_key_exists($required, $descriptor)) {
        fail("descriptor is missing {$required}");
    }
}
if (!is_string($descriptor['scenario_id']) || !is_string($descriptor['php_file'])
    || !is_int($descriptor['warmup_calls']) || $descriptor['warmup_calls'] < 0
    || !is_array($descriptor['config']) || !is_string($descriptor['expected_checksum'])) {
    fail('descriptor field types are invalid');
}

$descriptorDirectory = dirname($descriptorPath);
$scenarioPath = realpath($descriptorDirectory . DIRECTORY_SEPARATOR . $descriptor['php_file']);
$directoryPrefix = rtrim(realpath($descriptorDirectory) ?: '', DIRECTORY_SEPARATOR) . DIRECTORY_SEPARATOR;
if ($scenarioPath === false || !str_starts_with($scenarioPath, $directoryPrefix) || !is_file($scenarioPath)) {
    fail('php_file must resolve to a file below the descriptor directory');
}
$callable = require $scenarioPath;
if (!is_callable($callable)) {
    fail('scenario PHP file must return a callable');
}

$checksum = static fn (mixed $result): string => hash('sha256', serialize($result));
$verify = static function (mixed $result) use ($descriptor, $checksum): string {
    $actual = $checksum($result);
    if (!hash_equals($descriptor['expected_checksum'], $actual)) {
        fail("checksum mismatch: expected {$descriptor['expected_checksum']}, got {$actual}");
    }
    return $actual;
};

for ($index = 0; $index < $descriptor['warmup_calls']; $index++) {
    $verify($callable($descriptor['config']));
}
$opcacheBefore = opcache_metrics();
$callNs = [];
$resultChecksum = null;
for ($index = 0; $index < 10; $index++) {
    $started = hrtime(true);
    $result = $callable($descriptor['config']);
    $callNs[] = hrtime(true) - $started;
    $resultChecksum = $verify($result);
}
$steadyStateNs = [];
for ($index = 0; $index < $steadyStateCalls; $index++) {
    $started = hrtime(true);
    $result = $callable($descriptor['config']);
    $steadyStateNs[] = hrtime(true) - $started;
    $verify($result);
}
$opcacheAfter = opcache_metrics();

$output = [
    'call_ns' => $callNs,
    'compile_phase' => null,
    'compile_phase_unsupported_reason' => 'scenario does not expose a separately verifiable compilation phase',
    'opcache' => [
        'after_calls' => $opcacheAfter,
        'before_calls' => $opcacheBefore,
        'source' => $opcacheBefore === null && $opcacheAfter === null ? null : 'opcache_get_status(false)',
    ],
    'protocol_version' => 1,
    'result_checksum' => $resultChecksum,
    'scenario_id' => $descriptor['scenario_id'],
    'steady_state_ns' => $steadyStateNs,
    'warmup' => [
        'calls' => $descriptor['warmup_calls'],
        'config' => $descriptor['config'],
    ],
];

try {
    echo json_encode($output, JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR), "\n";
} catch (Throwable $error) {
    fail('could not encode result: ' . $error->getMessage());
}

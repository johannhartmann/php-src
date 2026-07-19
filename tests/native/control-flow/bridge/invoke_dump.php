<?php

declare(strict_types=1);

function bridge_error(string $message): never
{
    fwrite(STDERR, $message . "\n");
    exit(64);
}

$input = stream_get_contents(STDIN);
if ($input === false) {
    bridge_error('unable to read bridge request');
}

try {
    $request = json_decode($input, true, 16, JSON_THROW_ON_ERROR);
} catch (JsonException $exception) {
    bridge_error('invalid bridge request JSON');
}

$requestKeys = is_array($request) ? array_keys($request) : [];
sort($requestKeys);
if (!is_array($request)
    || $requestKeys !== ['filename', 'options', 'repeat', 'source_base64']
    || !is_string($request['filename'])
    || !is_array($request['options'])
    || !is_int($request['repeat'])
    || $request['repeat'] < 1
    || $request['repeat'] > 10
    || !is_string($request['source_base64'])
) {
    bridge_error('invalid bridge request shape');
}

$source = base64_decode($request['source_base64'], true);
if ($source === false) {
    bridge_error('invalid source_base64');
}
if (!function_exists('native_mir_test_compile_dump')) {
    bridge_error('native_mir_test extension is not loaded');
}

$calls = [];
for ($call = 1; $call <= $request['repeat']; $call++) {
    try {
        $calls[] = native_mir_test_compile_dump(
            $source,
            $request['filename'],
            $request['options'],
        );
    } catch (Throwable $throwable) {
        $calls[] = [
            'diagnostics' => [[
                'code' => 'BRIDGE_EXCEPTION',
                'message' => get_class($throwable) . ': ' . $throwable->getMessage(),
                'opline' => null,
                'stage' => 'bridge',
            ]],
            'mir' => null,
            'phase' => 'compile',
            'schema_version' => 1,
            'source' => [
                'byte_length' => strlen($source),
                'filename' => $request['filename'],
                'source_id' => 'fnv1a64:' . hash(
                    'fnv1a64',
                    $request['filename'] . "\0" . $source,
                ),
            ],
            'status' => 'error',
            'wave' => 4,
        ];
    }
}

echo json_encode(
    ['calls' => $calls, 'schema_version' => 1],
    JSON_THROW_ON_ERROR | JSON_UNESCAPED_SLASHES,
), "\n";

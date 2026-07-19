<?php

if (!extension_loaded('native_mir_test')) {
    fwrite(STDERR, "native_mir_test is unavailable\n");
    exit(2);
}

$request = json_decode(stream_get_contents(STDIN), true, 512, JSON_THROW_ON_ERROR);
$results = [];
foreach ($request['cases'] as $case) {
    $source = base64_decode($case['source'], true);
    if ($source === false) {
        throw new RuntimeException('invalid source encoding');
    }
    $results[] = native_mir_test_compile_dump(
        $source,
        $case['filename'],
        ['wave' => 5, 'function' => $case['function']],
    );
}
echo json_encode(['results' => $results], JSON_THROW_ON_ERROR);

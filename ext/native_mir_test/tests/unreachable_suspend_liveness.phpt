--TEST--
Native suspend liveness ignores unreachable suspension opcodes
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_dump')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$result = native_mir_test_compile_dump(
    <<<'PHP'
<?php
function native_unreachable_suspend_liveness(): Generator
{
    sin(...[0]);
    throw new RuntimeException('boom');
    yield 1;
}
PHP,
    'unreachable-suspend-liveness.php',
    [
        'function' => 'native_unreachable_suspend_liveness',
    ],
);
printf(
    "%s %s\n",
    $result['status'],
    $result['diagnostics'][0]['code'] ?? 'MIRL0000',
);
?>
--EXPECT--
accepted MIRL0000

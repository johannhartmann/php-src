--TEST--
Native direct internal calls resolve named by-value and by-reference parameters
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$source = <<<'PHP'
<?php
function w11p_named_internal_argument_modes(float $timeout): int
{
    $valueError = false;
    try {
        stream_socket_client('tcp://127.0.0.1:1', timeout: $timeout);
    } catch (ValueError) {
        $valueError = true;
    }

    $matches = null;
    $count = preg_match_all(
        pattern: '/a/',
        matches: $matches,
        subject: 'aa',
    );

    return ($valueError ? 100 : 0) + $count * 10 + count($matches[0]);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-named-internal-argument-modes.php',
    [NAN],
    [
        'wave' => 11,
        'function' => 'w11p_named_internal_argument_modes',
    ],
);
printf(
    "%s return=%d vm=%d execute_ex=%d handlers=%d\n",
    $result['status'],
    $result['execution']['return_value'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);
?>
--EXPECT--
accepted return=122 vm=0 execute_ex=0 handlers=0

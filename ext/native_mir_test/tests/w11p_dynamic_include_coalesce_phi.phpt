--TEST--
Native dynamic include preserves a coalesce result across its PHI edge
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
if (!function_exists('stream_socket_pair')) {
    die('skip stream_socket_pair() is not available');
}
?>
--FILE--
<?php
$included = __DIR__ . '/w11p_dynamic_include_coalesce_phi.inc';
$includeProgram = <<<'PHP'
<?php
$payload = "file1\r\n\nb0rk\r\n/";
[$writer, $reader] = stream_socket_pair(
    STREAM_PF_UNIX,
    STREAM_SOCK_STREAM,
    STREAM_IPPROTO_IP,
);
$command = "NLST /\r\n";
if ($command !== 'emptydir') {
    $coalesced = $payload ?? 'default';
    fwrite($writer, $coalesced);
    stream_socket_shutdown($writer, STREAM_SHUT_WR);
    return stream_get_contents($reader);
}
return 'unreachable';
PHP;
file_put_contents($included, $includeProgram);

$source = <<<'PHP'
<?php
function w11p_dynamic_include_coalesce_phi(string $included): string
{
    return include $included;
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-dynamic-include-coalesce-phi.php',
    [$included],
    ['wave' => 11, 'function' => 'w11p_dynamic_include_coalesce_phi'],
);
unlink($included);
printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value'] ?? null),
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);
?>
--CLEAN--
<?php
@unlink(__DIR__ . '/w11p_dynamic_include_coalesce_phi.inc');
?>
--EXPECT--
accepted return="file1\r\n\nb0rk\r\n\/" vm=0 execute_ex=0 handler=0

--TEST--
Native conditional boxed mutations retain independent loop PHI definitions
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
function decode_name_value_pairs(string $data): array
{
    $result = [];
    $length = strlen($data);
    $offset = 0;
    while ($offset !== $length) {
        $nameLength = ord($data[$offset++]);
        if ($nameLength >= 128) {
            $nameLength = ($nameLength & 0x7f) << 24;
            $nameLength |= ord($data[$offset++]) << 16;
            $nameLength |= ord($data[$offset++]) << 8;
            $nameLength |= ord($data[$offset++]);
        }

        $valueLength = ord($data[$offset++]);
        if ($valueLength >= 128) {
            $valueLength = ($valueLength & 0x7f) << 24;
            $valueLength |= ord($data[$offset++]) << 16;
            $valueLength |= ord($data[$offset++]) << 8;
            $valueLength |= ord($data[$offset++]);
        }

        $name = substr($data, $offset, $nameLength);
        $value = substr($data, $offset + $nameLength, $valueLength);
        $result[$name] = $value;
        $offset += $nameLength + $valueLength;
    }
    return $result;
}
PHP;

$data = chr(15) . chr(1) . 'FCGI_MPXS_CONNS' . '0';
$result = native_mir_test_compile_execute(
    $source,
    'w14-conditional-boxed-loop-phis.php',
    [$data],
    [
        'wave' => 11,
        'function' => 'decode_name_value_pairs',
    ],
);
printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
);
?>
--EXPECT--
accepted return={"FCGI_MPXS_CONNS":"0"} vm=0 execute_ex=0 handler=0

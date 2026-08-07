--TEST--
Native lazy scalar assign-op publishes values for source-bound array appends
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
function w11p_lazy_assign_op_append(array $input): array
{
    $split = 8;
    $values = [];
    $mask = (1 << $split) - 1;
    $index = $overflow = 0;
    $length = count($input);
    $input[] = 0;
    $remaining = 31;

    while ($index != $length) {
        $digit = $input[$index] & $mask;
        $input[$index] >>= $split;
        if (!$overflow) {
            $remaining -= $split;
            $overflow = $split <= $remaining ? 0 : $split - $remaining;
            if (!$remaining) {
                $index++;
                $remaining = 31;
                $overflow = 0;
            }
        } elseif (++$index != $length) {
            $temporaryMask = (1 << $overflow) - 1;
            $digit |= ($input[$index] & $temporaryMask) << $remaining;
            $input[$index] >>= $overflow;
            $remaining = 31 - $overflow;
            $overflow = $split <= $remaining ? 0 : $split - $remaining;
        }
        $values[] = $digit;
    }
    while ($values[count($values) - 1] == 0) {
        unset($values[count($values) - 1]);
    }
    return array_reverse($values);
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w11p-lazy-assign-op-append.php',
    [[2147483647, 2147483647, 2147483647, 3, 0, 0, 32, 2147483584, 127]],
    [
        'wave' => 11,
        'function' => 'w11p_lazy_assign_op_append',
        'repeat' => 2,
    ],
);

printf(
    "%s return=%s runs=%d vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['executions'],
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=[127,255,255,255,128,0,0,0,128,0,0,0,0,0,0,0,0,0,0,0,127,255,255,255,255,255,255,255,255,255,255,255] runs=2 vm=0 execute_ex=0 handler=0 active=0

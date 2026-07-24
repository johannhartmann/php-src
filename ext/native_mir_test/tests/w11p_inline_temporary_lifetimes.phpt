--TEST--
Native baseline copies and releases temporary values inline
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
class NativeLifetimeDestructor
{
    public function __destruct()
    {
        echo 7;
    }
}

function temporary_string_key()
{
    return "key";
}

function inline_temporary_lifetimes($left, $right, $index)
{
    $left[temporary_string_key()] ??= 1;
    $right[$index++] ??= 2;
    new NativeLifetimeDestructor();
    return [$left, $right, $index];
}
PHP;

$dump = native_mir_test_compile_dump(
    $source,
    'w11p-inline-temporary-lifetimes-dump.php',
    [
        'wave' => 10,
        'function' => 'inline_temporary_lifetimes',
    ],
);
$opcodes = array_count_values($dump['source_opcodes']);
printf(
    "copy_tmp=%d free=%d\n",
    $opcodes['ZEND_COPY_TMP'] ?? 0,
    $opcodes['ZEND_FREE'] ?? 0,
);

$result = native_mir_test_compile_execute(
    $source,
    'w11p-inline-temporary-lifetimes.php',
    [[], ['old'], 0],
    [
        'wave' => 11,
        'function' => 'inline_temporary_lifetimes',
        'repeat' => 10,
    ],
);
printf(
    "%s return=%s vm=%d execute_ex=%d handler=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    $result['execution']['vm_handler_calls'],
    $result['execution']['execute_ex_calls'],
    $result['execution']['opline_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
copy_tmp=2 free=5
7777777777accepted return=[{"key":1},["old"],1] vm=0 execute_ex=0 handler=0 active=0

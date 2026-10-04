--TEST--
Native writable static property fetches preserve typed reference sources
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$result = native_mir_test_compile_execute(
    <<<'PHP'
<?php
class NativeStaticTypedReference
{
    public static ?stdClass $first;
    public static ?stdClass $second;
}

function native_static_typed_reference_sources(): array
{
    NativeStaticTypedReference::$first = new stdClass;
    NativeStaticTypedReference::$second = &NativeStaticTypedReference::$first;
    $replacement = new stdClass;
    NativeStaticTypedReference::$first = &$replacement;

    $result = [
        NativeStaticTypedReference::$first === $replacement,
        NativeStaticTypedReference::$second instanceof stdClass,
        NativeStaticTypedReference::$first === NativeStaticTypedReference::$second,
    ];
    NativeStaticTypedReference::$first = null;
    NativeStaticTypedReference::$second = null;

    return $result;
}
PHP,
    'static-typed-reference-sources.php',
    [],
    [
        'function' => 'native_static_typed_reference_sources',
        'repeat' => 20,
    ],
);

printf(
    "%s return=%s closure=%s vm=%d active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    ($result['execution']['failed_codeunits'] ?? -1) === 0
        && ($result['execution']['performance']['ready_codeunits'] ?? -1)
            === ($result['execution']['performance']['compiled_codeunits'] ?? -2)
        ? 'ready'
        : 'incomplete',
    $result['execution']['vm_handler_calls'],
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=[true,true,false] closure=ready vm=0 active=0

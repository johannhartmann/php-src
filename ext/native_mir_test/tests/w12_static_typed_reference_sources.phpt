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
class W12StaticTypedReference
{
    public static ?stdClass $first;
    public static ?stdClass $second;
}

function w12_static_typed_reference_sources(): array
{
    W12StaticTypedReference::$first = new stdClass;
    W12StaticTypedReference::$second = &W12StaticTypedReference::$first;
    $replacement = new stdClass;
    W12StaticTypedReference::$first = &$replacement;

    $result = [
        W12StaticTypedReference::$first === $replacement,
        W12StaticTypedReference::$second instanceof stdClass,
        W12StaticTypedReference::$first === W12StaticTypedReference::$second,
    ];
    W12StaticTypedReference::$first = null;
    W12StaticTypedReference::$second = null;

    return $result;
}
PHP,
    'w12-static-typed-reference-sources.php',
    [],
    [
        'wave' => 11,
        'function' => 'w12_static_typed_reference_sources',
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

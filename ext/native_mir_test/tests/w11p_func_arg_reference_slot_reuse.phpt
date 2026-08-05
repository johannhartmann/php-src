--TEST--
Native dynamic internal calls consume reference-valued FUNC_ARG slots
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
final class W11PFuncArgReferenceSource
{
    private string $value = 'native';

    public function &__get(string $name): mixed
    {
        return $this->value;
    }
}

function w11p_func_arg_reference_slot_reuse(): array
{
    $source = new W11PFuncArgReferenceSource();
    $callable = 'strlen';
    $length = $callable($source->missing);
    $upper = strtoupper($source->missing);

    return [$length, $upper, $source->missing];
}
PHP,
    'w11p-func-arg-reference-slot-reuse.php',
    [],
    [
        'wave' => 11,
        'function' => 'w11p_func_arg_reference_slot_reuse',
        'repeat' => 20,
    ],
);

printf(
    "%s return=%s closure=%s active=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value']),
    ($result['execution']['failed_codeunits'] ?? -1) === 0
        && ($result['execution']['performance']['ready_codeunits'] ?? -1)
            === ($result['execution']['performance']['compiled_codeunits'] ?? -2)
        ? 'ready'
        : 'incomplete',
    $result['execution']['entry_active_calls'],
);
?>
--EXPECT--
accepted return=[6,"NATIVE","native"] closure=ready active=0

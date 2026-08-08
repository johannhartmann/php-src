--TEST--
Native repeated internal constructors receive boxed property arguments
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
class W14RepeatedConstructorNode extends RecursiveArrayIterator
{
    protected array $children = [];

    public function getChildren(): RecursiveArrayIterator
    {
        return new RecursiveArrayIterator($this->children);
    }
}

function w14_repeated_constructor_root(): array
{
    $node = new W14RepeatedConstructorNode([]);

    return [
        $node->getChildren() instanceof RecursiveArrayIterator,
        $node->getChildren() instanceof RecursiveArrayIterator,
    ];
}
PHP;

$result = native_mir_test_compile_execute(
    $source,
    'w14-repeated-constructor-array-argument.php',
    [],
    [
        'wave' => 11,
        'function' => 'w14_repeated_constructor_root',
        'repeat' => 20,
    ],
);
$execution = $result['execution'];
printf(
    "%s return=%s runs=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($execution['return_value']),
    $execution['executions'],
    $execution['vm_handler_calls'],
    $execution['execute_ex_calls'],
    $execution['opline_handler_calls'],
);
?>
--EXPECT--
accepted return=[true,true] runs=20 vm=0 execute_ex=0 handler=0

--TEST--
Native boolean PHI negation preserves canonical branch conditions
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
function boolean_phi_unary_branch(string $argument, bool $assigned): string
{
    $isSwitch = false;
    switch ($argument) {
        case '--switch':
            $isSwitch = true;
            break;
    }

    if ($assigned) {
        $notSwitch = !$isSwitch;
        if ($notSwitch) {
            return 'assigned-miss';
        }
        return 'assigned-hit';
    }

    if (!$isSwitch) {
        return 'direct-miss';
    }
    return 'direct-hit';
}
PHP;

$cases = [
    ['argument', false],
    ['--switch', false],
    ['argument', true],
    ['--switch', true],
];
foreach ($cases as $index => $arguments) {
    $result = native_mir_test_compile_execute(
        $source,
        "w11p-boolean-phi-unary-branch-$index.php",
        $arguments,
        [
            'wave' => 11,
            'function' => 'boolean_phi_unary_branch',
            'repeat' => 20,
        ],
    );
    printf(
        "%s return=%s vm=%d execute_ex=%d handler=%d active=%d\n",
        $result['status'],
        $result['execution']['return_value'],
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
        $result['execution']['entry_active_calls'],
    );
}
?>
--EXPECT--
accepted return=direct-miss vm=0 execute_ex=0 handler=0 active=0
accepted return=direct-hit vm=0 execute_ex=0 handler=0 active=0
accepted return=assigned-miss vm=0 execute_ex=0 handler=0 active=0
accepted return=assigned-hit vm=0 execute_ex=0 handler=0 active=0

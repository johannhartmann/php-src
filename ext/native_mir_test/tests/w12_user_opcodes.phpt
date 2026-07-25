--TEST--
Native user opcode callbacks select code-image control flow without VM dispatch
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
function w12_user_opcode(int $value)
{
    $value += 2;
    $value += 3;
    return $value;
}
PHP;

$cases = [
    ['continue', null, 1, 2],
    ['dispatch', null, 6, 2],
    ['dispatch_to', 'ZEND_ASSIGN_OP', 6, 2],
    ['return', null, null, 1],
    ['leave', null, null, 1],
];
foreach ($cases as [$action, $dispatchTo, $expected, $calls]) {
    $userOpcode = [
        'opcode' => 'ZEND_ASSIGN_OP',
        'action' => $action,
    ];
    if ($dispatchTo !== null) {
        $userOpcode['dispatch_to'] = $dispatchTo;
    }
    $result = native_mir_test_compile_execute(
        $source,
        "w12-user-opcode-$action.php",
        [1],
        [
            'wave' => 11,
            'function' => 'w12_user_opcode',
            'user_opcode' => $userOpcode,
        ],
    );
    printf(
        "%s status=%s result=%s calls=%d/%d vm=%d execute_ex=%d handler=%d\n",
        $action,
        $result['status'],
        json_encode($result['execution']['return_value'] ?? null),
        $result['execution']['user_opcode_calls'] ?? -1,
        $calls,
        $result['execution']['vm_handler_calls'] ?? -1,
        $result['execution']['execute_ex_calls'] ?? -1,
        $result['execution']['opline_handler_calls'] ?? -1,
    );
    if (($result['execution']['return_value'] ?? null) !== $expected) {
        printf("diagnostics=%s\n", json_encode($result['diagnostics'] ?? null));
    }
}

foreach ([
    ['dispatch', 4],
    ['continue', 1],
] as [$action, $expected]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w12-user-opcode-moved-$action.php",
        [1],
        [
            'wave' => 11,
            'function' => 'w12_user_opcode',
            'user_opcode' => [
                'opcode' => 'ZEND_ASSIGN_OP',
                'action' => $action,
                'advance' => 1,
            ],
        ],
    );
    printf(
        "moved_%s status=%s result=%s calls=%d vm=%d execute_ex=%d handler=%d\n",
        $action,
        $result['status'],
        json_encode($result['execution']['return_value'] ?? null),
        $result['execution']['user_opcode_calls'] ?? -1,
        $result['execution']['vm_handler_calls'] ?? -1,
        $result['execution']['execute_ex_calls'] ?? -1,
        $result['execution']['opline_handler_calls'] ?? -1,
    );
    if (($result['execution']['return_value'] ?? null) !== $expected) {
        printf("diagnostics=%s\n", json_encode($result['diagnostics'] ?? null));
    }
}
?>
--EXPECT--
continue status=accepted result=1 calls=2/2 vm=0 execute_ex=0 handler=0
dispatch status=accepted result=6 calls=2/2 vm=0 execute_ex=0 handler=0
dispatch_to status=accepted result=6 calls=2/2 vm=0 execute_ex=0 handler=0
return status=accepted result=null calls=1/1 vm=0 execute_ex=0 handler=0
leave status=accepted result=null calls=1/1 vm=0 execute_ex=0 handler=0
moved_dispatch status=accepted result=4 calls=1 vm=0 execute_ex=0 handler=0
moved_continue status=accepted result=1 calls=1 vm=0 execute_ex=0 handler=0

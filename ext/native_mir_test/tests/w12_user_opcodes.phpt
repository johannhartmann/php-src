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
function w12_user_opcode_binary(int $value)
{
    return $value + 9;
}
function w12_user_opcode_unary($value)
{
    return $value + 9;
}
function w12_user_opcode_incdec(int $value)
{
    return $value++;
}
function w12_user_opcode_array($key, $array)
{
    return $key + $array;
}
function w12_user_opcode_control_source($left, $right)
{
    return $left && $right;
}
function w12_user_opcode_enter(): array
{
    $GLOBALS['w12_user_opcode_enter_trace'][] = 'entered';
    $value = 1;
    $value += 2;
    return [$GLOBALS['w12_user_opcode_enter_trace'], $value];
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

$GLOBALS['w12_user_opcode_enter_trace'] = [];
$result = native_mir_test_compile_execute(
    $source,
    'w12-user-opcode-enter.php',
    [],
    [
        'wave' => 11,
        'function' => 'w12_user_opcode_enter',
        'user_opcode' => [
            'opcode' => 'ZEND_ASSIGN_OP',
            'action' => 'enter',
        ],
    ],
);
printf(
    "enter status=%s result=%s calls=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value'] ?? null),
    $result['execution']['user_opcode_calls'] ?? -1,
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);

$binaryTargets = [
    'ZEND_ADD' => 10,
    'ZEND_SUB' => -8,
    'ZEND_MUL' => 9,
    'ZEND_DIV' => 1 / 9,
    'ZEND_MOD' => 1,
    'ZEND_POW' => 1,
    'ZEND_SL' => 512,
    'ZEND_SR' => 0,
    'ZEND_BW_OR' => 9,
    'ZEND_BW_AND' => 1,
    'ZEND_BW_XOR' => 8,
    'ZEND_BOOL_XOR' => false,
    'ZEND_IS_IDENTICAL' => false,
    'ZEND_IS_NOT_IDENTICAL' => true,
    'ZEND_IS_EQUAL' => false,
    'ZEND_IS_NOT_EQUAL' => true,
    'ZEND_IS_SMALLER' => true,
    'ZEND_IS_SMALLER_OR_EQUAL' => true,
    'ZEND_SPACESHIP' => -1,
    'ZEND_CONCAT' => '19',
    'ZEND_FAST_CONCAT' => '19',
];
foreach ($binaryTargets as $target => $expected) {
    $result = native_mir_test_compile_execute(
        $source,
        "w12-user-opcode-binary-$target.php",
        [1],
        [
            'wave' => 11,
            'function' => 'w12_user_opcode_binary',
            'user_opcode' => [
                'opcode' => 'ZEND_ADD',
                'action' => 'dispatch_to',
                'dispatch_to' => $target,
            ],
        ],
    );
    printf(
        "binary_%s status=%s result=%s calls=%d vm=%d execute_ex=%d handler=%d\n",
        $target,
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

$result = native_mir_test_compile_execute(
    $source,
    'w12-user-opcode-array-key-exists.php',
    ['native', ['native' => 1]],
    [
        'wave' => 11,
        'function' => 'w12_user_opcode_array',
        'user_opcode' => [
            'opcode' => 'ZEND_ADD',
            'action' => 'dispatch_to',
            'dispatch_to' => 'ZEND_ARRAY_KEY_EXISTS',
        ],
    ],
);
printf(
    "array_key_exists status=%s result=%s calls=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value'] ?? null),
    $result['execution']['user_opcode_calls'] ?? -1,
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);
if (($result['execution']['return_value'] ?? null) !== true) {
    printf("diagnostics=%s\n", json_encode($result['diagnostics'] ?? null));
}

$result = native_mir_test_compile_execute(
    $source,
    'w12-user-opcode-control-source.php',
    [true, false],
    [
        'wave' => 11,
        'function' => 'w12_user_opcode_control_source',
        'user_opcode' => [
            'opcode' => 'ZEND_JMPZ_EX',
            'action' => 'dispatch_to',
            'dispatch_to' => 'ZEND_BOOL_NOT',
        ],
    ],
);
printf(
    "control_source status=%s result=%s calls=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value'] ?? null),
    $result['execution']['user_opcode_calls'] ?? -1,
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);

$unaryTargets = [
    'ZEND_BW_NOT' => [1, -2],
    'ZEND_BOOL_NOT' => [1, false],
    'ZEND_BOOL' => [1, true],
    'ZEND_STRLEN' => ['123', 3],
];
foreach ($unaryTargets as $target => [$input, $expected]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w12-user-opcode-unary-$target.php",
        [$input],
        [
            'wave' => 11,
            'function' => 'w12_user_opcode_unary',
            'user_opcode' => [
                'opcode' => 'ZEND_ADD',
                'action' => 'dispatch_to',
                'dispatch_to' => $target,
            ],
        ],
    );
    printf(
        "unary_%s status=%s result=%s calls=%d vm=%d execute_ex=%d handler=%d\n",
        $target,
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

$incdecTargets = [
    'ZEND_PRE_INC' => 5,
    'ZEND_PRE_DEC' => 3,
    'ZEND_POST_INC' => 4,
    'ZEND_POST_DEC' => 4,
];
foreach ($incdecTargets as $target => $expected) {
    $result = native_mir_test_compile_execute(
        $source,
        "w12-user-opcode-incdec-$target.php",
        [4],
        [
            'wave' => 11,
            'function' => 'w12_user_opcode_incdec',
            'user_opcode' => [
                'opcode' => 'ZEND_POST_INC',
                'action' => 'dispatch_to',
                'dispatch_to' => $target,
            ],
        ],
    );
    printf(
        "incdec_%s status=%s result=%s calls=%d vm=%d execute_ex=%d handler=%d\n",
        $target,
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

$generatorSource = <<<'PHP'
<?php
function w12_user_opcode_generator(): Generator
{
    $value = 1;
    $value += 2;
    yield $value;
    return 9;
}
function w12_user_opcode_generator_root(): array
{
    $generator = w12_user_opcode_generator();
    $valid = $generator->valid();
    try {
        $generator->getReturn();
        $return = 'unexpected';
    } catch (Throwable $exception) {
        $return = $exception->getMessage();
    }
    return [$valid, $return];
}
PHP;
$result = native_mir_test_compile_execute(
    $generatorSource,
    'w12-user-opcode-generator-return.php',
    [],
    [
        'wave' => 11,
        'function' => 'w12_user_opcode_generator_root',
        'user_opcode' => [
            'opcode' => 'ZEND_ASSIGN_OP',
            'action' => 'return',
        ],
    ],
);
printf(
    "generator_return status=%s result=%s calls=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value'] ?? null),
    $result['execution']['user_opcode_calls'] ?? -1,
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);
?>
--EXPECT--
continue status=accepted result=1 calls=2/2 vm=0 execute_ex=0 handler=0
dispatch status=accepted result=6 calls=2/2 vm=0 execute_ex=0 handler=0
dispatch_to status=accepted result=6 calls=2/2 vm=0 execute_ex=0 handler=0
return status=accepted result=null calls=1/1 vm=0 execute_ex=0 handler=0
leave status=accepted result=null calls=1/1 vm=0 execute_ex=0 handler=0
enter status=accepted result=[["entered","entered"],1] calls=2 vm=0 execute_ex=0 handler=0
binary_ZEND_ADD status=accepted result=10 calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_SUB status=accepted result=-8 calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_MUL status=accepted result=9 calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_DIV status=accepted result=0.1111111111111111 calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_MOD status=accepted result=1 calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_POW status=accepted result=1 calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_SL status=accepted result=512 calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_SR status=accepted result=0 calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_BW_OR status=accepted result=9 calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_BW_AND status=accepted result=1 calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_BW_XOR status=accepted result=8 calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_BOOL_XOR status=accepted result=false calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_IS_IDENTICAL status=accepted result=false calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_IS_NOT_IDENTICAL status=accepted result=true calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_IS_EQUAL status=accepted result=false calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_IS_NOT_EQUAL status=accepted result=true calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_IS_SMALLER status=accepted result=true calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_IS_SMALLER_OR_EQUAL status=accepted result=true calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_SPACESHIP status=accepted result=-1 calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_CONCAT status=accepted result="19" calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_FAST_CONCAT status=accepted result="19" calls=1 vm=0 execute_ex=0 handler=0
array_key_exists status=accepted result=true calls=1 vm=0 execute_ex=0 handler=0
control_source status=accepted result=false calls=1 vm=0 execute_ex=0 handler=0
unary_ZEND_BW_NOT status=accepted result=-2 calls=1 vm=0 execute_ex=0 handler=0
unary_ZEND_BOOL_NOT status=accepted result=false calls=1 vm=0 execute_ex=0 handler=0
unary_ZEND_BOOL status=accepted result=true calls=1 vm=0 execute_ex=0 handler=0
unary_ZEND_STRLEN status=accepted result=3 calls=1 vm=0 execute_ex=0 handler=0
incdec_ZEND_PRE_INC status=accepted result=5 calls=1 vm=0 execute_ex=0 handler=0
incdec_ZEND_PRE_DEC status=accepted result=3 calls=1 vm=0 execute_ex=0 handler=0
incdec_ZEND_POST_INC status=accepted result=4 calls=1 vm=0 execute_ex=0 handler=0
incdec_ZEND_POST_DEC status=accepted result=4 calls=1 vm=0 execute_ex=0 handler=0
moved_dispatch status=accepted result=4 calls=1 vm=0 execute_ex=0 handler=0
moved_continue status=accepted result=1 calls=1 vm=0 execute_ex=0 handler=0
generator_return status=accepted result=[false,"Cannot get return value of a generator that hasn't returned"] calls=1 vm=0 execute_ex=0 handler=0

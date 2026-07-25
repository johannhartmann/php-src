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
function w12_user_opcode_single($value)
{
    return !$value;
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
function w12_user_opcode_branch_target($left, $right)
{
    return $left && $right;
}
function w12_user_opcode_return_target($value)
{
    return +$value;
}
function w12_user_opcode_throw_target(Throwable $exception)
{
    return !$exception;
}
function w12_user_opcode_switch_target($value)
{
    switch ($value) {
        case -4: return 'negative';
        case 0: return 'zero';
        case 1: return 'one';
        case 2: return 'two';
        case 3: return 'three';
        case 4: return 'four';
        case 5: return 'five';
        case 19: return 'nineteen';
        default: return 'other';
    }
}
function w12_user_opcode_match_target($value)
{
    return match ($value) {
        1 => 'one',
        2 => 'two',
        'three' => 'string-three',
        default => 'other',
    };
}
function w12_user_opcode_enter(): array
{
    $GLOBALS['w12_user_opcode_enter_trace'][] = 'entered';
    $value = 1;
    $value += 2;
    return [$GLOBALS['w12_user_opcode_enter_trace'], $value];
}
function w12_user_opcode_receive(int $value = 3, ...$rest): array
{
    return [$value, $rest];
}
function w12_user_opcode_catch(): string
{
    try {
        throw new Exception('native-catch');
    } catch (RuntimeException $exception) {
        return 'wrong';
    } catch (Exception $exception) {
        return $exception->getMessage();
    }
}
function w12_user_opcode_user_callee(int $value): int
{
    return $value + 4;
}
function w12_user_opcode_user_call(int $value): int
{
    return w12_user_opcode_user_callee($value);
}
function w12_user_opcode_internal_call(string $value): int
{
    return strcmp($value, 'native');
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

$result = native_mir_test_compile_execute(
    $source,
    'w12-user-opcode-nop.php',
    [1],
    [
        'wave' => 11,
        'function' => 'w12_user_opcode',
        'user_opcode' => [
            'opcode' => 'ZEND_ASSIGN_OP',
            'action' => 'dispatch_to',
            'dispatch_to' => 'ZEND_NOP',
        ],
    ],
);
printf(
    "dispatch_to_nop status=%s result=%s calls=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value'] ?? null),
    $result['execution']['user_opcode_calls'] ?? -1,
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);

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

foreach ([
    ['ZEND_RECV_INIT', [], [3, []]],
    ['ZEND_RECV_VARIADIC', [7, 8, 9], [7, [8, 9]]],
] as [$target, $arguments, $expected]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w12-user-opcode-$target.php",
        $arguments,
        [
            'wave' => 11,
            'function' => 'w12_user_opcode_receive',
            'user_opcode' => [
                'opcode' => $target,
                'action' => 'dispatch_to',
                'dispatch_to' => $target,
            ],
        ],
    );
    printf(
        "receive_%s status=%s result=%s calls=%d vm=%d execute_ex=%d handler=%d\n",
        $target,
        $result['status'],
        json_encode($result['execution']['return_value'] ?? null),
        $result['execution']['user_opcode_calls'] ?? -1,
        $result['execution']['vm_handler_calls'] ?? -1,
        $result['execution']['execute_ex_calls'] ?? -1,
        $result['execution']['opline_handler_calls'] ?? -1,
    );
}

$result = native_mir_test_compile_execute(
    $source,
    'w12-user-opcode-catch.php',
    [],
    [
        'wave' => 11,
        'function' => 'w12_user_opcode_catch',
        'user_opcode' => [
            'opcode' => 'ZEND_CATCH',
            'action' => 'dispatch_to',
            'dispatch_to' => 'ZEND_CATCH',
        ],
    ],
);
printf(
    "catch status=%s result=%s calls=%d vm=%d execute_ex=%d handler=%d\n",
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
    'ZEND_CASE' => false,
    'ZEND_CASE_STRICT' => false,
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

$singleTargets = [
    'ZEND_COUNT' => [[1, 2, 3], 3],
    'ZEND_GET_TYPE' => [['native'], 'array'],
];
foreach ($singleTargets as $target => [$input, $expected]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w12-user-opcode-single-$target.php",
        [$input],
        [
            'wave' => 11,
            'function' => 'w12_user_opcode_single',
            'user_opcode' => [
                'opcode' => 'ZEND_BOOL_NOT',
                'action' => 'dispatch_to',
                'dispatch_to' => $target,
            ],
        ],
    );
    printf(
        "single_%s status=%s result=%s calls=%d vm=%d execute_ex=%d handler=%d\n",
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
    'w12-user-opcode-in-array.php',
    ['native', ['native' => true]],
    [
        'wave' => 11,
        'function' => 'w12_user_opcode_array',
        'user_opcode' => [
            'opcode' => 'ZEND_ADD',
            'action' => 'dispatch_to',
            'dispatch_to' => 'ZEND_IN_ARRAY',
        ],
    ],
);
printf(
    "in_array status=%s result=%s calls=%d vm=%d execute_ex=%d handler=%d\n",
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

$result = native_mir_test_compile_execute(
    $source,
    'w12-user-opcode-branch-target.php',
    [true, false],
    [
        'wave' => 11,
        'function' => 'w12_user_opcode_branch_target',
        'user_opcode' => [
            'opcode' => 'ZEND_JMPZ_EX',
            'action' => 'dispatch_to',
            'dispatch_to' => 'ZEND_JMPNZ_EX',
        ],
    ],
);
printf(
    "branch_target status=%s result=%s calls=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value'] ?? null),
    $result['execution']['user_opcode_calls'] ?? -1,
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);

$result = native_mir_test_compile_execute(
    $source,
    'w12-user-opcode-return-target.php',
    [37],
    [
        'wave' => 11,
        'function' => 'w12_user_opcode_return_target',
        'user_opcode' => [
            'opcode' => 'ZEND_MUL',
            'action' => 'dispatch_to',
            'dispatch_to' => 'ZEND_RETURN',
        ],
    ],
);
printf(
    "return_target status=%s result=%s calls=%d vm=%d execute_ex=%d handler=%d\n",
    $result['status'],
    json_encode($result['execution']['return_value'] ?? null),
    $result['execution']['user_opcode_calls'] ?? -1,
    $result['execution']['vm_handler_calls'] ?? -1,
    $result['execution']['execute_ex_calls'] ?? -1,
    $result['execution']['opline_handler_calls'] ?? -1,
);

try {
    native_mir_test_compile_execute(
        $source,
        'w12-user-opcode-throw-target.php',
        [new Exception('native-target')],
        [
            'wave' => 11,
            'function' => 'w12_user_opcode_throw_target',
            'user_opcode' => [
                'opcode' => 'ZEND_BOOL_NOT',
                'action' => 'dispatch_to',
                'dispatch_to' => 'ZEND_THROW',
            ],
        ],
    );
} catch (Throwable $exception) {
    printf("throw_target exception=%s\n", $exception->getMessage());
}

$multiwayTargets = [
    ['w12_user_opcode_switch_target', 'ZEND_SWITCH_LONG', 'ZEND_MATCH', 2, 'two'],
    ['w12_user_opcode_match_target', 'ZEND_MATCH', 'ZEND_SWITCH_LONG', 2, 'two'],
    ['w12_user_opcode_match_target', 'ZEND_MATCH', 'ZEND_SWITCH_STRING', 'three', 'string-three'],
];
foreach ($multiwayTargets as [$function, $sourceOpcode, $target, $input, $expected]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w12-user-opcode-multiway-$target.php",
        [$input],
        [
            'wave' => 11,
            'function' => $function,
            'user_opcode' => [
                'opcode' => $sourceOpcode,
                'action' => 'dispatch_to',
                'dispatch_to' => $target,
            ],
        ],
    );
    printf(
        "multiway_%s status=%s result=%s calls=%d vm=%d execute_ex=%d handler=%d\n",
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

$callCases = [
    [
        'user',
        'w12_user_opcode_user_call',
        [5],
        9,
        ['ZEND_INIT_FCALL', 'ZEND_SEND_VAR', 'ZEND_DO_UCALL'],
    ],
    [
        'internal',
        'w12_user_opcode_internal_call',
        ['native'],
        0,
        ['ZEND_INIT_FCALL', 'ZEND_SEND_VAR', 'ZEND_DO_ICALL'],
    ],
];
foreach ($callCases as [$kind, $function, $arguments, $expected, $opcodes]) {
    foreach ($opcodes as $opcode) {
        foreach (['dispatch', 'dispatch_to'] as $action) {
            $userOpcode = [
                'opcode' => $opcode,
                'action' => $action,
            ];
            if ($action === 'dispatch_to') {
                $userOpcode['dispatch_to'] = $opcode;
            }
            $result = native_mir_test_compile_execute(
                $source,
                "w12-user-opcode-call-$kind-$opcode-$action.php",
                $arguments,
                [
                    'wave' => 11,
                    'function' => $function,
                    'user_opcode' => $userOpcode,
                ],
            );
            printf(
                "call_%s_%s_%s status=%s result=%s calls=%d vm=%d execute_ex=%d handler=%d\n",
                $kind,
                $opcode,
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
dispatch_to_nop status=accepted result=1 calls=2 vm=0 execute_ex=0 handler=0
enter status=accepted result=[["entered","entered"],1] calls=2 vm=0 execute_ex=0 handler=0
receive_ZEND_RECV_INIT status=accepted result=[3,[]] calls=1 vm=0 execute_ex=0 handler=0
receive_ZEND_RECV_VARIADIC status=accepted result=[7,[8,9]] calls=1 vm=0 execute_ex=0 handler=0
catch status=accepted result="native-catch" calls=2 vm=0 execute_ex=0 handler=0
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
binary_ZEND_CASE status=accepted result=false calls=1 vm=0 execute_ex=0 handler=0
binary_ZEND_CASE_STRICT status=accepted result=false calls=1 vm=0 execute_ex=0 handler=0
array_key_exists status=accepted result=true calls=1 vm=0 execute_ex=0 handler=0
single_ZEND_COUNT status=accepted result=3 calls=1 vm=0 execute_ex=0 handler=0
single_ZEND_GET_TYPE status=accepted result="array" calls=1 vm=0 execute_ex=0 handler=0
in_array status=accepted result=true calls=1 vm=0 execute_ex=0 handler=0
control_source status=accepted result=false calls=1 vm=0 execute_ex=0 handler=0
branch_target status=accepted result=true calls=1 vm=0 execute_ex=0 handler=0
return_target status=accepted result=37 calls=1 vm=0 execute_ex=0 handler=0
throw_target exception=native-target
multiway_ZEND_MATCH status=accepted result="two" calls=1 vm=0 execute_ex=0 handler=0
multiway_ZEND_SWITCH_LONG status=accepted result="two" calls=1 vm=0 execute_ex=0 handler=0
multiway_ZEND_SWITCH_STRING status=accepted result="string-three" calls=1 vm=0 execute_ex=0 handler=0
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
call_user_ZEND_INIT_FCALL_dispatch status=accepted result=9 calls=1 vm=0 execute_ex=0 handler=0
call_user_ZEND_INIT_FCALL_dispatch_to status=accepted result=9 calls=1 vm=0 execute_ex=0 handler=0
call_user_ZEND_SEND_VAR_dispatch status=accepted result=9 calls=1 vm=0 execute_ex=0 handler=0
call_user_ZEND_SEND_VAR_dispatch_to status=accepted result=9 calls=1 vm=0 execute_ex=0 handler=0
call_user_ZEND_DO_UCALL_dispatch status=accepted result=9 calls=1 vm=0 execute_ex=0 handler=0
call_user_ZEND_DO_UCALL_dispatch_to status=accepted result=9 calls=1 vm=0 execute_ex=0 handler=0
call_internal_ZEND_INIT_FCALL_dispatch status=accepted result=0 calls=1 vm=0 execute_ex=0 handler=0
call_internal_ZEND_INIT_FCALL_dispatch_to status=accepted result=0 calls=1 vm=0 execute_ex=0 handler=0
call_internal_ZEND_SEND_VAR_dispatch status=accepted result=0 calls=1 vm=0 execute_ex=0 handler=0
call_internal_ZEND_SEND_VAR_dispatch_to status=accepted result=0 calls=1 vm=0 execute_ex=0 handler=0
call_internal_ZEND_DO_ICALL_dispatch status=accepted result=0 calls=1 vm=0 execute_ex=0 handler=0
call_internal_ZEND_DO_ICALL_dispatch_to status=accepted result=0 calls=1 vm=0 execute_ex=0 handler=0
generator_return status=accepted result=[false,"Cannot get return value of a generator that hasn't returned"] calls=1 vm=0 execute_ex=0 handler=0

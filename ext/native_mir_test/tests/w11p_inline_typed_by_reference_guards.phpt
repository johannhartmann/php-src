--TEST--
Native inline typed reference frames guard referent types
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$cases = [
    'bool-fast' => [
        <<<'PHP'
<?php
function w11p_bool_reference_leaf(bool &$value): int
{
    return $value ? 1 : 0;
}

function w11p_bool_reference_root(): int
{
    $false = false;
    $falseAlias =& $false;
    $true = true;
    $trueAlias =& $true;
    return 10 * w11p_bool_reference_leaf($false)
        + w11p_bool_reference_leaf($true);
}
PHP,
        'w11p_bool_reference_root',
        1,
    ],
    'weak-coercion' => [
        <<<'PHP'
<?php
function w11p_weak_reference_leaf(int &$value): int
{
    return $value;
}

function w11p_weak_reference_root(): int
{
    $value = '41';
    $alias =& $value;
    $result = w11p_weak_reference_leaf($value);
    return ($value === 41 ? 100 : 0) + $result;
}
PHP,
        'w11p_weak_reference_root',
        141,
    ],
    'strict-mismatch' => [
        <<<'PHP'
<?php declare(strict_types=1);
function w11p_strict_reference_leaf(int &$value): int
{
    return $value;
}

function w11p_strict_reference_root(): int
{
    $value = '41';
    $alias =& $value;
    try {
        w11p_strict_reference_leaf($value);
    } catch (TypeError) {
        return $value === '41' ? 1 : -1;
    }
    return 0;
}
PHP,
        'w11p_strict_reference_root',
        1,
    ],
];

foreach ($cases as $label => [$source, $function, $expected]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w11p-inline-typed-by-reference-{$label}.php",
        [],
        [
            'wave' => 11,
            'function' => $function,
            'repeat' => 10,
        ],
    );
    $execution = $result['execution'];
    $performance = $execution['performance'];
    printf(
        "%s %s return=%d expected=%d direct=%d inner_helpers=%d vm=%d execute_ex=%d handler=%d active=%d\n",
        $label,
        $result['status'],
        $execution['return_value'],
        $expected,
        $performance['direct_call_frame_bytes'] > 0,
        $performance['inner_call_runtime_helper_calls'],
        $execution['vm_handler_calls'],
        $execution['execute_ex_calls'],
        $execution['opline_handler_calls'],
        $execution['entry_active_calls'],
    );
}
?>
--EXPECT--
bool-fast accepted return=1 expected=1 direct=1 inner_helpers=0 vm=0 execute_ex=0 handler=0 active=0
weak-coercion accepted return=141 expected=141 direct=1 inner_helpers=0 vm=0 execute_ex=0 handler=0 active=0
strict-mismatch accepted return=1 expected=1 direct=1 inner_helpers=0 vm=0 execute_ex=0 handler=0 active=0

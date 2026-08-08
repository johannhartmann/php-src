--TEST--
Native boxed loop assignments retain complete multi-part PHI definitions
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
function loop_add_assign(mixed $value, mixed $step, int $count): mixed
{
    for ($index = 0; $index < $count; $index++) {
        $value += $step;
    }
    return $value;
}

function loop_or_assign(mixed $value, mixed $mask, int $count): mixed
{
    for ($index = 0; $index < $count; $index++) {
        $value |= $mask;
    }
    return $value;
}

function loop_and_assign(mixed $value, mixed $mask, int $count): mixed
{
    for ($index = 0; $index < $count; $index++) {
        $value &= $mask;
    }
    return $value;
}

function loop_xor_assign(mixed $value, mixed $mask, int $count): mixed
{
    for ($index = 0; $index < $count; $index++) {
        $value ^= $mask;
    }
    return $value;
}

function loop_add_assign_exception(mixed $value): string
{
    try {
        for ($index = 0; $index < 3; $index++) {
            $value += 1;
        }
    } catch (TypeError) {
        return 'type-error';
    }
    return 'missing-error';
}

function loop_add_assign_warning(int $count): array
{
    $warnings = 0;
    set_error_handler(static function () use (&$warnings): void {
        $warnings++;
    });
    try {
        for ($index = 0; $index < $count; $index++) {
            $value += 2;
        }
    } finally {
        restore_error_handler();
    }
    return [$value, $warnings];
}
PHP;

$cases = [
    ['add-entry', 'loop_add_assign', [5, 3, 0], null],
    ['add-backedge', 'loop_add_assign', [5, 3, 4], null],
    ['or-backedge', 'loop_or_assign', [1, 6, 3], null],
    ['and-backedge', 'loop_and_assign', [15, 10, 3], null],
    ['xor-backedge', 'loop_xor_assign', [5, 3, 3], null],
    ['xor-string', 'loop_xor_assign', ["\x0f\xf0", "\x03\x0c", 3], 'hex'],
    ['add-exception', 'loop_add_assign_exception', [[]], null],
    ['add-warning', 'loop_add_assign_warning', [3], null],
];

foreach ($cases as [$label, $function, $arguments, $format]) {
    $result = native_mir_test_compile_execute(
        $source,
        "w14-boxed-loop-assign-{$label}.php",
        $arguments,
        [
            'wave' => 11,
            'function' => $function,
            'repeat' => 20,
        ],
    );
    $value = $result['execution']['return_value'];
    if ($format === 'hex') {
        $value = bin2hex($value);
    }
    printf(
        "%s %s=%s vm=%d execute_ex=%d handler=%d\n",
        $result['status'],
        $label,
        json_encode($value),
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
    );
}
?>
--EXPECT--
accepted add-entry=5 vm=0 execute_ex=0 handler=0
accepted add-backedge=17 vm=0 execute_ex=0 handler=0
accepted or-backedge=7 vm=0 execute_ex=0 handler=0
accepted and-backedge=10 vm=0 execute_ex=0 handler=0
accepted xor-backedge=6 vm=0 execute_ex=0 handler=0
accepted xor-string="0cfc" vm=0 execute_ex=0 handler=0
accepted add-exception="type-error" vm=0 execute_ex=0 handler=0
accepted add-warning=[6,1] vm=0 execute_ex=0 handler=0

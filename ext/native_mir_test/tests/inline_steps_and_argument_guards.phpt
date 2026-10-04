--TEST--
Native inline checked steps, scalar selects, and argument guards
--DESCRIPTION--
Checked chains whose accumulator is the right operand of a subtraction or
that overflow only on a later iteration, a bool scalar diamond, and typed
argument guards that pass while warming up and then fail, by value and by
reference. Every result must match the VM and run natively.
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$cases = [
    'right-accumulator' => [<<<'PHP'
<?php
function right_leaf(int $value, int $step): int
{
    return $step - ($value - 1);
}
function right_root(int $count): int
{
    $value = 0;
    for ($index = 0; $index < $count; $index++) {
        $value = right_leaf($value, 5);
    }
    return $value;
}
PHP, 'right_root', [7]],
    'late-overflow' => [<<<'PHP'
<?php
function late_leaf(int $value, int $step): int
{
    return (($value + $step) + $step) - 1;
}
function late_root(int $start): string
{
    $value = $start;
    $steps = 0;
    try {
        for ($index = 0; $index < 8; $index++) {
            $value = late_leaf($value, 1 << 61);
            $steps++;
        }
    } catch (TypeError) {
        return "overflow after $steps";
    }
    return "no overflow: $value";
}
PHP, 'late_root', [0]],
    'bool-diamond' => [<<<'PHP'
<?php
function bool_leaf(int $value): bool
{
    return $value < 3 ? true : false;
}
function bool_root(int $count): int
{
    $true = 0;
    for ($value = 0; $value < $count; $value++) {
        if (bool_leaf($value)) {
            $true++;
        }
    }
    return $true;
}
PHP, 'bool_root', [6]],
    'guard-after-warmup' => [<<<'PHP'
<?php
function guard_leaf(int $value): int
{
    return $value * 2;
}
function guard_root(): string
{
    $inputs = [1, 2, 3, "4", 5, "6"];
    $out = [];
    foreach ($inputs as $input) {
        $out[] = guard_leaf($input);
    }
    return implode(',', $out);
}
PHP, 'guard_root', []],
    'bool-reference-guard' => [<<<'PHP'
<?php
function flag_leaf(bool &$flag): int
{
    return $flag ? 1 : 0;
}
function flag_root(): string
{
    $values = [true, false, 1, 0, "yes", ""];
    $out = [];
    foreach ($values as $value) {
        $alias =& $value;
        $out[] = flag_leaf($value) . gettype($value);
        unset($alias);
    }
    return implode(',', $out);
}
PHP, 'flag_root', []],
];

foreach ($cases as $label => [$source, $function, $arguments]) {
    $result = native_mir_test_compile_execute(
        $source, "$label.php", $arguments,
        ['function' => $function, 'repeat' => 20],
    );
    $execution = $result['execution'] ?? [];
    $performance = $execution['performance'] ?? [];
    printf("%s %s return=%s direct=%d inline=%d typed=%d vm=%d handler=%d\n",
        $label, $result['status'], var_export($execution['return_value'] ?? null, true),
        $performance['direct_call_sites'] ?? -1, $performance['direct_leaf_scalar_sites'] ?? -1,
        $performance['direct_typed_body_sites'] ?? -1, $execution['vm_handler_calls'] ?? -1,
        $execution['opline_handler_calls'] ?? -1);
}
?>
--EXPECT--
right-accumulator accepted return=6 direct=1 inline=1 typed=0 vm=0 handler=0
late-overflow accepted return='overflow after 2' direct=1 inline=1 typed=0 vm=0 handler=0
bool-diamond accepted return=3 direct=1 inline=1 typed=0 vm=0 handler=0
guard-after-warmup accepted return='2,4,6,8,10,12' direct=1 inline=0 typed=0 vm=0 handler=0
bool-reference-guard accepted return='1boolean,0boolean,1boolean,0boolean,1boolean,0boolean' direct=1 inline=0 typed=0 vm=0 handler=0

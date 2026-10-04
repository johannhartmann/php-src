--TEST--
Native code executes nested and recursive user calls on Zend frames
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
function run_native(
    string $source,
    string $function,
    array $arguments,
    int $repeat = 1,
): void
{
    ob_start();
    $result = native_mir_test_compile_execute(
        $source,
        $function . '.php',
        $arguments,
        ['function' => $function, 'repeat' => $repeat],
    );
    $output = ob_get_clean();
    printf(
        "%s %s %s vm=%d execute_ex=%d handler=%d "
            . "output=%s return=%s\n",
        $function,
        $result['status'],
        $result['phase'],
        $result['execution']['vm_handler_calls'],
        $result['execution']['execute_ex_calls'],
        $result['execution']['opline_handler_calls'],
        $output === '' ? '<empty>' : $output,
        json_encode($result['execution']['return_value']),
    );
}

$nested = <<<'PHP'
<?php
function nested_inner(): int
{
    echo 1;
    return 7;
}

function nested_outer(int $value): void
{
    echo $value;
}

function nested_case(): void
{
    nested_outer(nested_inner());
}
PHP;

$recursive = <<<'PHP'
<?php
function recursive_case(bool $again): int
{
    if (!$again) {
        return 1;
    }
    return recursive_case(false);
}
PHP;

$mutual = <<<'PHP'
<?php
function is_even(bool $again): bool
{
    if (!$again) {
        return true;
    }
    return is_odd(false);
}

function is_odd(bool $again): bool
{
    if (!$again) {
        return false;
    }
    return is_even(false);
}

function mutual_case(): bool
{
    return is_even(true);
}
PHP;

$callInBranch = <<<'PHP'
<?php
function predicate(bool $value): bool
{
    return $value;
}

function branch_case(): int
{
    if (predicate(true)) {
        return 7;
    }
    return 9;
}
PHP;

$default = <<<'PHP'
<?php
function default_value(int $value = 7): int
{
    return $value;
}

function default_case(): int
{
    return default_value();
}
PHP;

$scalars = <<<'PHP'
<?php
function scalar_int(int $value): int
{
    return $value;
}

function scalar_bool(bool $value): bool
{
    return $value;
}

function scalar_float(float $value): float
{
    return $value;
}

function scalar_null(): null
{
    return null;
}

function scalar_null_side_effect(): null
{
    echo 1;
    return null;
}

function scalar_int_case(): int
{
    return scalar_int(4);
}

function scalar_bool_case(): bool
{
    return scalar_bool(true);
}

function scalar_float_case(): float
{
    return scalar_float(2.5);
}

function scalar_null_case(): null
{
    return scalar_null();
}

function scalar_null_side_effect_case(): null
{
    return scalar_null_side_effect();
}

function scalar_echo_case(): void
{
    echo scalar_int(4);
}
PHP;

$weakTypes = <<<'PHP'
<?php
function weak_target(int $value): int
{
    return $value;
}

function weak_case(): int
{
    return weak_target(true);
}
PHP;

$strictTypes = <<<'PHP'
<?php declare(strict_types=1);
function strict_target(int $value): int
{
    return $value;
}

function strict_case(): int
{
    return strict_target(true);
}
PHP;

$weakReturn = <<<'PHP'
<?php
function weak_return_case(): int
{
    return true;
}
PHP;

$strictReturn = <<<'PHP'
<?php declare(strict_types=1);
function strict_return_case(): int
{
    return true;
}
PHP;

$strictNumericReturn = <<<'PHP'
<?php declare(strict_types=1);
function strict_float_target(): float
{
    return 2;
}

function strict_float_case(): float
{
    return strict_float_target();
}
PHP;

run_native($nested, 'nested_case', []);
run_native($nested, 'nested_case', [], 3);
run_native($recursive, 'recursive_case', [true]);
run_native($mutual, 'mutual_case', [], 10);
run_native($callInBranch, 'branch_case', []);
run_native($default, 'default_case', []);
run_native($scalars, 'scalar_int_case', []);
run_native($scalars, 'scalar_bool_case', []);
run_native($scalars, 'scalar_float_case', []);
run_native($scalars, 'scalar_null_case', []);
run_native($scalars, 'scalar_null_side_effect_case', []);
run_native($scalars, 'scalar_echo_case', []);
run_native($weakTypes, 'weak_case', []);
run_native($weakReturn, 'weak_return_case', []);
try {
    run_native($strictTypes, 'strict_case', []);
} catch (TypeError $error) {
    printf("strict_case TypeError %s\n", $error->getMessage());
}
try {
    run_native($strictReturn, 'strict_return_case', []);
} catch (TypeError $error) {
    printf("strict_return_case TypeError %s\n", $error->getMessage());
}
run_native($strictNumericReturn, 'strict_float_case', []);
?>
--EXPECT--
nested_case accepted complete vm=0 execute_ex=0 handler=0 output=17 return=null
nested_case accepted complete vm=0 execute_ex=0 handler=0 output=171717 return=null
recursive_case accepted complete vm=0 execute_ex=0 handler=0 output=<empty> return=1
mutual_case accepted complete vm=0 execute_ex=0 handler=0 output=<empty> return=false
branch_case accepted complete vm=0 execute_ex=0 handler=0 output=<empty> return=7
default_case accepted complete vm=0 execute_ex=0 handler=0 output=<empty> return=7
scalar_int_case accepted complete vm=0 execute_ex=0 handler=0 output=<empty> return=4
scalar_bool_case accepted complete vm=0 execute_ex=0 handler=0 output=<empty> return=true
scalar_float_case accepted complete vm=0 execute_ex=0 handler=0 output=<empty> return=2.5
scalar_null_case accepted complete vm=0 execute_ex=0 handler=0 output=<empty> return=null
scalar_null_side_effect_case accepted complete vm=0 execute_ex=0 handler=0 output=1 return=null
scalar_echo_case accepted complete vm=0 execute_ex=0 handler=0 output=4 return=null
weak_case accepted complete vm=0 execute_ex=0 handler=0 output=<empty> return=1
weak_return_case accepted complete vm=0 execute_ex=0 handler=0 output=<empty> return=1
strict_case TypeError strict_target(): Argument #1 ($value) must be of type int, true given, called in strict_case.php on line 9
strict_return_case TypeError strict_return_case(): Return value must be of type int, true returned
strict_float_case accepted complete vm=0 execute_ex=0 handler=0 output=<empty> return=2

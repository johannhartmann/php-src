--TEST--
Native direct calls guard the stack against the callee's machine frame
--DESCRIPTION--
Many values live across a recursive direct call give the callee a real
machine frame. Self and mutual recursion must stop with Zend's stack limit
error rather than overflow the C stack, and the live values must survive
calls and the recovered overflow.
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--INI--
zend.max_allowed_stack_size=512K
zend.reserved_stack_size=64K
--FILE--
<?php
/* Many values live across a recursive direct call force real spill slots, so
 * the callee's machine frame is far larger than its return address. */
function wide_self(int $depth): int
{
    $v0 = $depth * 3 + 0;
    $v1 = $depth * 4 + 1;
    $v2 = $depth * 5 + 2;
    $v3 = $depth * 6 + 3;
    $v4 = $depth * 7 + 4;
    $v5 = $depth * 8 + 5;
    $v6 = $depth * 9 + 6;
    $v7 = $depth * 10 + 7;
    $v8 = $depth * 11 + 8;
    $v9 = $depth * 12 + 9;
    $v10 = $depth * 13 + 10;
    $v11 = $depth * 14 + 11;
    $v12 = $depth * 15 + 12;
    $v13 = $depth * 16 + 13;
    $v14 = $depth * 17 + 14;
    $v15 = $depth * 18 + 15;
    $v16 = $depth * 19 + 16;
    $v17 = $depth * 20 + 17;
    $v18 = $depth * 21 + 18;
    $v19 = $depth * 22 + 19;
    $v20 = $depth * 23 + 20;
    $v21 = $depth * 24 + 21;
    $v22 = $depth * 25 + 22;
    $v23 = $depth * 26 + 23;
    $inner = $depth > 0 ? wide_self($depth - 1) : 0;
    return $inner + $v0 + $v1 + $v2 + $v3 + $v4 + $v5 + $v6 + $v7 + $v8 + $v9 + $v10 + $v11 + $v12 + $v13 + $v14 + $v15 + $v16 + $v17 + $v18 + $v19 + $v20 + $v21 + $v22 + $v23 + strlen((string) $depth);
}

function wide_even(int $depth): int
{
    $v0 = $depth * 3 + 0;
    $v1 = $depth * 4 + 1;
    $v2 = $depth * 5 + 2;
    $v3 = $depth * 6 + 3;
    $v4 = $depth * 7 + 4;
    $v5 = $depth * 8 + 5;
    $v6 = $depth * 9 + 6;
    $v7 = $depth * 10 + 7;
    $v8 = $depth * 11 + 8;
    $v9 = $depth * 12 + 9;
    $v10 = $depth * 13 + 10;
    $v11 = $depth * 14 + 11;
    $v12 = $depth * 15 + 12;
    $v13 = $depth * 16 + 13;
    $v14 = $depth * 17 + 14;
    $v15 = $depth * 18 + 15;
    $v16 = $depth * 19 + 16;
    $v17 = $depth * 20 + 17;
    $v18 = $depth * 21 + 18;
    $v19 = $depth * 22 + 19;
    $v20 = $depth * 23 + 20;
    $v21 = $depth * 24 + 21;
    $v22 = $depth * 25 + 22;
    $v23 = $depth * 26 + 23;
    $inner = $depth > 0 ? wide_odd($depth - 1) : 0;
    return $inner + $v0 + $v1 + $v2 + $v3 + $v4 + $v5 + $v6 + $v7 + $v8 + $v9 + $v10 + $v11 + $v12 + $v13 + $v14 + $v15 + $v16 + $v17 + $v18 + $v19 + $v20 + $v21 + $v22 + $v23;
}

function wide_odd(int $depth): int
{
    $v0 = $depth * 3 + 0;
    $v1 = $depth * 4 + 1;
    $v2 = $depth * 5 + 2;
    $v3 = $depth * 6 + 3;
    $v4 = $depth * 7 + 4;
    $v5 = $depth * 8 + 5;
    $v6 = $depth * 9 + 6;
    $v7 = $depth * 10 + 7;
    $v8 = $depth * 11 + 8;
    $v9 = $depth * 12 + 9;
    $v10 = $depth * 13 + 10;
    $v11 = $depth * 14 + 11;
    $v12 = $depth * 15 + 12;
    $v13 = $depth * 16 + 13;
    $v14 = $depth * 17 + 14;
    $v15 = $depth * 18 + 15;
    $v16 = $depth * 19 + 16;
    $v17 = $depth * 20 + 17;
    $v18 = $depth * 21 + 18;
    $v19 = $depth * 22 + 19;
    $v20 = $depth * 23 + 20;
    $v21 = $depth * 24 + 21;
    $v22 = $depth * 25 + 22;
    $v23 = $depth * 26 + 23;
    $inner = $depth > 0 ? wide_even($depth - 1) : 0;
    return $inner - ($v0 + $v1 + $v2 + $v3 + $v4 + $v5 + $v6 + $v7 + $v8 + $v9 + $v10 + $v11 + $v12 + $v13 + $v14 + $v15 + $v16 + $v17 + $v18 + $v19 + $v20 + $v21 + $v22 + $v23);
}

var_dump(wide_self(20));
var_dump(wide_even(21));

foreach (['wide_self', 'wide_even'] as $function) {
    try {
        $function(1000000);
        echo "$function: no overflow\n";
    } catch (Error $error) {
        echo "$function: ", preg_replace('/\d+ bytes/', 'N bytes', $error->getMessage()), "\n";
    }
}
var_dump(wide_self(3));
?>
--EXPECT--
int(78908)
int(3828)
wide_self: Maximum call stack size of N bytes (zend.max_allowed_stack_size - zend.reserved_stack_size) reached. Infinite recursion?
wide_even: Maximum call stack size of N bytes (zend.max_allowed_stack_size - zend.reserved_stack_size) reached. Infinite recursion?
int(3196)

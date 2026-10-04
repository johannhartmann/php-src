--TEST--
Native scalar assignments publish through global reference cells
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
$native_global_scalar = false;

function native_write_global_scalar(): void
{
    global $native_global_scalar;
    $native_global_scalar = true;
}

function native_read_global_scalar(): bool
{
    global $native_global_scalar;
    return $native_global_scalar;
}

native_write_global_scalar();
var_dump($native_global_scalar, native_read_global_scalar());
?>
--EXPECT--
bool(true)
bool(true)

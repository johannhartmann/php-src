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
$w11p_global_scalar = false;

function w11p_write_global_scalar(): void
{
    global $w11p_global_scalar;
    $w11p_global_scalar = true;
}

function w11p_read_global_scalar(): bool
{
    global $w11p_global_scalar;
    return $w11p_global_scalar;
}

w11p_write_global_scalar();
var_dump($w11p_global_scalar, w11p_read_global_scalar());
?>
--EXPECT--
bool(true)
bool(true)

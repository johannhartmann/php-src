--TEST--
Native executor rejects stale code for recycled partial applications
--SKIPIF--
<?php
if (!function_exists('native_mir_test_compile_execute')) {
    die('skip native_mir_test is not available');
}
?>
--FILE--
<?php
function w11p_recycled_partial_target($first, $second, ...$rest): int
{
    return func_num_args();
}

var_dump(w11p_recycled_partial_target(1, ?)(2, 'ignored'));
var_dump(w11p_recycled_partial_target(1, ?, ...)(2, 'forwarded'));
var_dump(w11p_recycled_partial_target(1, ?)(2, 'ignored'));
var_dump(w11p_recycled_partial_target(1, ?, ...)(2, 'forwarded'));
?>
--EXPECT--
int(2)
int(3)
int(2)
int(3)

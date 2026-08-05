--TEST--
Native reference assignment does not register live temporary operands as GC roots
--SKIPIF--
<?php
if (!extension_loaded('native_mir_test')) {
    die('skip native_mir_test extension not available');
}
?>
--INI--
zend.enable_gc=1
--FILE--
<?php
$array = [];
$array[0] = [[]];
$array[0][0] = &$array[0];

$status = gc_status();
var_dump($status['roots']);

unset($array);
$status = gc_status();
var_dump($status['roots']);
var_dump(gc_collect_cycles());
?>
--EXPECT--
int(0)
int(1)
int(1)

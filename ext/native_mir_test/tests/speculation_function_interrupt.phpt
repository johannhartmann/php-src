--TEST--
Native: a specialized function leaves for its generic copy on an interrupt
--DESCRIPTION--
A specialized function's loop calls no interrupt handler: a pending
interrupt leaves at the poll for the generic copy, which handles it at its
next poll. The time limit ends the script as in the VM and the shutdown
function runs.
--SKIPIF--
<?php
if (getenv("SKIP_SLOW_TESTS")) die("skip slow test");
?>
--EXTENSIONS--
opcache
--ENV--
ZEND_NATIVE_SPECULATE=1
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
max_execution_time=1
--FILE--
<?php
function spin(array $a) {
    $s = 0;
    for ($i = 0; $i < PHP_INT_MAX; $i++) {
        $s = ($s + $a[$i & 7]) & 0xFFFF;
    }
    return $s;
}
register_shutdown_function(function () { echo "shutdown\n"; });
$a = []; for ($i = 0; $i < 8; $i++) { $a[] = $i; }
echo spin($a), "\n";
?>
--EXPECTF--
Fatal error: Maximum execution time of 1 second exceeded in %s on line %d
shutdown

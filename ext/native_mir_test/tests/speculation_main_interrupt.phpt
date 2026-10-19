--TEST--
Native: a specialized main script leaves for its generic copy on an interrupt
--DESCRIPTION--
A pending interrupt, whose handler could change globals the specialized
main script keeps in registers, leaves at the loop's interrupt poll for
the generic copy, which handles it at its next poll: the time limit ends
the script as in the VM.
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
$s = 0;
for ($i = 0; $i < PHP_INT_MAX; $i++) {
    $s = ($s + $i) & 0xFFFF;
}
echo $s, "\n";
?>
--EXPECTF--
Fatal error: Maximum execution time of 1 second exceeded in %s on line %d

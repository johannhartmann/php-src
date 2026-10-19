--TEST--
Native: a timeout interrupts a loop in a typed body
--DESCRIPTION--
The typed body's interrupt poll leaves the body while the timeout is
pending; the call runs again through its Zend entry, whose poll raises the
fatal error.
--EXTENSIONS--
opcache
--SKIPIF--
<?php
if (getenv("SKIP_SLOW_TESTS")) die("skip slow test");
?>
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function spin(int $n) {
    $x = $n;
    while (true) {
        $x = ($x + 1) & 0xFF;
    }
    return $x;
}
function outer() {
    return spin(1);
}
set_time_limit(1);
echo "start\n";
echo outer(), "\n";
?>
--EXPECTF--
start

Fatal error: Maximum execution time of 1 second exceeded in %s on line %d

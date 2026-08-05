--TEST--
Observer: Recursive calls preserve their arguments
--EXTENSIONS--
zend_test
opcache
--INI--
zend_test.observer.enabled=1
zend_test.observer.show_output=0
zend_test.observer.observe_all=1
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function ackermann(int $m, int $n): int
{
    if ($m === 0) {
        return $n + 1;
    }
    if ($n === 0) {
        return ackermann($m - 1, 1);
    }
    return ackermann($m - 1, ackermann($m, $n - 1));
}

var_dump(ackermann(3, 3));
?>
--EXPECT--
int(61)

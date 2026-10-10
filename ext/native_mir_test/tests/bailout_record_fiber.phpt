--TEST--
Native: a fatal error in a fiber unwinds the entries from C inside it
--DESCRIPTION--
Callbacks entered from C inside a fiber are unwound by their bailout records before the jump to the fiber's catcher; the main context's chain resumes and the shutdown function runs callbacks again.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
register_shutdown_function(function () {
    echo "shutdown ", implode(',', array_map(fn($x) => $x * 3, [1, 2])), "\n";
});
$f1 = new Fiber(function () { $v = Fiber::suspend(array_map(fn($x) => $x + 1, [1])); echo "f1 got $v\n"; });
$f2 = new Fiber(function () {
    array_map(function ($x) { Fiber::suspend($x); return array_map(function ($y) { trigger_error("fatal in fiber", E_USER_ERROR); }, [1]); }, [5]);
});
var_dump($f1->start());
var_dump($f2->start());
$f1->resume('one');
$f2->resume();
echo "not reached\n";
?>
--EXPECTF--
array(1) {
  [0]=>
  int(2)
}
int(5)
f1 got one

Deprecated: Passing E_USER_ERROR to trigger_error() is deprecated since 8.4, throw an exception or call exit with a string message instead in %s on line 7

Fatal error: fatal in fiber in %s on line 7
shutdown 3,6

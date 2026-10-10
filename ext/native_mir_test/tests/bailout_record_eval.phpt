--TEST--
Native: a fatal error compiling eval code inside a callback
--DESCRIPTION--
The include/eval state is released by its bailout record before the jump; shutdown code evaluates again.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
register_shutdown_function(function () { echo "shutdown ", eval('return 42;'), "\n"; });
echo eval('return array_sum(array_map(fn($x) => $x, [1, 2, 3]));'), "\n";
array_map(function () { eval('class EvalDup {} class EvalDup {}'); }, [1]);
?>
--EXPECTF--
6

Fatal error: Cannot redeclare class EvalDup (previously declared in %s(4) : eval()'d code:1) in %s(4) : eval()'d code on line 1
shutdown 42

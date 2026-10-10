--TEST--
Native: a fatal error with a pending exception passes the entry from C
--DESCRIPTION--
A bailout while an exception is pending inside a callback continues as a bailout, as the VM does; shutdown functions still enter native code.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
// A bailout while an exception is pending, inside a callback.
class T { function __toString(): string { trigger_error("fatal in toString", E_USER_ERROR); } }
register_shutdown_function(function () { echo "shutdown ok ", count(array_filter([1, 0, 2], fn($x) => $x)), "\n"; });
try {
    array_map(function ($x) { try { throw new Exception("pending"); } finally { echo (string) new T; } }, [1]);
} catch (Exception $e) { echo "caught ", $e->getMessage(), "\n"; }
?>
--EXPECTF--
Deprecated: Passing E_USER_ERROR to trigger_error() is deprecated since 8.4, throw an exception or call exit with a string message instead in %s on line 3

Fatal error: fatal in toString in %s on line 3
shutdown ok 2

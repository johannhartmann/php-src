--TEST--
Native internal calls release named arguments an internal function rejects
--DESCRIPTION--
Extra named arguments of an internal call are destroyed when the call throws, before the next statement, as DO_ICALL releases them.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class D { public $n; function __construct($n) { $this->n = $n; } function __destruct() { echo "~{$this->n}|"; } }
function run($i) {
    try { $r = sprintf("%s", x: new D("a$i")); } catch (Error $e) { echo $e->getMessage(), "|"; }
    try { $r = sprintf("%s", "p", x: "s$i"); } catch (Error $e) { echo $e->getMessage(), "|"; }
    try { sprintf("%s", "p", x: new D("b$i")); } catch (Error $e) { echo $e->getMessage(), "|"; }
    try { $r = max(1, 2, y: new D("c$i")); } catch (Error $e) { echo $e->getMessage(), "|"; }
    try {
        try { $r = array_push($arr, y: new D("d$i")); } finally { echo "fin|"; }
    } catch (Error $e) { echo $e->getMessage(), "|"; }
    echo "\n";
}
run(1);
run(2);
echo "done\n";
--EXPECT--
Internal function sprintf() does not accept named variadic arguments|~a1|Internal function sprintf() does not accept named variadic arguments|Internal function sprintf() does not accept named variadic arguments|~b1|Internal function max() does not accept named variadic arguments|fin|~c1|array_push(): Argument #1 ($array) must be of type array, null given|
~d1|Internal function sprintf() does not accept named variadic arguments|~a2|Internal function sprintf() does not accept named variadic arguments|Internal function sprintf() does not accept named variadic arguments|~b2|Internal function max() does not accept named variadic arguments|fin|~c2|array_push(): Argument #1 ($array) must be of type array, null given|
~d2|done

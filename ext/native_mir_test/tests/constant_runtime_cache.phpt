--TEST--
Native x64 constant fetches use the runtime cache
--DESCRIPTION--
Like the VM, a found non-deprecated constant is cached per opline; undefined constants keep their error.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
define("IM", 139968); define("S", "str"); define("F", 1.5);
function g() { return IM + F . S; }
for ($i = 0; $i < 3; $i++) var_dump(g());
function u() { return UNDEF_CONST_X; }
try { u(); } catch (Error $e) { echo $e->getMessage(), "\n"; }
define("UNDEF_CONST_X", 5); var_dump(u(), u());
?>
--EXPECTF--
string(11) "139969.5str"
string(11) "139969.5str"
string(11) "139969.5str"
Undefined constant "UNDEF_CONST_X"
int(5)
int(5)

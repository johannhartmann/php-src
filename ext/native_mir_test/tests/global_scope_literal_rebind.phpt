--TEST--
Native global-scope literal assignments stay observable to calls
--DESCRIPTION--
A global-scope CV lives in the symbol table, so any call can modify or rebind
it through global, $GLOBALS or a reference. A literal array or string
assigned to it, and an exact scalar, must be read back from the frame after
such a call rather than from the register that held the literal.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
$a = [1, 2];
function set_elem() { global $a; $a[0] = 10; }
set_elem();
var_dump($a[0], count($a));

$b = [1, 2];
function rebind() { global $b; $b = [5, 6, 7]; }
rebind();
var_dump($b[1], count($b));

$c = [1];
function via_globals() { $GLOBALS["c"][0] = 3; }
via_globals();
var_dump($c[0]);

$s = "ab";
function grow() { global $s; $s = "cdef"; }
grow();
var_dump(strlen($s), $s[3]);

$n = 1;
function retype() { global $n; $n = "x"; }
retype();
var_dump($n);

$T = [9869608];
function bump() { global $T; $T[0] += 1; }
for ($q = 0; $q < 2; $q++) { bump(); }
var_dump($T[0] / 40e3);

function dynamic() { $d = [1]; $name = "d"; $$name = [4]; var_dump($d[0]); }
dynamic();
?>
--EXPECT--
int(10)
int(2)
int(6)
int(3)
int(3)
int(4)
string(1) "f"
string(1) "x"
float(246.74025)
int(4)

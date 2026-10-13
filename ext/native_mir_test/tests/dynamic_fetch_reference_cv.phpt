--TEST--
A variable-variable read of a CV bound to a reference copies the referenced value
--FILE--
<?php
function byref(&$p) { $n = 'p'; return $$n; }
function viaglobal() { global $gv; $n = 'gv'; return $$n; }
function viastatic() { static $s = 1; $n = 's'; $r = $$n; $s++; return $r; }
function local() { $v = 1; $alias = &$v; $n = 'v'; $arr = [$$n]; $v = 9; return $arr[0]; }
$gv = 'orig';
for ($i = 0; $i < 3; $i++) {
    $c = 1; $r = byref($c); $c = 7; var_dump($r);
    $g = viaglobal(); $gv = 'changed'; var_dump($g); $gv = 'orig';
    var_dump(viastatic() > 0, local());
}
?>
--EXPECT--
int(1)
string(4) "orig"
bool(true)
int(1)
int(1)
string(4) "orig"
bool(true)
int(1)
int(1)
string(4) "orig"
bool(true)
int(1)

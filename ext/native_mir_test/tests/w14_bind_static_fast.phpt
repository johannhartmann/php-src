--TEST--
Native BIND_STATIC of a bound static variable
--DESCRIPTION--
static $x; binds the CV to the static variable's reference without
decoding the operation once the variable is bound. Initializers, constant
expressions, methods and a CV that already holds a value keep stock
semantics.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function counter() { static $n = 0; return ++$n; }
function cache($k, $v = null) { static $c = []; if ($v !== null) { $c[$k] = $v; } return $c[$k] ?? null; }
function lazy() { static $x = PHP_INT_SIZE * 2; return $x; }
function preset() { $s = str_repeat('a', 2); static $s; return $s; }
class K { function m() { static $calls = 0; return ++$calls; } }
$out = [];
for ($i = 0; $i < 3; $i++) { $out[] = [counter(), cache("k$i", $i), cache('k0'), lazy(), preset(), (new K)->m()]; }
echo json_encode($out), "\n";
?>
--EXPECT--
[[1,0,0,16,null,1],[2,1,0,16,null,2],[3,2,0,16,null,3]]

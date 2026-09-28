--TEST--
Native x64 typed bodies with nested direct calls
--DESCRIPTION--
A typed body may contain nested direct user calls when every call in it
becomes a typed component call, so an inner result feeds the outer argument
in registers. Bodies with calls that stay phased keep the Zend entry.
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
function add(int $a, int $b): int { return $a + $b; }
function twice(int $x): int { return add(add($x, 1), add($x, 2)); }
function sq(float $x): float { return $x * $x; }
function hyp(float $a, float $b): float { return sqrt(sq($a) + sq($b)); }
function ident($x) { return $x; }
function wrap($x) { return ident(ident($x)); }
class C { function m($x) { return $x; } }
function mixed_nest($x) { return ident((new C)->m(ident($x))); }
for ($i = 0; $i < 2; $i++) {
    var_dump(twice(5), hyp(3.0, 4.0), wrap("s"), wrap([1]), mixed_nest(7));
}
?>
--EXPECT--
int(13)
float(5)
string(1) "s"
array(1) {
  [0]=>
  int(1)
}
int(7)
int(13)
float(5)
string(1) "s"
array(1) {
  [0]=>
  int(1)
}
int(7)

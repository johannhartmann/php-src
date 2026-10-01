--TEST--
Native call-site fast path for new of a class without a constructor
--DESCRIPTION--
new C of a literal class without a constructor and without arguments
creates the object in the fast Init and calls nothing; arguments to such a
new are still evaluated, and abstract classes still throw.
--EXTENSIONS--
opcache
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_update_protection=0
--FILE--
<?php
class Hook { public $callbacks = []; public $n = 3; }
abstract class Shape {}
function side($x) { echo "side$x "; return $x; }
function make() { $hooks = []; for ($i = 0; $i < 4; $i++) { $h = new Hook; $h->n += $i; $hooks[] = $h; } return $hooks; }
for ($r = 0; $r < 3; $r++) {
    echo implode(',', array_map(fn($h) => $h->n, make())), ' ', get_class(new Hook), ' ', (new Hook)->n, ' ';
    $o = new Hook(side($r));
    echo $o->n, "\n";
}
try { new Shape; } catch (Error $e) { echo get_class($e), "\n"; }
?>
--EXPECT--
3,4,5,6 Hook 3 side0 3
3,4,5,6 Hook 3 side1 3
3,4,5,6 Hook 3 side2 3
Error
